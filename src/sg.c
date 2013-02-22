#include "sg.h"
#include <scsi/sg.h>
#include <memory.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <errno.h>

static void sg_cb(struct ev_loop *loop, ev_io *io, int revents);

bool sg_init(sg_t *sg, struct ev_loop *loop, const char *sg_path)
{
	memset(sg, 0, sizeof(*sg));

	int sg_fd = open(sg_path, O_RDWR|O_CLOEXEC);
	if (sg_fd < 0)
		return false;

	ev_io *io = &sg->io;
	ev_io_init(io, sg_cb, sg_fd, EV_READ);
	ev_io_start(loop, io);

	return true;
}

void sg_close(sg_t *sg, struct ev_loop *loop)
{
	ev_io_stop(loop, &sg->io);
	close(sg->io.fd);
}

bool sg_request(sg_t *sg, sg_request_t *req, sg_callback cb, unsigned char *cdb,
				   char cdb_len, int dxfer_dir, void *buf, unsigned int buf_len,
				   unsigned int timeout)
{
	assert(cb);

	sg_io_hdr_t hdr;
	memset(&hdr, 0, sizeof(hdr));
	hdr.interface_id = 'S';
	hdr.dxfer_direction = dxfer_dir;
	hdr.cmd_len = cdb_len;
	hdr.mx_sb_len = sizeof(req->sense);
	hdr.dxfer_len = buf_len;
	hdr.dxferp = buf;
	hdr.cmdp = cdb;
	hdr.sbp = req->sense;
	hdr.timeout = timeout;
	hdr.flags = SG_FLAG_LUN_INHIBIT;
	hdr.pack_id = 0;
	hdr.usr_ptr = req;

	req->cb = cb;
	ssize_t ret = write(sg->io.fd, &hdr, sizeof(hdr));
	if (ret == sizeof(hdr)) {
		req->in_progress = true;
		req->start = ev_now(EV_DEFAULT);
		return true;
	} else {
		if (errno == EWOULDBLOCK || errno == EAGAIN) {
			printf("Failed to submit io, would block.\n");
			return true;
		} else {
			printf("Failed to submit io: %m\n");
		}
		return false;
	}
}

static void sg_cb(struct ev_loop *loop, ev_io *io, int revents)
{
	sg_request_t *req;
	sg_io_hdr_t hdr;

	if (revents != EV_READ) {
		printf("WARN: sg event %d\n", revents);
	}

	if (read(io->fd, &hdr, sizeof(hdr)) == sizeof(hdr)) {
			req = (sg_request_t *)hdr.usr_ptr;
			req->end = ev_now(loop);
			req->in_progress = false;
			req->cb(req, hdr.status, hdr.masked_status,
					hdr.msg_status, hdr.sb_len_wr, hdr.host_status,
					hdr.driver_status, hdr.resid, hdr.duration, hdr.info);
	}
}
