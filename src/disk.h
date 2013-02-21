#ifndef DISK_H
#define DISK_H

#include "sg.h"
#include <time.h>
#include <ev.h>
#include <stdbool.h>

typedef struct disk_conf_t {
	char sg_path[32];
} disk_conf_t;

typedef struct disk_state_t {
	disk_conf_t *conf;
	sg_t sg;
	sg_request_t tur_request;
	ev_timer timer;
	bool request_sent;
	struct timespec last_ping_ts;
	struct timespec last_reply;
} disk_state_t;

void disk_init(disk_state_t *disk, disk_conf_t *conf);

#endif
