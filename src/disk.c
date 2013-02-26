#include "disk.h"
#include "util.h"

#include "scsicmd.h"
#include "ata.h"
#include "ata_parse.h"

#include <scsi/sg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <memory.h>

#define DEF_TIMEOUT 30*1000

static void disk_state_machine_step(disk_t *disk);

int disk_json(disk_t *disk, char *buf, int len)
{
	int orig_len = len;

	buf_add_char(buf, len, ' ');
	buf_add_char(buf, len, '{');
	buf_add_str(buf, len, " \"dev\": \"%s\" ", disk->sg_path);
	buf_add_str(buf, len, ", \"vendor\": \"%s\"", disk->vendor);
	buf_add_str(buf, len, ", \"model\": \"%s\"", disk->model);
	buf_add_str(buf, len, ", \"serial\": \"%s\"", disk->serial);
	buf_add_str(buf, len, ", \"fw_rev\": \"%s\"", disk->fw_rev);
	buf_add_char(buf, len, '}');
	buf_add_char(buf, len, ' ');

	return orig_len - len;
}

static inline bool sg_request_data(disk_t *disk, sg_callback cb, unsigned char *cdb, int cdb_len)
{
	bool alive = sg_request(&disk->sg, &disk->data_request, cb, cdb, cdb_len, SG_DXFER_FROM_DEV, disk->data_buf, sizeof(disk->data_buf), DEF_TIMEOUT);
	if (!alive && disk->on_death) {
		printf("Disk %p died\n", disk);
		disk->on_death(disk);
	}
	return alive;
}

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
		bool alive = sg_request(&disk->sg, &disk->tur_request, disk_reply, cdb, sizeof(cdb), SG_DXFER_NONE, NULL, 0, DEF_TIMEOUT);
		printf("request sent, alive: %s\n", alive ? "yes" : "no");
		if (!alive && disk->on_death) {
			printf("Disk %p died\n", disk);
			disk->on_death(disk);
		}
	} else {
		printf("TUR request on the air still!\n");
	}
}

static void disk_inquiry_reply(sg_request_t *req, unsigned char status, unsigned char masked_status,
				unsigned char msg_status, char sb_len_wr, short int host_status,
				short int driver_status, int residual_len, int duration_msec, int info)
{
	disk_t *disk = container_of(req, disk_t, data_request);
	printf("Got inquiry reply in %f msecs (%d in sg)\n", 1000.0*(req->end-req->start), duration_msec);

	if (status != 0) {
		printf("Inquiry failed, status=%d!\n", status);
		disk_state_machine_step(disk);
		return;
	}

	parse_inquiry(disk->data_buf, sizeof(disk->data_buf) - residual_len, &disk->device_type, disk->vendor,
	              disk->model, disk->fw_rev, disk->serial);

	if (strcmp(disk->vendor, "ATA     ") == 0) {
		// Disk is an ATA Disk, need to use ATA INQUIRY to get the real details
		printf("ATA disk needs to be ATA IDENTIFYied\n");
		disk->pending_ata_identify = 1;
	}

	disk_state_machine_step(disk);
}

void disk_inquiry(disk_t *disk)
{
	if (disk->data_request.in_progress) {
		disk->pending_inquiry = 1;
		return;
	}

	disk->pending_inquiry = 0;

	unsigned char cdb[6];
	int cdb_len = cdb_inquiry_simple(cdb, sizeof(disk->data_buf));
	bool alive = sg_request_data(disk, disk_inquiry_reply, cdb, cdb_len);
	printf("Inquiry request sent, alive: %s\n", alive ? "yes" : "no");
}

static void disk_ata_identify_reply(sg_request_t *req, unsigned char status, unsigned char masked_status,
				unsigned char msg_status, char sb_len_wr, short int host_status,
				short int driver_status, int residual_len, int duration_msec, int info)
{
	disk_t *disk = container_of(req, disk_t, data_request);
	printf("Got ATA IDENTIFY reply in %f msecs (%d in sg)\n", 1000.0*(req->end-req->start), duration_msec);

	if (status != 0) {
		printf("ATA IDENTIFY failed, status=%d!\n", status);
		disk_state_machine_step(disk);
		return;
	}

	char ata_model[(46 - 27 + 1)*2 + 1] = "";

	ata_get_ata_identify_model(disk->data_buf, ata_model);

	char *vendor = strtok(ata_model, " ");
	strncpy(disk->vendor, vendor, sizeof(disk->vendor));

	char *model = strtok(NULL, " ");
	strncpy(disk->model, model, sizeof(disk->model));

	ata_get_ata_identify_serial_number(disk->data_buf, disk->serial);
	ata_get_ata_identify_fw_rev(disk->data_buf, disk->fw_rev);
	printf("ATA model: %s:%s\n", model, vendor);

	disk_state_machine_step(disk);
}

void disk_ata_identify(disk_t *disk)
{
	if (disk->data_request.in_progress) {
		disk->pending_ata_identify = 1;
		return;
	}

	disk->pending_ata_identify = 0;

	unsigned char cdb[12];
	int cdb_len = 12;
	memset(cdb, 0, sizeof(cdb));
	cdb[0] = 0xA1;
	cdb[1] = 0x4<<1;
	cdb[2] = ata_passthrough_flags_2(0, 0, 1, 1, ATA_PT_LEN_SPEC_SECTOR_COUNT);
	cdb[4] = 1;
	cdb[9] = 0xEC;
	bool alive = sg_request_data(disk, disk_ata_identify_reply, cdb, cdb_len);
	printf("ATA identify request sent, alive: %s\n", alive? "yes" : "no");

}

static void disk_state_machine_step(disk_t *disk)
{
	if (disk->pending_inquiry)
		disk_inquiry(disk);
	else if (disk->pending_ata_identify)
		disk_ata_identify(disk);
}

void disk_cleanup(disk_t *disk)
{
	sg_close(&disk->sg, EV_DEFAULT);
	memset(disk, 0, sizeof(*disk));
}

void disk_init(disk_t *disk, const char *dev)
{
	memset(disk, 0, sizeof(*disk));
	strcpy(disk->sg_path, dev);

	if (!sg_init(&disk->sg, EV_DEFAULT, disk->sg_path)) {
		printf("Failed to access disk %s: %m\n", disk->sg_path);
		return;
	}

	disk_inquiry(disk);
}
