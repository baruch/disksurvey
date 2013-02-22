#include "disk_mgr.h"
#include "disk.h"

#include <glob.h>
#include <stdio.h>
#include <string.h>

#define MAX_DISKS 5

struct disk_mgr {
	struct ev_loop *loop;
	ev_timer periodic_rescan_timer;
	ev_timer tur_timer;
	disk_t disk[MAX_DISKS];
};
static struct disk_mgr mgr;

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
				disk_init(&mgr.disk[first_free_disk], dev);
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
	mgr.loop = loop;
	
	// The first event will be 0 seconds into the event loop and then once every hour
	ev_timer *timer = &mgr.periodic_rescan_timer;
	ev_timer_init(timer, rescan_cb, 0, 60*60);
	ev_timer_start(loop, &mgr.periodic_rescan_timer);

	timer = &mgr.tur_timer;
	ev_timer_init(timer, tur_timer, 0.0, 1.0);
	ev_timer_start(EV_DEFAULT, timer);
}
