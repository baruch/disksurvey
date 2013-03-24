#ifndef DISK_H
#define DISK_H

#include "sg.h"
#include "scsicmd.h"
#include "latency.h"
#include "util.h"
#include "src/disk_def.h"

#include <time.h>
#include <ev.h>
#include <stdbool.h>

typedef struct disk_t {
	char sg_path[32];
	sg_t sg;
	sg_request_t tur_request;
	sg_request_t data_request;

	unsigned pending_inquiry : 1;
	unsigned pending_ata_identify : 1;
	unsigned pending_sata_smart_result : 1;

	tribool_e is_ata;

	ev_tstamp last_ping_ts;
	ev_tstamp last_reply_ts;
	ev_tstamp last_monitor_ts;

	void (*on_death)(struct disk_t *disk);

	char data_buf[4096] __attribute__(( aligned(4096) ));
	disk_info_t disk_info;
	latency_t latency;

} disk_t;

void disk_init(disk_t *disk, const char *dev);
void disk_cleanup(disk_t *disk);
void disk_tick(disk_t *disk);
void disk_tur(disk_t *disk);
void disk_inquiry(disk_t *disk);
int disk_json(disk_t *disk, char *buf, int len);

#endif
