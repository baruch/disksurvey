#include "disk.h"
#include "util.h"

#include "scsicmd.h"
#include "ata.h"
#include "ata_parse.h"

#include <scsi/sg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <memory.h>

#define DEF_TIMEOUT 30*1000
#define MONITOR_INTERVAL_SEC 3600

static void disk_state_machine_step(disk_t *disk);
static void disk_monitor(disk_t *disk);

inline const char *json_tribool(tribool_e state)
{
	if (state == TRIBOOL_TRUE)
		return "true";
	if (state == TRIBOOL_FALSE)
		return "false";
	else
		return "unknown";
}

inline const char *json_bool(bool is_true)
{
	return is_true ? "true" : "false";
}

int disk_json(disk_t *disk, char *buf, int len)
{
	int orig_len = len;

	buf_add_char(buf, len, ' ');
	buf_add_char(buf, len, '{');
	buf_add_str(buf, len, " \"dev\": \"%s\" ", disk->sg_path);
	buf_add_str(buf, len, ", \"vendor\": \"%s\"", disk->disk_info.vendor);
	buf_add_str(buf, len, ", \"model\": \"%s\"", disk->disk_info.model);
	buf_add_str(buf, len, ", \"serial\": \"%s\"", disk->disk_info.serial);
	buf_add_str(buf, len, ", \"fw_rev\": \"%s\"", disk->disk_info.fw_rev);

	bool smart_ok = true;

	switch (disk->disk_info.disk_type) {
		case DISK_TYPE_ATA:
			if (disk->disk_info.ata.smart_supported)
				smart_ok = disk->disk_info.ata.smart_ok;
			break;

		case DISK_TYPE_SAS:
			smart_ok = disk->disk_info.sas.smart_asc == 0 && disk->disk_info.sas.smart_ascq == 0;
			break;

		case DISK_TYPE_UNKNOWN:
			break;
	}

	buf_add_str(buf, len, ", \"smart_ok\": \"%s\"", json_bool(smart_ok));

	latency_summary_t *entry = &disk->latency.entries[disk->latency.cur_entry];

	double *top_latencies = entry->top_latencies;
	buf_add_str(buf, len, ", \"last_top_latency\": [%g,%g,%g,%g,%g]", top_latencies[0], top_latencies[1], top_latencies[2], top_latencies[3], top_latencies[4]);

	buf_add_str(buf, len, ", \"last_histogram\": [%u,%u,%u,%u,%u,%u,%u]", entry->hist[0], entry->hist[1], entry->hist[2], entry->hist[3], entry->hist[4], entry->hist[5], entry->hist[6]);

	buf_add_char(buf, len, '}');
	buf_add_char(buf, len, ' ');

	return orig_len - len;
}

static inline bool sg_request_data(disk_t *disk, sg_callback cb, unsigned char *cdb, int cdb_len)
{
	bool alive = sg_request(&disk->sg, &disk->data_request, cb, cdb, cdb_len, SG_DXFER_FROM_DEV, disk->data_buf, sizeof(disk->data_buf), DEF_TIMEOUT);
	if (!alive && disk->on_death) {
		printf("Disk %p died\n", disk);
		disk->on_death(disk);
	}
	return alive;
}

static void disk_tur_reply(sg_request_t *req, unsigned char status, unsigned char masked_status,
				unsigned char msg_status, char sb_len_wr, short int host_status,
				short int driver_status, int residual_len, int duration_msec, int info)
{
	disk_t *disk = container_of(req, disk_t, tur_request);
	double latency = req->end - req->start;

	printf("Got reply in %f msecs (%d in sg)\n", 1000.0*latency, duration_msec);
	disk->last_reply_ts = req->end;

	latency_add_sample(&disk->latency, latency*1000.0);
}

