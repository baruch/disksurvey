#ifndef DISK_SCANNER_H
#define DISK_SCANNER_H

#include "sg.h"
#include "scsicmd.h"
#include "util.h"
#include "src/disk_def.h"

#include <time.h>
#include <ev.h>
#include <stdbool.h>

typedef struct disk_scanner_t disk_scanner_t;

typedef void (*scanner_done_cb)(disk_scanner_t *scanner);

struct disk_scanner_t {
	char sg_path[32];
    disk_info_t disk_info;

	sg_t sg;
	sg_request_t data_request;
	char data_buf[4096] __attribute__(( aligned(4096) ));
    scanner_done_cb done_cb;
    bool is_active;
    bool success;
};

bool disk_scanner_active(disk_scanner_t *disk_scanner);
void disk_scanner_inquiry(disk_scanner_t *disk, const char *sg_dev, scanner_done_cb done_cb);

#endif
