#ifndef DISK_H
#define DISK_H

#include "sg.h"
#include <time.h>
#include <ev.h>
#include <stdbool.h>

typedef struct disk_t {
	char sg_path[32];
	sg_t sg;
	sg_request_t tur_request;
	ev_timer timer;
	bool request_sent;
	struct timespec last_ping_ts;
	struct timespec last_reply;
} disk_t;

void disk_init(disk_t *disk, const char *dev);

#endif
