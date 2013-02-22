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
	bool request_sent;
	struct timespec last_ping_ts;
	struct timespec last_reply;

	void (*on_death)(struct disk_t *disk);
} disk_t;

void disk_init(disk_t *disk, const char *dev);
void disk_cleanup(disk_t *disk);
void disk_tur(disk_t *disk);

#endif
