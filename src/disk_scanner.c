#include "disk_scanner.h"
#include "wire_log.h"

#include "scsicmd.h"
#include "ata.h"
#include "ata_parse.h"

#include <scsi/sg.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define DEF_TIMEOUT 30*1000

typedef bool (*parser_cb_t)(disk_scanner_t *disk);

static char nibble_to_char(unsigned char ch)
{
	if (ch < 10)
		return '0' + ch;
	else if (ch < 16)
		return 'A' + ch - 10;
	else
		return '*';
}

static void log_hex(const char *prefix, const unsigned char *buf, int buf_len)
{
	char hex[3*16];
	int i, j;
	int line = 1;

	for (i = 0, j = 0; i < buf_len; i++) {
		if (j > 0)
			hex[j++] = ' ';
		hex[j++] = nibble_to_char((buf[i] & 0xF0) >> 4);
		hex[j++] = nibble_to_char((buf[i] & 0x0F));

		if (j + 3 > sizeof(hex)) {
			hex[j] = 0;
			wire_log(WLOG_DEBUG, "%s:%d: %s", prefix, line++, hex);
			j = 0;
		}
	}

	if (j > 0) {
		hex[j] = 0;
		wire_log(WLOG_DEBUG, "%s:%d: %s", prefix, line++, hex);
	}
}

static bool sg_request_data(disk_scanner_t *disk, unsigned char *cdb, int cdb_len)
{
	if (sg_request_submit(&disk->sg, &disk->data_request, cdb, cdb_len, SG_DXFER_FROM_DEV, disk->data_buf, sizeof(disk->data_buf), DEF_TIMEOUT) < 0) {
		wire_log(WLOG_INFO, "Failed to submit request for disk scanner");
		return false;
	}

	if (sg_request_wait_response(&disk->sg, &disk->data_request) < 0) {
		wire_log(WLOG_INFO, "Failed to read request for disk scanner");
		return false;
	}

	if (disk->data_request.hdr.status != 0) {
		wire_log(WLOG_INFO, "Request failed, status=%d", disk->data_request.hdr.status);
		if (disk->data_request.hdr.sb_len_wr) {
			char sense_text[64*3];
			unsigned char i;
			int j;
			for (i = 0, j = 0; i < disk->data_request.hdr.sb_len_wr && i < 64; i++) {
				if (j > 0)
					sense_text[j++] = ' ';
				sense_text[j++] = nibble_to_char((disk->data_request.hdr.sbp[i] & 0xF0) >> 4);
				sense_text[j++] = nibble_to_char((disk->data_request.hdr.sbp[i] & 0x0F));
			}
			sense_text[j] = 0;

			wire_log(WLOG_INFO, "Sense buffer: %s", sense_text);

			sense_info_t sense_info;
			bool parsed = scsi_parse_sense(disk->data_request.hdr.sbp, disk->data_request.hdr.sb_len_wr, &sense_info);
			if (parsed) {
				wire_log(WLOG_INFO, "Sense info: %01X/%02X/%02X/%04X", sense_info.sense_key, sense_info.asc, sense_info.ascq, 0/*sense_info.vendor_unique_error*/);
			}
		}
		return false;
	}

	return true;
}

static bool ata_identify_parse(disk_scanner_t *disk)
{
	sg_request_t *req = &disk->data_request;
	wire_log(WLOG_INFO, "Got ATA IDENTIFY reply in %f msecs (%d in sg)", 1000.0*(req->end-req->start), req->hdr.duration);

	char ata_model[(46 - 27 + 1)*2 + 1] = "";

	ata_get_ata_identify_model(disk->data_buf, ata_model);

	char *vendor = strtok(ata_model, " ");
	strncpy(disk->disk_info.vendor, vendor, sizeof(disk->disk_info.vendor));

	char *model = strtok(NULL, " ");
	strncpy(disk->disk_info.model, model, sizeof(disk->disk_info.model));

	ata_get_ata_identify_serial_number(disk->data_buf, disk->disk_info.serial);
	ata_get_ata_identify_fw_rev(disk->data_buf, disk->disk_info.fw_rev);
	wire_log(WLOG_INFO, "ATA model: %s:%s", model, vendor);
	disk->disk_info.ata.smart_supported = ata_get_ata_identify_smart_supported(disk->data_buf);
    disk->disk_info.ata.smart_ok = true; // Just default to it until we actually read it

	return true;
}

static bool inquiry_parse(disk_scanner_t *disk)
{
	sg_request_t *req = &disk->data_request;
	wire_log(WLOG_INFO, "Got inquiry reply in %f msecs (%d in sg)", 1000.0*(req->end-req->start), req->hdr.duration);
	wire_log(WLOG_DEBUG, "data buf size %u res %u data %u", sizeof(disk->data_buf), req->hdr.resid, sizeof(disk->data_buf) - req->hdr.resid);
	log_hex("data buffer", (unsigned char *)disk->data_buf, sizeof(disk->data_buf) - req->hdr.resid);

	bool success = parse_inquiry((unsigned char *)disk->data_buf, sizeof(disk->data_buf) - req->hdr.resid, &disk->disk_info.device_type, disk->disk_info.vendor,
	              disk->disk_info.model, disk->disk_info.fw_rev, disk->disk_info.serial);

    wire_log(WLOG_INFO, "Disk identified by INQUIRY as vendor='%s' model='%s' serial='%s' fw_rev='%s' success=%d",
           disk->disk_info.vendor,
           disk->disk_info.model,
           disk->disk_info.serial,
           disk->disk_info.fw_rev,
		   success);
	return success;
}

bool disk_scanner_inquiry(disk_scanner_t *disk, const char *sg_dev)
{
	memset(disk, 0, sizeof(*disk));
	strcpy(disk->sg_path, sg_dev);

	if (!sg_init(&disk->sg, disk->sg_path)) {
		wire_log(WLOG_INFO, "Failed to access disk %s: %m", disk->sg_path);
		return false;
	}

	bool success;

	unsigned char cdb[32];
	int cdb_len = cdb_inquiry_simple(cdb, sizeof(disk->data_buf));
	success = sg_request_data(disk, cdb, cdb_len);
	if (!success) {
		wire_log(WLOG_INFO, "Failed to request INQUIRY on %s", sg_dev);
		goto exit;
	}

	if (!inquiry_parse(disk)) {
		wire_log(WLOG_INFO, "Failed to parse inquiry data on %s", sg_dev);
		success = false;
		goto exit;
	}

	if (strcmp(disk->disk_info.vendor, "ATA     ") == 0 || disk->disk_info.serial[0] == 0) {
		// Disk is an ATA Disk, need to use ATA IDENTIFY to get the real details
		wire_log(WLOG_INFO, "ATA disk needs to be ATA IDENTIFYied");
		disk->disk_info.disk_type = DISK_TYPE_ATA;
	} else {
		disk->disk_info.disk_type = DISK_TYPE_SAS;
		goto exit;
	}

	cdb_len = cdb_ata_identify(cdb);
	success = sg_request_data(disk, cdb, cdb_len);
	wire_log(WLOG_INFO, "ATA identify request sent, alive: %s", success ? "yes" : "no");
	if (!success)
		goto exit;

	if (!ata_identify_parse(disk)) {
		success = false;
		goto exit;
	}

	success = true;

exit:
	sg_close(&disk->sg);
	return success;
}
