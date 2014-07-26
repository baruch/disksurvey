#include "disk_mgr.h"
#include "disk.h"
#include "disk_scanner.h"
#include "util.h"
#include "system_id.h"
#include "protocol.pb-c.h"
#include "timer_bus.h"

#include "wire.h"
#include "wire_fd.h"
#include "wire_stack.h"
#include "wire_wait.h"
#include "wire_log.h"
#include "wire_io.h"

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
	timer_bus_t timer_bus;
	wire_wait_t wait_rescan;
	wire_t task_rescan;
	wire_t task_tur;
	wire_t task_five_min_timer;
	wire_t task_stop;
	wire_t task_dead_disk_reaper;
	wire_pool_t wire_pool;

	system_identifier_t system_id;
	int active;
	int alive_head;
	int dead_head;
	int first_unused_entry;
	struct disk_state disk_list[MAX_DISKS];
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

static void cleanup_dead_disks(struct disk_mgr *m)
{
	wire_log(WLOG_INFO, "Cleanup dead disks started");
	bool found = false;
	int disk_idx;

	do {
		found = false;

		// since we modify the list, we can't do it in the loop
		for_active_disks(disk_idx) {
			if (m->disk_list[disk_idx].died) {
				found = true;
				break;
			}
		}

		if (found) {
			m->disk_list[disk_idx].died = false;

			disk_list_remove(disk_idx, &m->alive_head);
			disk_list_append(disk_idx, &m->dead_head);
		}
	} while (found);
	wire_log(WLOG_INFO, "Cleanup dead disks finished");
}

static void task_dead_disk_reaper(void *arg)
{
	struct disk_mgr *m = arg;

	while (1) {
		cleanup_dead_disks(m);
		wire_suspend();
	}
}

