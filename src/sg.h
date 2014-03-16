#ifndef DISKSURVEY_SG_H
#define DISKSURVEY_SG_H

#include <stdbool.h>
#include <scsi/sg.h>

typedef struct sg_request_t sg_request_t;

struct sg_request_t {
	sg_io_hdr_t hdr;
	double start;
	double end;
	unsigned char sense[128];
};

typedef struct sg {
	int sg_fd;
	sg_io_hdr_t hdr;
} sg_t;

bool sg_init(sg_t *sg, const char *sg_path);
void sg_close(sg_t *sg);

int sg_request_submit(sg_t *sg, sg_request_t *req, unsigned char *cdb,
				   char cdb_len, int dxfer_dir, void *buf, unsigned int buf_len,
				   unsigned int timeout);
int sg_request_wait_response(sg_t *sg, sg_request_t *req);

#endif
