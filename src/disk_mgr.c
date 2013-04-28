#include "disk_mgr.h"
#include "disk.h"
#include "disk_scanner.h"
#include "util.h"
#include "system_id.h"
#include "protocol.pb-c.h"

#include <arpa/inet.h>
#include <sys/mman.h>
#include <ctype.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#define MAX_DISKS 128
#define MAX_SCAN_DISKS MAX_DISKS

struct disk_state {
	int prev;
	int next;
	bool died;
	disk_t disk;
};

struct disk_mgr {
	struct ev_loop *loop;
	ev_timer periodic_rescan_timer;
	ev_timer tur_timer;
	ev_timer five_min_timer;
	ev_timer stop_timer;
	ev_async cleanup_dead_disks;
	ev_signal hup_signal;

	system_identifier_t system_id;
	int alive_head;
	int dead_head;
	int first_unused_entry;
	struct disk_state disk_list[MAX_DISKS];
	disk_scanner_t disk_scan_list[MAX_SCAN_DISKS];
	char state_file_name[256];
};
static struct disk_mgr mgr;

#define for_active_disks(_idx_) \
	for (_idx_ = mgr.alive_head; _idx_ != -1; _idx_ = mgr.disk_list[_idx_].next)

#define for_dead_disks(_idx_) \
	for (_idx_ = mgr.dead_head; _idx_ != -1; _idx_ = mgr.disk_list[_idx_].next)

static inline void strlcpy(char *dst, const char *src, size_t len)
{
    strncpy(dst, src, len);
    dst[len-1] = 0;
}

static void disk_list_remove(int idx, int *old_head)
{
	assert(idx != -1);

	struct disk_state *entry = &mgr.disk_list[idx];

	if (entry->prev != -1)
		mgr.disk_list[entry->prev].next = entry->next;
	else
		*old_head = entry->next;

	if (entry->next != -1)
		mgr.disk_list[entry->next].prev = entry->prev;

	entry->prev = entry->next = -1;
}

static void disk_list_append(int idx, int *new_head)
{
	assert(idx != -1);

	// Go to the end of the list starting with new_head
	int *disk_idx_ptr = new_head;
	int prev_disk_idx = -1;
	while (*disk_idx_ptr != -1) {
		prev_disk_idx = *disk_idx_ptr;
		disk_idx_ptr = &mgr.disk_list[*disk_idx_ptr].next;
	}

	// Add to the end of the list
	struct disk_state *entry = &mgr.disk_list[idx];
	entry->prev = prev_disk_idx;
	entry->next = -1;
	*disk_idx_ptr = idx;
}

static int disk_list_get_unused(void)
{
	if (mgr.first_unused_entry < ARRAY_SIZE(mgr.disk_list)) {
		return mgr.first_unused_entry++;
	}

	// No more never used entries, recycle old ones
	int idx = mgr.dead_head;
	if (idx != -1)
		disk_list_remove(idx, &mgr.dead_head);
	return idx;
}

int disk_manager_disk_list_json(char *buf, int len)
{
	int orig_len = len;
	int disk_idx;
	bool first = true;

	buf_add_char(buf, len, '[');

	// TODO: Handle dead disks
	for_active_disks(disk_idx) {
		if (!first)
			buf_add_char(buf, len, ',');
		else
			first = false;

		int written = disk_json(&mgr.disk_list[disk_idx].disk, buf, len);
		if (written < 0)
			return -1;

		buf += written;
		len -= written;
	}

	buf_add_char(buf, len, ']');
	buf_add_char(buf, len, 0);

	// Return number of stored characters
	return orig_len - len;
}

static void cleanup_dead_disks(struct ev_loop *loop, ev_async *watcher, int revents)
{
	printf("Cleanup dead disks started\n");
	bool found = false;
	int disk_idx;

	do {
		found = false;

		// since we modify the list, we can't do it in the loop
		for_active_disks(disk_idx) {
			if (mgr.disk_list[disk_idx].died) {
				found = true;
				break;
			}
		}

		if (found) {
			mgr.disk_list[disk_idx].died = false;

			disk_list_remove(disk_idx, &mgr.alive_head);
			disk_list_append(disk_idx, &mgr.dead_head);

			// TODO: When cleaning a dead disk, keep the useful info in it for a while, it may yet return
			disk_t *disk = &mgr.disk_list[disk_idx].disk;
			printf("Cleaning dead disk %p\n", disk);
			disk_cleanup(disk);
		}
	} while (found);
	printf("Cleanup dead disks finished\n");
}

