#ifndef DISK_H
#define DISK_H

#include "sg.h"
#include "scsicmd.h"
#include "latency.h"
#include "util.h"
#include "src/disk_def.h"
#include "wire_pool.h"
#include "wire_wait.h"

#include <stdbool.h>
#include <stdio.h>

typedef struct disk_t {
	wire_t *wire;
	wire_wait_t wait;
	char sg_path[32];
	sg_t sg;
	sg_request_t request;

	unsigned active : 1;
	unsigned request_tick : 1;
	unsigned request_tur : 1;

	uint64_t last_ping_ts;
	uint64_t last_reply_ts;
	uint64_t last_monitor_ts;

	void (*on_death)(struct disk_t *disk);

	char data_buf[4096] __attribute__(( aligned(4096) ));
	disk_info_t disk_info;
	latency_t latency;
} disk_t;

bool disk_init(disk_t *disk, disk_info_t *disk_info, const char *dev, wire_pool_t *pool);
void disk_stop(disk_t *disk);
void disk_tick(disk_t *disk);
void disk_tur(disk_t *disk);
int disk_json(disk_t *disk, char *buf, int len);

#endif
