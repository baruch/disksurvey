#include "disk.h"
#include "util.h"

#include <scsi/sg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <memory.h>

#define DEF_TIMEOUT 30*1000

static void disk_reply(sg_request_t *req, unsigned char status, unsigned char masked_status,
				unsigned char msg_status, char sb_len_wr, short int host_status,
				short int driver_status, int residual_len, int duration_msec, int info)
{
	//disk_t *disk = container_of(req, disk_t, tur_request);

	printf("Got reply in %f msecs (%d in sg)\n", 1000.0*(req->end-req->start), duration_msec);
}

void disk_tur(disk_t *disk)
{
	if (!disk->tur_request.in_progress) {
		unsigned char cdb[6];
		memset(cdb, 0, sizeof(cdb));
		bool sent = sg_request(&disk->sg, &disk->tur_request, disk_reply, cdb, sizeof(cdb), SG_DXFER_NONE, NULL, 0, DEF_TIMEOUT);
		printf("request sent %d\n", sent);
	} else {
		printf("TUR request on the air still!\n");
	}
}

void disk_init(disk_t *disk, const char *dev)
{
	memset(disk, 0, sizeof(*disk));
	strcpy(disk->sg_path, dev);

	if (!sg_init(&disk->sg, EV_DEFAULT, disk->sg_path)) {
		printf("Failed to access disk %s: %m\n", disk->sg_path);
		return;
	}
}