static void on_death(disk_t *disk)
{
	struct disk_state *state = container_of(disk, struct disk_state, disk);
	state->died = true;
	printf("Marking disk %p as dead for cleanup\n", disk);
	ev_async_send(mgr.loop, &mgr.cleanup_dead_disks);
}

static bool disk_manager_is_active(const char *dev)
{
	int disk_idx;
	for_active_disks(disk_idx) {
		char *sg_path = mgr.disk_list[disk_idx].disk.sg_path;
		if (strcmp(dev, sg_path) == 0) {
			return true;
		}
	}

	return false;
}

static void disk_mgr_scan_done_cb(disk_scanner_t *disk_scanner)
{
	if (!disk_scanner->success) {
		printf("Error while scanning device %s\n", disk_scanner->sg_path);
		return;
	}

	disk_info_t *new_disk_info = &disk_scanner->disk_info;

	if (new_disk_info->device_type != SCSI_DEV_TYPE_BLOCK) {
		printf("Unsupported device type %d\n", new_disk_info->device_type);
		return;
	}

	// Is this a disk we have seen in the past and can reattach to the old info?
	int disk_idx;
	for_dead_disks(disk_idx) {
		disk_t *disk = &mgr.disk_list[disk_idx].disk;
		disk_info_t *old_disk_info = &disk->disk_info;

		if (strcmp(new_disk_info->vendor, old_disk_info->vendor) == 0 &&
		    strcmp(new_disk_info->model, old_disk_info->model) == 0 &&
			strcmp(new_disk_info->serial, old_disk_info->serial) == 0)
		{
			disk_init(disk, new_disk_info, disk_scanner->sg_path);
			disk->on_death = on_death;
			disk_list_remove(disk_idx, &mgr.dead_head);
			disk_list_append(disk_idx, &mgr.alive_head);
			return;
		}
	}

	// This is a completely new disk, allocate a new one for it
	int new_disk_idx = disk_list_get_unused();
	if (new_disk_idx != -1) {
		printf("adding idx=%d!\n", new_disk_idx);
		disk_t *disk = &mgr.disk_list[new_disk_idx].disk;
		disk_init(disk, new_disk_info, disk_scanner->sg_path);
		disk->on_death = on_death;
		disk_list_append(new_disk_idx, &mgr.alive_head);
	} else {
		printf("Want to add but no space!\n");
	}
}

static bool disk_manager_save_disk_info(disk_info_t *disk_info, int fd)
{
    Disksurvey__DiskATA disk_ata_pb = DISKSURVEY__DISK_ATA__INIT;
    Disksurvey__DiskSAS disk_sas_pb = DISKSURVEY__DISK_SAS__INIT;
    Disksurvey__DiskInfo disk_info_pb = DISKSURVEY__DISK_INFO__INIT;
    void *buf;
    uint32_t buf_size;

    // Fill the data
    disk_info_pb.vendor = strdup(disk_info->vendor);
    disk_info_pb.model = strdup(disk_info->model);
    disk_info_pb.serial = strdup(disk_info->serial);
    disk_info_pb.fw_rev = strdup(disk_info->fw_rev);
    disk_info_pb.has_device_type = true;
    disk_info_pb.device_type = disk_info->device_type;

    switch (disk_info->disk_type) {
        case DISK_TYPE_ATA:
            disk_ata_pb.smart_supported = disk_info->ata.smart_supported;
            disk_ata_pb.smart_ok = disk_info->ata.smart_ok;
            disk_info_pb.ata = &disk_ata_pb;
            break;
        case DISK_TYPE_SAS:
            disk_sas_pb.smart_asc = disk_info->sas.smart_asc;
            disk_sas_pb.smart_ascq = disk_info->sas.smart_ascq;
            disk_info_pb.sas = &disk_sas_pb;
            break;
        case DISK_TYPE_UNKNOWN:
            break;
    }

    // Marshall it
    buf_size = disksurvey__disk_info__get_packed_size(&disk_info_pb);
    buf = alloca(buf_size);
    disksurvey__disk_info__pack(&disk_info_pb, buf);

    // Write the size
    uint32_t buf_size_n = htonl(buf_size);
	ssize_t ret = write(fd, &buf_size_n, sizeof(buf_size_n));
	if (ret != sizeof(buf_size_n)) {
		printf("Error writing to data file buf_size: %m\n");
		return false;
	}

    // Write the data
	ret = write(fd, buf, buf_size);
	if (ret != buf_size) {
		printf("Error writing to data file (disk_info): %m\n");
		return false;
	}

	return true;
}

