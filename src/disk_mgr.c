#include "disk_mgr.h"
#include "disk.h"
#include "util.h"

#include <glob.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#define MAX_DISKS 120

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
	ev_async cleanup_dead_disks;

	int alive_head;
	int dead_head;
	int first_unused_entry;
	struct disk_state disk_list[MAX_DISKS];
};
static struct disk_mgr mgr;

#define for_active_disks(_idx_) \
	for (_idx_ = mgr.alive_head; _idx_ != -1; _idx_ = mgr.disk_list[_idx_].next)

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
	int disk_idx;

	for_active_disks(disk_idx) {
		if (mgr.disk_list[disk_idx].died) {
			mgr.disk_list[disk_idx].died = false;

			disk_list_remove(disk_idx, &mgr.alive_head);
			disk_list_append(disk_idx, &mgr.dead_head);

			// TODO: When cleaning a dead disk, keep the useful info in it for a while, it may yet return
			disk_t *disk = &mgr.disk_list[disk_idx].disk;
			printf("Cleaning dead disk %p\n", disk);
			disk_cleanup(disk);
		}
	}
	printf("Cleanup dead disks finished\n");
}

static void on_death(disk_t *disk)
{
	struct disk_state *state = container_of(disk, struct disk_state, disk);
	state->died = true;
	printf("Marking disk %p as dead for cleanup\n", disk);
	ev_async_send(mgr.loop, &mgr.cleanup_dead_disks);
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
		bool found = false;
		printf("\tDevice: %s - ", dev);

		int disk_idx;
		for_active_disks(disk_idx) {
			char *sg_path = mgr.disk_list[disk_idx].disk.sg_path;
			if (strcmp(dev, sg_path) == 0) {
				printf("already known\n");
				found = true;
				break;
			}
		}

		if (!found) {
			int new_disk_idx = disk_list_get_unused();
			if (new_disk_idx != -1) {
				printf("adding idx=%d!\n", new_disk_idx);
				disk_t *disk = &mgr.disk_list[new_disk_idx].disk;
				disk_init(disk, dev);
				disk->on_death = on_death;
				disk_list_append(new_disk_idx, &mgr.alive_head);
			} else {
				printf("Want to add but no space!\n");
			}
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
}

static void disk_manager_init_mgr(void)
{
	// Initialize the disk list
	mgr.first_unused_entry = 0;

	// Initialize the heads
	mgr.alive_head = -1;
	mgr.dead_head = -1;
}

void disk_manager_init(struct ev_loop *loop)
{
	disk_manager_init_mgr();

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
}
