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
#include <stdio.h>

typedef struct disk_t {
	char sg_path[32];
	sg_t sg;
	sg_request_t tur_request;
	sg_request_t data_request;

	unsigned pending_sata_smart_result : 1;

	ev_tstamp last_ping_ts;
	ev_tstamp last_reply_ts;
	ev_tstamp last_monitor_ts;

	void (*on_death)(struct disk_t *disk);

	char data_buf[4096] __attribute__(( aligned(4096) ));
	disk_info_t disk_info;
	latency_t latency;
} disk_t;

void disk_init(disk_t *disk, disk_info_t *disk_info, const char *dev);
void disk_stop(disk_t *disk);
void disk_cleanup(disk_t *disk);
void disk_tick(disk_t *disk);
void disk_tur(disk_t *disk);
int disk_json(disk_t *disk, char *buf, int len);

#endif