static bool disk_manager_save_disk_latency(latency_t *latency, int fd)
{
    int i;
    Disksurvey__LatencyEntry **entries_pb;
    Disksurvey__Latency latency_pb = DISKSURVEY__LATENCY__INIT;
    void *buf;
    uint32_t buf_size;


    // Fill the data
    latency_pb.current_entry = latency->cur_entry;
    latency_pb.has_current_entry = true;
    latency_pb.n_entries = ARRAY_SIZE(latency->entries);

    entries_pb = calloc(latency_pb.n_entries, sizeof(Disksurvey__LatencyEntry*));
    latency_pb.entries = entries_pb;

    for (i = 0; i < latency_pb.n_entries; i++) {
        Disksurvey__LatencyEntry *entry = malloc(sizeof(Disksurvey__LatencyEntry));
        entries_pb[i] = entry;

        disksurvey__latency_entry__init(entry);
        entry->n_top_latencies = ARRAY_SIZE(latency->entries[0].top_latencies);
        entry->top_latencies = latency->entries[i].top_latencies;
        entry->n_histogram = ARRAY_SIZE(latency->entries[0].hist);
        entry->histogram = latency->entries[i].hist;
    }

    // Marshall it
    buf_size = disksurvey__latency__get_packed_size(&latency_pb);
    buf = malloc(buf_size);
    disksurvey__latency__pack(&latency_pb, buf);

    // Write the size
    uint32_t buf_size_n = htonl(buf_size);
	ssize_t ret = write(fd, &buf_size_n, sizeof(buf_size_n));
	if (ret != sizeof(buf_size_n)) {
		printf("Error writing to data file buf_size: %m\n");
		return false;
	}

    // Write the data
	ret = write(fd, buf, buf_size);
	if (ret != buf_size) {
		printf("Error writing to data file (latency): %m\n");
		return false;
	}

	return true;
}

static bool disk_manager_save_disk_state(disk_t *disk, int fd)
{
    if (!disk_manager_save_disk_info(&disk->disk_info, fd))
        return false;
    if (!disk_manager_save_disk_latency(&disk->latency, fd))
        return false;
    return true;
}

static void disk_manager_save_state_nofork(void)
{
	int disk_idx;
	bool error = true;
	char tmp_file_name[256];

	printf("Saving state\n");

	snprintf(tmp_file_name, sizeof(tmp_file_name), "%s.XXXXXX", mgr.state_file_name);
	int fd = mkstemp(tmp_file_name);

	uint32_t version = htonl(2);
	ssize_t ret = write(fd, &version, sizeof(version));
	if (ret != sizeof(version)) {
		printf("Error writing to data file: %m\n");
		goto Exit;
	}

	for_active_disks(disk_idx) {
		disk_t *disk = &mgr.disk_list[disk_idx].disk;
		printf("Saving live disk %d: %p\n", disk_idx, disk);
		if (!disk_manager_save_disk_state(disk, fd)) {
            printf("Error saving disk data\n");
			goto Exit;
        }
	}

	for_dead_disks(disk_idx) {
		disk_t *disk = &mgr.disk_list[disk_idx].disk;
		printf("Saving dead disk %d: %p\n", disk_idx, disk);
		if (!disk_manager_save_disk_state(disk, fd)) {
            printf("Error saving disk data\n");
			goto Exit;
        }
	}

	error = false;

Exit:
	close(fd);
	if (error) {
		unlink(tmp_file_name);
	} else {
		rename(tmp_file_name, mgr.state_file_name);
	}
	printf("Save state done, %s\n", error ? "with errors" : "successfully");
}

/** To avoid any needless delays while writing the state to the disk and to
 * also avoid locking the monitoring from its work we will simply fork to lock
 * the state in a known consistent way and let the parent work normally, the
 * only impact is the copy-on-write that will be needed by the parent when it
 * modifies data but due to the normal rate of work this shouldn't be a big impact.
 */
static void disk_manager_save_state(void)
{
	printf("Forking to save state\n");
	pid_t pid = fork();
	if (pid == 0) {
		/* Child, saves information */
		disk_manager_save_state_nofork();
		exit(0);
	} else if (pid == -1) {
		/* Parent, error */
		printf("Error forking to save state: %m\n");
	} else {
		/* Parent, ok */
	}
}

