#ifndef DISKSURVEY_SG_H
#define DISKSURVEY_SG_H

#include <ev.h>
#include <stdbool.h>

typedef struct sg_request_t sg_request_t;

typedef void (*sg_callback)(sg_request_t *req, unsigned char status, unsigned char masked_status, unsigned char msg_status, char sb_len_wr, short int host_status, short int driver_status, int residual_len, int duration_msec, int info);

struct sg_request_t {
	sg_callback cb;
	unsigned char sense[128];
	ev_tstamp start;
	ev_tstamp end;
	bool in_progress;
};

typedef struct sg {
	ev_io io;
} sg_t;

bool sg_init(sg_t *sg, struct ev_loop *loop, const char *sg_path);
void sg_close(sg_t *sg, struct ev_loop *loop);

bool sg_request(sg_t *sg, sg_request_t *req, sg_callback cb, unsigned char *cdb,
				   char cdb_len, int dxfer_dir, void *buf, unsigned int buf_len,
				   unsigned int timeout);

#endif
