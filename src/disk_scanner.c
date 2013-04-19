#include "disk_scanner.h"

#include "scsicmd.h"
#include "ata.h"
#include "ata_parse.h"

#include <scsi/sg.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define DEF_TIMEOUT 30*1000

static void disk_scanner_done(disk_scanner_t *disk, bool success)
{
    assert(disk->is_active == true);

    disk->success = success;
    disk->done_cb(disk);
    disk->is_active = false;
}

static inline bool sg_request_data(disk_scanner_t *disk, sg_callback cb, unsigned char *cdb, int cdb_len)
{
	bool alive = sg_request(&disk->sg, &disk->data_request, cb, cdb, cdb_len, SG_DXFER_FROM_DEV, disk->data_buf, sizeof(disk->data_buf), DEF_TIMEOUT);
	if (!alive) {
		printf("Disk %p died in scanning\n", disk);
        disk_scanner_done(disk, false);
	}
	return alive;
}

static void disk_scanner_ata_identify_reply(sg_request_t *req, unsigned char status, unsigned char masked_status,
				unsigned char msg_status, char sb_len_wr, short int host_status,
				short int driver_status, int residual_len, int duration_msec, int info)
{
	disk_scanner_t *disk = container_of(req, disk_scanner_t, data_request);
	printf("Got ATA IDENTIFY reply in %f msecs (%d in sg)\n", 1000.0*(req->end-req->start), duration_msec);

	if (status != 0) {
		printf("ATA IDENTIFY failed, status=%d!\n", status);
        disk_scanner_done(disk, false);
		return;
	}

	char ata_model[(46 - 27 + 1)*2 + 1] = "";

	ata_get_ata_identify_model(disk->data_buf, ata_model);

	char *vendor = strtok(ata_model, " ");
	strncpy(disk->disk_info.vendor, vendor, sizeof(disk->disk_info.vendor));

	char *model = strtok(NULL, " ");
	strncpy(disk->disk_info.model, model, sizeof(disk->disk_info.model));

	ata_get_ata_identify_serial_number(disk->data_buf, disk->disk_info.serial);
	ata_get_ata_identify_fw_rev(disk->data_buf, disk->disk_info.fw_rev);
	printf("ATA model: %s:%s\n", model, vendor);
	disk->disk_info.ata.smart_supported = ata_get_ata_identify_smart_supported(disk->data_buf);
    disk->disk_info.ata.smart_ok = true; // Just default to it until we actually read it
    disk_scanner_done(disk, true);
}

static void disk_scanner_ata_identify(disk_scanner_t *disk)
{
	unsigned char cdb[32];
	int cdb_len = cdb_ata_identify(cdb);
	bool alive = sg_request_data(disk, disk_scanner_ata_identify_reply, cdb, cdb_len);
	printf("ATA identify request sent, alive: %s\n", alive? "yes" : "no");
}

static void disk_scanner_inquiry_reply(sg_request_t *req, unsigned char status, unsigned char masked_status,
				unsigned char msg_status, char sb_len_wr, short int host_status,
				short int driver_status, int residual_len, int duration_msec, int info)
{
	disk_scanner_t *disk = container_of(req, disk_scanner_t, data_request);
	printf("Got inquiry reply in %f msecs (%d in sg)\n", 1000.0*(req->end-req->start), duration_msec);

	if (status != 0) {
		printf("Inquiry failed, status=%d!\n", status);
        disk_scanner_done(disk, false);
		return;
	}

	parse_inquiry(disk->data_buf, sizeof(disk->data_buf) - residual_len, &disk->disk_info.device_type, disk->disk_info.vendor,
	              disk->disk_info.model, disk->disk_info.fw_rev, disk->disk_info.serial);

    printf("Disk identified by INQUIRY as vendor='%s' model='%s' serial='%s' fw_rev='%s'\n",
           disk->disk_info.vendor,
           disk->disk_info.model,
           disk->disk_info.serial,
           disk->disk_info.fw_rev);

	if (strcmp(disk->disk_info.vendor, "ATA     ") == 0 || disk->disk_info.serial[0] == 0) {
		// Disk is an ATA Disk, need to use ATA IDENTIFY to get the real details
		printf("ATA disk needs to be ATA IDENTIFYied\n");
		disk->disk_info.disk_type = DISK_TYPE_ATA;
        disk_scanner_ata_identify(disk);
	} else {
		disk->disk_info.disk_type = DISK_TYPE_SAS;
        disk_scanner_done(disk, true);
	}
}

void disk_scanner_inquiry(disk_scanner_t *disk, const char *sg_dev, scanner_done_cb done_cb)
{
    assert(done_cb);

	memset(disk, 0, sizeof(*disk));
	strcpy(disk->sg_path, sg_dev);
    disk->done_cb = done_cb;

	if (!sg_init(&disk->sg, EV_DEFAULT, disk->sg_path)) {
		printf("Failed to access disk %s: %m\n", disk->sg_path);
		return;
	}

    disk->is_active = true;
	unsigned char cdb[6];
	int cdb_len = cdb_inquiry_simple(cdb, sizeof(disk->data_buf));
	bool alive = sg_request_data(disk, disk_scanner_inquiry_reply, cdb, cdb_len);
	printf("Inquiry request sent, alive: %s\n", alive ? "yes" : "no");
}

bool disk_scanner_active(disk_scanner_t *disk_scanner)
{
    return disk_scanner->is_active;
}