void disk_manager_rescan(void)
{
	int ret;
	glob_t globbuf = { 0, NULL, 0 };

	printf("Rescanning disks\n");

	ret = glob("/dev/sg*", GLOB_NOSORT, NULL, &globbuf);
	if (ret != 0) {
		printf("Glob had an error finding scsi generic devices, ret=%d\n", ret);
		return;
	}

	printf("Found %d devices\n", globbuf.gl_pathc);

	int glob_idx;

	for (glob_idx = 0; glob_idx < globbuf.gl_pathc; glob_idx++) {
		char *dev = globbuf.gl_pathv[glob_idx];
		printf("\tDevice: %s - ", dev);

		if (disk_manager_is_active(dev)) {
			printf("already known\n");
			continue;
		}

		int disk_scanner_idx;
		for (disk_scanner_idx = 0; disk_scanner_idx < MAX_SCAN_DISKS; disk_scanner_idx++) {
			if (!disk_scanner_active(&mgr.disk_scan_list[disk_scanner_idx]))
				break;
		}
		if (disk_scanner_idx == MAX_SCAN_DISKS) {
			printf("No space to scan a new disk, will wait for next rescan\n");
		} else {
			disk_scanner_inquiry(&mgr.disk_scan_list[disk_scanner_idx], dev, disk_mgr_scan_done_cb);
		}
	}
}

static void rescan_cb(struct ev_loop *loop, ev_timer *watcher, int revents)
{
	disk_manager_rescan();
}

static void tur_timer(struct ev_loop *loop, ev_timer *watcher, int revents)
{
	int disk_idx;
	for_active_disks(disk_idx) {
		disk_tur(&mgr.disk_list[disk_idx].disk);
	}
}

static void five_min_tick_timer(struct ev_loop *loop, ev_timer *watcher, int revents)
{
	int disk_idx;
	for_active_disks(disk_idx) {
		disk_tick(&mgr.disk_list[disk_idx].disk);
	}

	disk_manager_save_state();
}

static void handle_sighup(struct ev_loop *loop, ev_signal *watcher, int revents)
{
	disk_manager_save_state();
}

static void disk_manager_init_mgr(void)
{
	// Initialize the disk list
	mgr.first_unused_entry = 0;

	// Initialize the heads
	mgr.alive_head = -1;
	mgr.dead_head = -1;

	snprintf(mgr.state_file_name, sizeof(mgr.state_file_name), "./disksurvey.dat");
}

static void disk_manager_load(void)
{
	int fd = open(mgr.state_file_name, O_RDONLY);
	if (fd < 0) {
		printf("Failed to open state data: %m\n");
		return;
	}

    struct stat statbuf;
    int ret = fstat(fd, &statbuf);
    if (ret < 0) {
        printf("Failed to stat file: %m\n");
        close(fd);
        return;
    }

    if (statbuf.st_size < 4) {
        printf("Not enough size to include any useful data, ignore it\n");
        close(fd);
        return;
    }

    unsigned char *buf = mmap(NULL, statbuf.st_size, PROT_READ, MAP_PRIVATE|MAP_POPULATE, fd, 0);
    if (!buf) {
        printf("Failed to map data: %m\n");
        close(fd);
        return;
    }

	uint32_t version = ntohl(*(uint32_t*)buf);
	if (version != 2) {
		printf("Unknown version of state file, got: %d expected: %d\n", version, 1);
		goto Exit;
	}

    printf("Loading disk data version %u\n", version);

    uint32_t offset = 4;
	uint32_t item_size;
	int i;
	for (i = 0; i < MAX_DISKS; i++) {
        Disksurvey__DiskInfo *disk_info_pb = NULL;
        Disksurvey__Latency *latency_pb = NULL;
		disk_info_t disk_info;
		latency_t latency;
        bool bad_disk = false;

        /* Read the disk info part */
        if (offset+4 > statbuf.st_size) {
            // This ends the last data, exit silently
            goto Exit;
        }
        item_size = ntohl(*(uint32_t*)(buf+offset));
        offset += 4;
        if (offset + item_size > statbuf.st_size) {
            printf("Not enough data in the file to finish reading, offset=%u item_size=%u size=%u\n", offset, item_size, (uint32_t)statbuf.st_size);
            goto Exit;
        }
        disk_info_pb = disksurvey__disk_info__unpack(NULL, item_size, buf+offset);
        if (!disk_info_pb) {
            printf("Failed to unpack disk survey disk info data\n");
            goto Exit;
        }
        offset += item_size;

        strlcpy(disk_info.vendor, disk_info_pb->vendor, sizeof(disk_info.vendor));
        strlcpy(disk_info.model, disk_info_pb->model, sizeof(disk_info.model));
        strlcpy(disk_info.serial, disk_info_pb->serial, sizeof(disk_info.serial));
        strlcpy(disk_info.fw_rev, disk_info_pb->fw_rev, sizeof(disk_info.fw_rev));
        if (disk_info_pb->has_device_type)
            disk_info.device_type = disk_info_pb->device_type;
        else
            disk_info.device_type = 0;

        if (disk_info_pb->ata && disk_info_pb->sas) {
            printf("A disk can't be both ATA and SAS at the same time, skipping\n");
            bad_disk = true;
        } else if (disk_info_pb->ata) {
            disk_info.ata.smart_supported = disk_info_pb->ata->smart_supported;
            disk_info.ata.smart_ok = disk_info_pb->ata->smart_ok;
        } else if (disk_info_pb->sas) {
            disk_info.sas.smart_asc = disk_info_pb->sas->smart_asc;
            disk_info.sas.smart_ascq = disk_info_pb->sas->smart_ascq;
        } else {
            printf("Not an ATA nor SAS disk, skipping\n");
            bad_disk = true;
        }

        disksurvey__disk_info__free_unpacked(disk_info_pb, NULL);

        /* Read the latency part */
        if (offset+4 > statbuf.st_size) {
            printf("Not enough data in the file to read the latency size, offset=%u size=%u\n", offset, (uint32_t)statbuf.st_size);
            goto Exit;
        }
        item_size = ntohl(*(uint32_t*)(buf+offset));
        offset += 4;
        if (offset + item_size > statbuf.st_size) {
            printf("Not enough data in the file to finish reading, offset=%u item_size=%u size=%u\n", offset, item_size, (uint32_t)statbuf.st_size);
            goto Exit;
        }
        latency_pb = disksurvey__latency__unpack(NULL, item_size, buf+offset);
        if (!latency_pb) {
            printf("Failed to unpack disk survey latency data\n");
            goto Exit;
        }
        offset += item_size;
        // TODO: convert the latency part
        disksurvey__latency__free_unpacked(latency_pb, NULL);

        if (bad_disk) {
            printf("Skipped a bad disk\n");
            continue;
        }

        /* Both parts loaded, add the disk */
		printf("Loaded disk data\n");
		mgr.disk_list[i].disk.disk_info = disk_info;
        mgr.disk_list[i].disk.latency = latency;
		disk_list_append(i, &mgr.dead_head);
		mgr.first_unused_entry = i+1;
	}

Exit:
    munmap(buf, statbuf.st_size);
	close(fd);
}

