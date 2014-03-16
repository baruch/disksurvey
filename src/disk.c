#include "disk.h"
#include "util.h"
#include "monoclock.h"

#include "scsicmd.h"
#include "ata.h"
#include "ata_parse.h"

#include <inttypes.h>
#include <scsi/sg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <memory.h>

#define DEF_TIMEOUT 30*1000
#define MONITOR_INTERVAL_SEC 3600

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

static bool sg_request_with_dir(disk_t *disk, unsigned char *cdb, int cdb_len, int xfer_dir)
{
	void *buf;
	unsigned buf_len;

	if (xfer_dir == SG_DXFER_NONE) {
		buf = NULL;
		buf_len = 0;
	} else {
		buf = disk->data_buf;
		buf_len = sizeof(disk->data_buf);
	}

	if (sg_request_submit(&disk->sg, &disk->request, cdb, cdb_len, xfer_dir, buf, buf_len, DEF_TIMEOUT) < 0) {
		printf("Failed to submit request for disk\n");
		return false;
	}

	if (sg_request_wait_response(&disk->sg, &disk->request) < 0) {
		printf("Failed to read request for disk\n");
		return false;
	}

	/*
	if (disk->request.hdr.status != 0) {
		printf("Request failed, status=%d\n", disk->request.hdr.status);
		return false;
	}
	*/

	return true;
}

static inline bool sg_request_nodata(disk_t *disk, unsigned char *cdb, int cdb_len)
{
	return sg_request_with_dir(disk, cdb, cdb_len, SG_DXFER_NONE);
}

static inline bool sg_request_data(disk_t *disk, unsigned char *cdb, int cdb_len)
{
	return sg_request_with_dir(disk, cdb, cdb_len, SG_DXFER_FROM_DEV);
}

static bool disk_do_tur(disk_t *disk)
{
	unsigned char cdb[32];
	unsigned cdb_len;

	if (disk->disk_info.disk_type == DISK_TYPE_ATA)
		cdb_len = cdb_ata_check_power_mode(cdb);
	else
		cdb_len = cdb_tur(cdb);

	bool alive = sg_request_nodata(disk, cdb, cdb_len);
	printf("request sent, alive: %s\n", alive ? "yes" : "no");
	if (!alive) {
		printf("Disk %p died\n", disk);
		return false;
	} else {
		disk->last_ping_ts = disk->request.start;
	}

	sg_request_t *req = &disk->request;
	double latency = req->end - req->start;

	printf("Got reply in %f msecs (%d in sg)\n", 1000.0*latency, req->hdr.duration);
	disk->last_reply_ts = req->end;

	latency_add_sample(&disk->latency, latency*1000.0);
	return true;
}

static bool disk_ata_smart_result(disk_t *disk)
{
	sg_request_t *req = &disk->request;
	unsigned char cdb[32];
	int cdb_len = cdb_ata_smart_return_status(cdb);
	bool alive = sg_request_data(disk, cdb, cdb_len);
	printf("ATA SMART RETURN RESULT request sent, alive: %s\n", alive? "yes" : "no");
	printf("Got ATA SMART RETURN RESULT reply in %f msecs (%d in sg)\n", 1000.0*(req->end-req->start), req->hdr.duration);
	if (!alive)
		return false;

	if (req->hdr.status == 0) {
		printf("ATA SMART RETURN RESULT succeeded but we expected it to always fail!\n");
		return true;
	}

	bool smart_ok;
	bool parsed = ata_smart_return_status_result(req->sense, req->hdr.sb_len_wr, &smart_ok);
	printf("parsing of smart return: %d\n", parsed);
	if (parsed) {
		disk->disk_info.ata.smart_ok = smart_ok;
	}
	return true;
}

static bool disk_monitor(disk_t *disk)
{
	uint64_t now = monoclock_get_seconds();

	printf("checking for monitoring last_monitor=%"PRIu64" now=%"PRIu64"\n", disk->last_monitor_ts, now);
	if (disk->last_monitor_ts + MONITOR_INTERVAL_SEC < now) {
		printf("Monitor initiated\n");
		disk->last_monitor_ts = now;
		if (disk->disk_info.disk_type == DISK_TYPE_ATA) {
			return disk_ata_smart_result(disk);
			//TODO: disk_ata_smart_attributes(disk);
		} else {
			//TODO: disk_informational_exception(disk);
		}
	} else {
		printf("Monitor skipped\n");
	}
	return true;
}

static bool disk_do_tick(disk_t *disk)
{
	latency_tick(&disk->latency);
	return disk_monitor(disk);
}

void disk_tick(disk_t *disk)
{
	disk->request_tick = 1;
	if (disk->active)
		wire_wait_resume(&disk->wait);
}

void disk_tur(disk_t *disk)
{
	disk->request_tur = 1;
	if (disk->active)
		wire_wait_resume(&disk->wait);
}

void disk_stop(disk_t *disk)
{
	if (disk->active)
		wire_wait_resume(&disk->wait);
	disk->active = 0;
}

static void disk_wire(void *arg)
{
	wire_wait_list_t wait_list;
	disk_t *disk = arg;

	if (!sg_init(&disk->sg, disk->sg_path)) {
		printf("Failed to access disk %s: %m\n", disk->sg_path);
		goto Exit;
	}

	wire_wait_list_init(&wait_list);
	wire_wait_init(&disk->wait);
	wire_wait_chain(&wait_list, &disk->wait);

	disk->active = 1;

	while (disk->active) {
		if (!disk->wait.triggered)
			wire_list_wait(&wait_list);
		wire_wait_reset(&disk->wait);

		if (disk->request_tur) {
			disk->request_tur = 0;
			disk->active = disk_do_tur(disk);
		}

		if (disk->request_tick) {
			disk->request_tick = 0;
			disk->active = disk_do_tick(disk);
		}
	}

	sg_close(&disk->sg);

Exit:
	memset(disk->sg_path, 0, sizeof(disk->sg_path));
	disk->on_death(disk);
}

bool disk_init(disk_t *disk, disk_info_t *disk_info, const char *dev, wire_pool_t *pool)
{
	memset(disk, 0, sizeof(*disk));
	strcpy(disk->sg_path, dev);
	memcpy(&disk->disk_info, disk_info, sizeof(disk_info_t));

	char name[32];
	snprintf(name, sizeof(name), "disk %s", disk->sg_path);
	wire_pool_alloc(pool, name, disk_wire, disk);

	return true;
}