void disk_tur(disk_t *disk)
{
	if (!disk->tur_request.in_progress) {
		unsigned char cdb[32];
		unsigned cdb_len;

		if (disk->disk_info.disk_type == DISK_TYPE_ATA)
			cdb_len = cdb_ata_check_power_mode(cdb);
		else
			cdb_len = cdb_tur(cdb);
			
		bool alive = sg_request(&disk->sg, &disk->tur_request, disk_tur_reply, cdb, cdb_len, SG_DXFER_NONE, NULL, 0, DEF_TIMEOUT);
		printf("request sent, alive: %s\n", alive ? "yes" : "no");
		if (!alive && disk->on_death) {
			printf("Disk %p died\n", disk);
			disk->on_death(disk);
		} else {
			disk->last_ping_ts = disk->tur_request.start;
		}
	} else {
		printf("TUR request on the air still!\n");
	}
}


static void disk_ata_smart_result_reply(sg_request_t *req, unsigned char status, unsigned char masked_status,
				unsigned char msg_status, char sb_len_wr, short int host_status,
				short int driver_status, int residual_len, int duration_msec, int info)
{
	disk_t *disk = container_of(req, disk_t, data_request);
	printf("Got ATA SMART RETURN RESULT reply in %f msecs (%d in sg)\n", 1000.0*(req->end-req->start), duration_msec);

	if (status == 0) {
		printf("ATA SMART RETURN RESULT succeeded but we expected it to always fail!\n");
		disk_state_machine_step(disk);
		return;
	}

	bool smart_ok;
	bool parsed = ata_smart_return_status_result(req->sense, sb_len_wr, &smart_ok);
		printf("parsing of smart return: %d\n", parsed);
	if (parsed) {
		disk->disk_info.ata.smart_ok = smart_ok;
	}

	disk_state_machine_step(disk);
}

static void disk_ata_smart_result(disk_t *disk)
{
	if (disk->data_request.in_progress) {
		disk->pending_sata_smart_result = 1;
		return;
	}

	disk->pending_sata_smart_result = 0;

	unsigned char cdb[32];
	int cdb_len = cdb_ata_smart_return_status(cdb);
	bool alive = sg_request_data(disk, disk_ata_smart_result_reply, cdb, cdb_len);
	printf("ATA SMART RETURN RESULT request sent, alive: %s\n", alive? "yes" : "no");
	
}

static void disk_state_machine_step(disk_t *disk)
{
	if (disk->data_request.in_progress)
		return;

	if (disk->pending_sata_smart_result)
		disk_ata_smart_result(disk);
}

static void disk_monitor(disk_t *disk)
{
	ev_tstamp now = ev_now(EV_DEFAULT);

	printf("checking for monitoring last_monitor=%f now=%f\n", disk->last_monitor_ts, now);
	if (disk->last_monitor_ts + MONITOR_INTERVAL_SEC < now) {
		printf("Monitor initiated\n");
		disk->last_monitor_ts = now;
		if (disk->disk_info.disk_type == DISK_TYPE_ATA) {
			disk_ata_smart_result(disk);
			//TODO: disk_ata_smart_attributes(disk);
		} else {
			//TODO: disk_informational_exception(disk);
		}
	} else {
		printf("Monitor skipped\n");
	}
}

void disk_tick(disk_t *disk)
{
	latency_tick(&disk->latency);
	disk_monitor(disk);
}

void disk_cleanup(disk_t *disk)
{
	sg_close(&disk->sg, EV_DEFAULT);
	memset(disk->sg_path, 0, sizeof(disk->sg_path));
}

void disk_stop(disk_t *disk)
{
	if (disk->tur_request.in_progress)
		return;
	if (disk->data_request.in_progress)
		return;

	disk->on_death(disk);
}

void disk_init(disk_t *disk, disk_info_t *disk_info, const char *dev)
{
	memset(disk, 0, sizeof(*disk));
	strcpy(disk->sg_path, dev);
	memcpy(&disk->disk_info, disk_info, sizeof(disk_info_t));

	if (!sg_init(&disk->sg, EV_DEFAULT, disk->sg_path)) {
		printf("Failed to access disk %s: %m\n", disk->sg_path);
		return;
	}
}