void disk_manager_init(struct ev_loop *loop)
{
	disk_manager_init_mgr();
	disk_manager_load();
	system_identifier_read(&mgr.system_id);

	mgr.loop = loop;

	// The first event will be 0 seconds into the event loop and then once every hour
	ev_timer *timer = &mgr.periodic_rescan_timer;
	ev_timer_init(timer, rescan_cb, 0, 60*60);
	ev_timer_start(loop, &mgr.periodic_rescan_timer);

	timer = &mgr.tur_timer;
	ev_timer_init(timer, tur_timer, 0.0, 1.0);
	ev_timer_start(loop, timer);

	timer = &mgr.five_min_timer;
	ev_timer_init(timer, five_min_tick_timer, 5.0*60.0, 5.0*60.0);
	ev_timer_start(loop, timer);

	ev_async *async = &mgr.cleanup_dead_disks;
	ev_async_init(async, cleanup_dead_disks);
	ev_async_start(loop, async);

	ev_signal *evsig = &mgr.hup_signal;
	ev_signal_init(evsig, handle_sighup, SIGHUP);
	ev_signal_start(loop, evsig);
}

static void disk_manager_check_stop(struct ev_loop *loop, ev_timer *watcher, int revents)
{
	printf("Checking for stop\n");

	int disk_idx;
	for_active_disks(disk_idx) {
		disk_t *disk = &mgr.disk_list[disk_idx].disk;
		printf("Trying to stop disk %d: %p\n", disk_idx, disk);
		disk_stop(disk);
	}

	cleanup_dead_disks(loop, NULL, revents);

	if (mgr.alive_head == -1) {
		printf("No more live disks, stopping\n");
		// No live disks anymore, can cleanly shutdown
		ev_break(mgr.loop, EVBREAK_ALL);
		disk_manager_save_state_nofork();
	}
	printf("checking done\n");
}

void disk_manager_stop(void)
{
	ev_timer_stop(mgr.loop, &mgr.tur_timer);
	ev_timer_stop(mgr.loop, &mgr.five_min_timer);

	ev_timer *timer = &mgr.stop_timer;
	ev_timer_init(timer, disk_manager_check_stop, 0, 1);
	ev_timer_start(mgr.loop, timer);
}
