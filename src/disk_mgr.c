#include "disk_mgr.h"
#include "disk.h"

#include <glob.h>
#include <stdio.h>
#include <string.h>

#define MAX_DISKS 120

struct disk_mgr {
	struct ev_loop *loop;
	ev_timer periodic_rescan_timer;
	ev_timer tur_timer;
	ev_async cleanup_dead_disks;
	disk_t disk[MAX_DISKS];
	bool dead_disk[MAX_DISKS];
};
static struct disk_mgr mgr;

static void cleanup_dead_disks(struct ev_loop *loop, ev_async *watcher, int revents)
{
	printf("Cleanup dead disks started\n");
	int disk_idx;
	for (disk_idx = 0; disk_idx < MAX_DISKS; disk_idx++) {
		if (mgr.dead_disk[disk_idx]) {
			mgr.dead_disk[disk_idx] = false;

			disk_t *disk = &mgr.disk[disk_idx];
			printf("Cleaning dead disk %p\n", disk);
			disk_cleanup(disk);
		}
	}
	printf("Cleanup dead disks finished\n");
}

static void on_death(disk_t *disk)
{
	int disk_idx;
	for (disk_idx = 0; disk_idx < MAX_DISKS; disk_idx++) {
		disk_t *cur_disk = &mgr.disk[disk_idx];
		if (cur_disk == disk) {
			printf("Marking disk %p as dead for cleanup\n", disk);
			mgr.dead_disk[disk_idx] = true;
			break;
		}
	}
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
	int first_free_disk = MAX_DISKS;

	for (glob_idx = 0; glob_idx < globbuf.gl_pathc; glob_idx++) {
		char *dev = globbuf.gl_pathv[glob_idx];
		bool found = false;
		printf("\tDevice: %s - ", dev);

		int disk_idx;
		for (disk_idx = 0; disk_idx < MAX_DISKS; disk_idx++) {
			char *sg_path = mgr.disk[disk_idx].sg_path;
			if (strcmp(dev, sg_path) == 0) {
				printf("already known\n");
				found = true;
				break;
			} else if (first_free_disk == MAX_DISKS && sg_path[0] == '\0') {
				first_free_disk = disk_idx;
			}
		}

		if (!found) {
			if (first_free_disk != MAX_DISKS) {
				printf("adding idx=%d!\n", first_free_disk);
				disk_t *disk = &mgr.disk[first_free_disk];
				disk_init(disk, dev);
				disk->on_death = on_death;
				first_free_disk = MAX_DISKS; // Spot not empty anymore
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
	for (disk_idx = 0; disk_idx < MAX_DISKS; disk_idx++) {
		if (mgr.disk[disk_idx].sg_path[0])
			disk_tur(&mgr.disk[disk_idx]);
	}
}

void disk_manager_init(struct ev_loop *loop)
{
	memset(&mgr, 0, sizeof(mgr));

	mgr.loop = loop;
	
	// The first event will be 0 seconds into the event loop and then once every hour
	ev_timer *timer = &mgr.periodic_rescan_timer;
	ev_timer_init(timer, rescan_cb, 0, 60*60);
	ev_timer_start(loop, &mgr.periodic_rescan_timer);

	timer = &mgr.tur_timer;
	ev_timer_init(timer, tur_timer, 0.0, 1.0);
	ev_timer_start(EV_DEFAULT, timer);

	ev_async *async = &mgr.cleanup_dead_disks;
	ev_async_init(async, cleanup_dead_disks);
	ev_async_start(loop, async);
}
