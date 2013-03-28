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
	buf_add_str(buf, len, ", \"is_ata\": \"%s\"", json_tribool(disk->is_ata));
	buf_add_str(buf, len, ", \"ata_smart_supported\": %s", json_bool(disk->disk_info.ata_smart_supported));
	buf_add_str(buf, len, ", \"sata_smart_ok\": \"%s\"", json_tribool(disk->disk_info.sata_smart_ok));

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

		if (disk->is_ata)
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

static void disk_inquiry_reply(sg_request_t *req, unsigned char status, unsigned char masked_status,
				unsigned char msg_status, char sb_len_wr, short int host_status,
				short int driver_status, int residual_len, int duration_msec, int info)
{
	disk_t *disk = container_of(req, disk_t, data_request);
	printf("Got inquiry reply in %f msecs (%d in sg)\n", 1000.0*(req->end-req->start), duration_msec);

	if (status != 0) {
		printf("Inquiry failed, status=%d!\n", status);
		disk_state_machine_step(disk);
		return;
	}

	parse_inquiry(disk->data_buf, sizeof(disk->data_buf) - residual_len, &disk->disk_info.device_type, disk->disk_info.vendor,
	              disk->disk_info.model, disk->disk_info.fw_rev, disk->disk_info.serial);

	if (strcmp(disk->disk_info.vendor, "ATA     ") == 0) {
		// Disk is an ATA Disk, need to use ATA INQUIRY to get the real details
		printf("ATA disk needs to be ATA IDENTIFYied\n");
		disk->pending_ata_identify = 1;
		disk->is_ata = TRIBOOL_TRUE;
	} else {
		disk->is_ata = TRIBOOL_FALSE;
	}
	disk_monitor(disk);

	disk_state_machine_step(disk);
}

void disk_inquiry(disk_t *disk)
{
	if (disk->data_request.in_progress) {
		disk->pending_inquiry = 1;
		return;
	}

	disk->pending_inquiry = 0;

	unsigned char cdb[6];
	int cdb_len = cdb_inquiry_simple(cdb, sizeof(disk->data_buf));
	bool alive = sg_request_data(disk, disk_inquiry_reply, cdb, cdb_len);
	printf("Inquiry request sent, alive: %s\n", alive ? "yes" : "no");
}

static void disk_ata_identify_reply(sg_request_t *req, unsigned char status, unsigned char masked_status,
				unsigned char msg_status, char sb_len_wr, short int host_status,
				short int driver_status, int residual_len, int duration_msec, int info)
{
	disk_t *disk = container_of(req, disk_t, data_request);
	printf("Got ATA IDENTIFY reply in %f msecs (%d in sg)\n", 1000.0*(req->end-req->start), duration_msec);

	if (status != 0) {
		printf("ATA IDENTIFY failed, status=%d!\n", status);
		disk_state_machine_step(disk);
		return;
	}

	char ata_model[(46 - 27 + 1)*2 + 1] = "";

	ata_get_ata_identify_model(disk->data_buf, ata_model);

	char *vendor = strtok(ata_model, " ");
	strncpy(disk->disk_info.vendor, vendor, sizeof(disk->disk_info.vendor));

	char *model = strtok(NULL, " ");
	strncpy(disk->disk_info.model, model, sizeof(disk->disk_info.model));

	ata_get_ata_identify_serial_number(disk->data_buf, disk->disk_info.serial);
	ata_get_ata_identify_fw_rev(disk->data_buf, disk->disk_info.fw_rev);
	printf("ATA model: %s:%s\n", model, vendor);
	disk->disk_info.ata_smart_supported = ata_get_ata_identify_smart_supported(disk->data_buf);

	disk_state_machine_step(disk);
}

void disk_ata_identify(disk_t *disk)
{
	if (disk->data_request.in_progress) {
		disk->pending_ata_identify = 1;
		return;
	}

	disk->pending_ata_identify = 0;

	unsigned char cdb[32];
	int cdb_len = cdb_ata_identify(cdb);
	bool alive = sg_request_data(disk, disk_ata_identify_reply, cdb, cdb_len);
	printf("ATA identify request sent, alive: %s\n", alive? "yes" : "no");
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
		disk->disk_info.sata_smart_ok = smart_ok ? TRIBOOL_TRUE : TRIBOOL_FALSE;
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

	if (disk->pending_inquiry)
		disk_inquiry(disk);
	else if (disk->pending_ata_identify)
		disk_ata_identify(disk);
	else if (disk->pending_sata_smart_result)
		disk_ata_smart_result(disk);
}

static void disk_monitor(disk_t *disk)
{
	ev_tstamp now = ev_now(EV_DEFAULT);

	printf("checking for monitoring last_monitor=%f now=%f\n", disk->last_monitor_ts, now);
	if (disk->last_monitor_ts + MONITOR_INTERVAL_SEC < now) {
		printf("Monitor initiated\n");
		disk->last_monitor_ts = now;
		if (disk->is_ata == TRIBOOL_TRUE) {
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

void disk_init(disk_t *disk, const char *dev)
{
	memset(disk, 0, sizeof(*disk));
	strcpy(disk->sg_path, dev);

	if (!sg_init(&disk->sg, EV_DEFAULT, disk->sg_path)) {
		printf("Failed to access disk %s: %m\n", disk->sg_path);
		return;
	}

	disk_inquiry(disk);
}
