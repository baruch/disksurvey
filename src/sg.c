#include "sg.h"
#include "monoclock.h"
#include "wire_fd.h"
#include "wire_log.h"
#include "wire_io.h"

#include <memory.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <errno.h>

static int submit_request(sg_t *sg, sg_request_t *request)
{
	request->hdr.usr_ptr = request;
	request->start = monoclock_get();
	ssize_t ret = write(sg->sg_fd, &request->hdr, sizeof(request->hdr));
	if (ret == sizeof(request->hdr)) {
		return 0;
	}

	if (errno == EWOULDBLOCK || errno == EAGAIN) {
		wire_log(WLOG_WARNING, "Failed to submit io, would block.");
	} else {
		wire_log(WLOG_ERR, "Failed to submit io: %m\n");
	}
	return -1;
}

static void set_nonblock(int fd)
{
        int ret = fcntl(fd, F_GETFL);
        if (ret < 0)
                return;

        fcntl(fd, F_SETFL, ret | O_NONBLOCK);
}

bool sg_init(sg_t *sg, const char *sg_path)
{
	sg->sg_fd = wio_open(sg_path, O_RDWR|O_CLOEXEC, 0);
	if (sg->sg_fd < 0)
		return false;

	set_nonblock(sg->sg_fd);

	return true;
}

void sg_close(sg_t *sg)
{
	wio_close(sg->sg_fd);
	sg->sg_fd = -1;
}

int sg_request_submit(sg_t *sg, sg_request_t *req, unsigned char *cdb,
				   char cdb_len, int dxfer_dir, void *buf, unsigned int buf_len,
				   unsigned int timeout)
{
	memset(&req->hdr, 0, sizeof(req->hdr));
	req->hdr.interface_id = 'S';
	req->hdr.dxfer_direction = dxfer_dir;
	req->hdr.cmd_len = cdb_len;
	req->hdr.mx_sb_len = sizeof(req->sense);
	req->hdr.dxfer_len = buf_len;
	req->hdr.dxferp = buf;
	req->hdr.cmdp = cdb;
	req->hdr.sbp = req->sense;
	req->hdr.timeout = timeout;
	req->hdr.flags = SG_FLAG_LUN_INHIBIT;
	req->hdr.pack_id = 0;
	req->hdr.usr_ptr = req;

	return submit_request(sg, req);
}

int sg_request_wait_response(sg_t *sg, sg_request_t *req)
{
	int result = -1;
	wire_fd_state_t fd_state;

	wire_fd_mode_init(&fd_state, sg->sg_fd);

	while (1) {
		wire_fd_mode_read(&fd_state);
		wire_fd_wait(&fd_state);

		sg_io_hdr_t hdr;
		int ret = read(sg->sg_fd, &hdr, sizeof(hdr));
		req->end = monoclock_get();
		if (ret == sizeof(hdr)) {
			if (req != hdr.usr_ptr) {
				wire_log(WLOG_WARNING, "Unknown response received, waiting for the real one!");
				continue;
			}
			req->hdr = hdr;
			result = 0;
			break;
		} else if (ret < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				continue;
			else {
				wire_log(WLOG_WARNING, "Error while reading the data, bailing out: %m");
				break;
			}
		} else {
			wire_log(WLOG_ERR, "Didn't read the full data only read %d bytes, weird!", ret);
		}
	}

	wire_fd_mode_none(&fd_state);
	return result;
}
