#ifndef DISK_SCANNER_H
#define DISK_SCANNER_H

#include "sg.h"
#include "scsicmd.h"
#include "util.h"
#include "src/disk_def.h"

#include <time.h>
#include <stdbool.h>

typedef struct disk_scanner_t disk_scanner_t;

struct disk_scanner_t {
	char sg_path[32];
	disk_info_t disk_info;

	sg_t sg;
	sg_request_t data_request;
	char data_buf[512] __attribute__(( aligned(4096) ));
};

bool disk_scanner_inquiry(disk_scanner_t *disk, const char *sg_dev);

#endif