static void on_death(disk_t *disk)
{
	struct disk_state *state = container_of(disk, struct disk_state, disk);
	state->died = true;
	wire_log(WLOG_INFO, "Marking disk %p as dead for cleanup", disk);
	wire_resume(&mgr.task_dead_disk_reaper);
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

static void disk_mgr_scan_done(disk_scanner_t *disk_scanner)
{
	disk_info_t *new_disk_info = &disk_scanner->disk_info;

	if (new_disk_info->device_type != SCSI_DEV_TYPE_BLOCK) {
		wire_log(WLOG_INFO, "Unsupported device type %d", new_disk_info->device_type);
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
            wire_log(WLOG_INFO, "Attaching to a previously seen disk");
			disk_init(disk, new_disk_info, disk_scanner->sg_path, &mgr.wire_pool);
			disk->on_death = on_death;
			disk_list_remove(disk_idx, &mgr.dead_head);
			disk_list_append(disk_idx, &mgr.alive_head);
			return;
		}
	}

	// This is a completely new disk, allocate a new one for it
	int new_disk_idx = disk_list_get_unused();
	if (new_disk_idx != -1) {
		wire_log(WLOG_INFO, "Adding a new disk at idx=%d!", new_disk_idx);
		disk_t *disk = &mgr.disk_list[new_disk_idx].disk;
		disk_init(disk, new_disk_info, disk_scanner->sg_path, &mgr.wire_pool);
		disk->on_death = on_death;
		disk_list_append(new_disk_idx, &mgr.alive_head);
	} else {
		wire_log(WLOG_INFO, "Want to add but no space!");
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
		wire_log(WLOG_INFO, "Error writing to data file buf_size: %m");
		return false;
	}

    // Write the data
	ret = write(fd, buf, buf_size);
	if (ret != buf_size) {
		wire_log(WLOG_INFO, "Error writing to data file (disk_info): %m");
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
		wire_log(WLOG_INFO, "Error writing to data file buf_size: %m");
		return false;
	}

    // Write the data
	ret = write(fd, buf, buf_size);
	if (ret != buf_size) {
		wire_log(WLOG_INFO, "Error writing to data file (latency): %m");
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

	wire_log(WLOG_INFO, "Saving state");

	snprintf(tmp_file_name, sizeof(tmp_file_name), "%s.XXXXXX", mgr.state_file_name);
	int fd = mkstemp(tmp_file_name);

	uint32_t version = htonl(2);
	ssize_t ret = write(fd, &version, sizeof(version));
	if (ret != sizeof(version)) {
		wire_log(WLOG_INFO, "Error writing to data file: %m");
		goto Exit;
	}

	for_active_disks(disk_idx) {
		disk_t *disk = &mgr.disk_list[disk_idx].disk;
		wire_log(WLOG_INFO, "Saving live disk %d: %p", disk_idx, disk);
		if (!disk_manager_save_disk_state(disk, fd)) {
            wire_log(WLOG_INFO, "Error saving disk data");
			goto Exit;
        }
	}

	for_dead_disks(disk_idx) {
		disk_t *disk = &mgr.disk_list[disk_idx].disk;
		wire_log(WLOG_INFO, "Saving dead disk %d: %p", disk_idx, disk);
		if (!disk_manager_save_disk_state(disk, fd)) {
            wire_log(WLOG_INFO, "Error saving disk data");
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
	wire_log(WLOG_INFO, "Save state done, %s", error ? "with errors" : "successfully");
}

/** To avoid any needless delays while writing the state to the disk and to
 * also avoid locking the monitoring from its work we will simply fork to lock
 * the state in a known consistent way and let the parent work normally, the
 * only impact is the copy-on-write that will be needed by the parent when it
 * modifies data but due to the normal rate of work this shouldn't be a big impact.
 */
void disk_manager_save_state(void)
{
	wire_log(WLOG_INFO, "Forking to save state");
	pid_t pid = fork();
	if (pid == 0) {
		/* Child, saves information */
		disk_manager_save_state_nofork();
		exit(0);
	} else if (pid == -1) {
		/* Parent, error */
		wire_log(WLOG_INFO, "Error forking to save state: %m");
	} else {
		/* Parent, ok */
	}
}

void disk_manager_rescan_internal(struct disk_mgr *m)
{
	int ret;
	glob_t globbuf = { 0, NULL, 0 };

	wire_log(WLOG_INFO, "Rescanning disks");

	ret = wio_glob("/dev/sg*", GLOB_NOSORT, NULL, &globbuf);
	if (ret != 0) {
		wire_log(WLOG_INFO, "Glob had an error finding scsi generic devices, ret=%d", ret);
		return;
	}

	wire_log(WLOG_INFO, "Found %d devices", (int)globbuf.gl_pathc);

	int glob_idx;

	for (glob_idx = 0; glob_idx < globbuf.gl_pathc; glob_idx++) {
		char *dev = globbuf.gl_pathv[glob_idx];

		if (disk_manager_is_active(dev)) {
			wire_log(WLOG_INFO, "Device: %s - already known", dev);
			continue;
		}

		disk_scanner_t disk_scan;
		bool success = disk_scanner_inquiry(&disk_scan, dev);
		if (!success) {
			wire_log(WLOG_INFO, "Device: %s - Error while scanning device %s", dev, disk_scan.sg_path);
		} else {
			disk_mgr_scan_done(&disk_scan);
		}
	}

	wio_globfree(&globbuf);
}

void disk_manager_rescan(void)
{
	wire_wait_resume(&mgr.wait_rescan);
}

static void task_rescan(void *arg)
{
	struct disk_mgr *m = arg;
	wire_wait_list_t wait_list;

	wire_wait_list_init(&wait_list);
	wire_wait_init(&m->wait_rescan);
	wire_wait_chain(&wait_list, &m->wait_rescan);

	while (m->active) {
		disk_manager_rescan_internal(m);
		if (!m->active)
			break;
		wire_wait_reset(&m->wait_rescan);
		wire_list_wait(&wait_list);
	}
}

static void task_tur(void *arg)
{
	struct disk_mgr *m = arg;

	while (timer_bus_sleep(&m->timer_bus, 1) >= 0) {
		int disk_idx;
		for_active_disks(disk_idx) {
			disk_tur(&mgr.disk_list[disk_idx].disk);
		}
	}
}

static void task_five_min_timer(void *arg)
{
	struct disk_mgr *m = arg;

	while (timer_bus_sleep(&m->timer_bus, 5*60)) {
		int disk_idx;
		for_active_disks(disk_idx) {
			disk_tick(&mgr.disk_list[disk_idx].disk);
		}

		// Now let the disk wires do their tick work
		wire_yield();

		// Save state after they did their work
		disk_manager_save_state();

		if (!m->active)
			break;

		disk_manager_rescan();
	}
}

static bool disk_manager_load_latency(latency_t *latency, unsigned char *buf, uint32_t *offset, uint32_t buf_size)
{
    Disksurvey__Latency *latency_pb = NULL;
    uint32_t item_size;

    /* Read the latency part */
    if (*offset+4 > buf_size) {
        wire_log(WLOG_INFO, "Not enough data in the file to read the latency size, offset=%u size=%u", *offset, buf_size);
        return false;
    }

    item_size = ntohl(*(uint32_t*)(buf + *offset));
    *offset += 4;
    if (*offset + item_size > buf_size) {
        wire_log(WLOG_INFO, "Not enough data in the file to finish reading, offset=%u item_size=%u size=%u", *offset, item_size, buf_size);
        return false;
    }
    latency_pb = disksurvey__latency__unpack(NULL, item_size, buf + *offset);
    if (!latency_pb) {
        wire_log(WLOG_INFO, "Failed to unpack disk survey latency data");
        return false;
    }
    *offset += item_size;

    // convert the latency part
    if (latency_pb->has_current_entry)
        latency->cur_entry = latency_pb->current_entry;
    else
        latency->cur_entry = 0;

    int k;
    for (k = 0; k < latency_pb->n_entries; k++) {
        int j;
        Disksurvey__LatencyEntry *entry = latency_pb->entries[k];

        int n_top_latencies = entry->n_top_latencies;
        if (n_top_latencies > ARRAY_SIZE(latency->entries[0].top_latencies))
            n_top_latencies = ARRAY_SIZE(latency->entries[0].top_latencies);
        for (j = 0; j < n_top_latencies; j++) {
            latency->entries[k].top_latencies[j] = entry->top_latencies[j];
        }

        int n_histogram = entry->n_histogram;
        if (n_histogram > ARRAY_SIZE(latency->entries[0].hist))
            n_histogram = ARRAY_SIZE(latency->entries[0].hist);
        for (j = 0; j < n_histogram; j++) {
            latency->entries[k].hist[j] = entry->histogram[j];
        }
    }

    disksurvey__latency__free_unpacked(latency_pb, NULL);
    return true;
}

static bool disk_manager_load_disk_info(disk_info_t *disk_info, unsigned char *buf, uint32_t *offset, uint32_t buf_size)
{
    bool bad_disk = false;
    Disksurvey__DiskInfo *disk_info_pb = NULL;
    uint32_t item_size;

    /* Read the disk info part */
    if (*offset+4 > buf_size) {
        // This ends the last data, exit silently
        return false;
    }
    item_size = ntohl(*(uint32_t*)(buf + *offset));
    *offset += 4;
    if (*offset + item_size > buf_size) {
        wire_log(WLOG_INFO, "Not enough data in the file to finish reading, offset=%u item_size=%u size=%u", *offset, item_size, buf_size);
        return false;
    }
    disk_info_pb = disksurvey__disk_info__unpack(NULL, item_size, buf + *offset);
    if (!disk_info_pb) {
        wire_log(WLOG_INFO, "Failed to unpack disk survey disk info data");
        return false;
    }
    *offset += item_size;

    strlcpy(disk_info->vendor, disk_info_pb->vendor, sizeof(disk_info->vendor));
    strlcpy(disk_info->model, disk_info_pb->model, sizeof(disk_info->model));
    strlcpy(disk_info->serial, disk_info_pb->serial, sizeof(disk_info->serial));
    strlcpy(disk_info->fw_rev, disk_info_pb->fw_rev, sizeof(disk_info->fw_rev));
    if (disk_info_pb->has_device_type)
        disk_info->device_type = disk_info_pb->device_type;
    else
        disk_info->device_type = 0;

    if (disk_info_pb->ata && disk_info_pb->sas) {
        wire_log(WLOG_INFO, "A disk can't be both ATA and SAS at the same time, skipping");
        bad_disk = true;
    } else if (disk_info_pb->ata) {
        disk_info->disk_type = DISK_TYPE_ATA;
        disk_info->ata.smart_supported = disk_info_pb->ata->smart_supported;
        disk_info->ata.smart_ok = disk_info_pb->ata->smart_ok;
    } else if (disk_info_pb->sas) {
        disk_info->disk_type = DISK_TYPE_SAS;
        disk_info->sas.smart_asc = disk_info_pb->sas->smart_asc;
        disk_info->sas.smart_ascq = disk_info_pb->sas->smart_ascq;
    } else {
        wire_log(WLOG_INFO, "Not an ATA nor SAS disk, skipping");
        bad_disk = true;
    }

    disksurvey__disk_info__free_unpacked(disk_info_pb, NULL);
    return !bad_disk;
}

static void disk_manager_load_fd(int fd)
{
    struct stat statbuf;
    int ret = fstat(fd, &statbuf);
    if (ret < 0) {
        wire_log(WLOG_INFO, "Failed to stat file: %m");
        close(fd);
        return;
    }

    if (statbuf.st_size < 4) {
        wire_log(WLOG_INFO, "Not enough size to include any useful data, ignore it");
        close(fd);
        return;
    }

    unsigned char *buf = mmap(NULL, statbuf.st_size, PROT_READ, MAP_PRIVATE|MAP_POPULATE, fd, 0);
    if (!buf) {
        wire_log(WLOG_INFO, "Failed to map data: %m");
        close(fd);
        return;
    }

	uint32_t version = ntohl(*(uint32_t*)buf);
	if (version != 2) {
		wire_log(WLOG_INFO, "Unknown version of state file, got: %d expected: %d", version, 1);
		goto Exit;
	}

    wire_log(WLOG_INFO, "Loading disk data version %u", version);

    uint32_t offset = sizeof(version);
	int i;
	for (i = 0; i < MAX_DISKS && offset < statbuf.st_size; i++) {
		disk_info_t disk_info;
		latency_t latency;

        if (!disk_manager_load_disk_info(&disk_info, buf, &offset, statbuf.st_size))
            goto Exit;

        if (!disk_manager_load_latency(&latency, buf, &offset, statbuf.st_size))
            goto Exit;

        /* Both parts loaded, add the disk */
		wire_log(WLOG_INFO, "Loaded disk data");
		mgr.disk_list[i].disk.disk_info = disk_info;
        mgr.disk_list[i].disk.latency = latency;
		disk_list_append(i, &mgr.dead_head);
		mgr.first_unused_entry = i+1;
	}

Exit:
    munmap(buf, statbuf.st_size);
}

static void disk_manager_load(void)
{
	int fd = open(mgr.state_file_name, O_RDONLY);
	if (fd < 0) {
		wire_log(WLOG_INFO, "Failed to open state data: %m");
		return;
	}

    disk_manager_load_fd(fd);

	close(fd);
}

void disk_manager_init(void)
{
	// Initialize the disk list
	mgr.first_unused_entry = 0;

	// Initialize the heads
	mgr.alive_head = -1;
	mgr.dead_head = -1;

	snprintf(mgr.state_file_name, sizeof(mgr.state_file_name), "./disksurvey.dat");
	mgr.active = 1;

	wire_pool_init(&mgr.wire_pool, NULL, MAX_DISKS, 4096);

	disk_manager_load();
	system_identifier_read(&mgr.system_id);

	timer_bus_init(&mgr.timer_bus, 1000);
	wire_init(&mgr.task_rescan, "disk rescan", task_rescan, &mgr, WIRE_STACK_ALLOC(64*1024));
	wire_init(&mgr.task_tur, "tur timer", task_tur, &mgr, WIRE_STACK_ALLOC(4096));
	wire_init(&mgr.task_five_min_timer, "five min timer", task_five_min_timer, &mgr, WIRE_STACK_ALLOC(4096));
	wire_init(&mgr.task_dead_disk_reaper, "dead disk reaper", task_dead_disk_reaper, &mgr, WIRE_STACK_ALLOC(4096));
}

static void stop_task(void *arg)
{
	struct disk_mgr *m = arg;

	int disk_idx;
	for_active_disks(disk_idx) {
		disk_t *disk = &m->disk_list[disk_idx].disk;
		wire_log(WLOG_INFO, "Trying to stop disk %d: %p", disk_idx, disk);
		disk_stop(disk);
	}

	while (1) {
		cleanup_dead_disks(m);
		if (m->alive_head == -1)
			break;

		wire_fd_wait_msec(1000);
	}

	wire_log(WLOG_INFO, "No more live disks, stopping");
	disk_manager_save_state_nofork();
}

void disk_manager_stop(void)
{
	mgr.active = 0;

	// Wake up the tasks so they will exit as they notice that the managr is not active anymore
	wire_resume(&mgr.task_rescan);
	timer_bus_stop(&mgr.timer_bus);

	// Now monitor the disks until they can all be stopped
	wire_init(&mgr.task_stop, "stopper", stop_task, &mgr, WIRE_STACK_ALLOC(4096));
}
