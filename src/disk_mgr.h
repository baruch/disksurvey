#ifndef DISKSURVEY_MGR_H
#define DISKSURVEY_MGR_H

#include <ev.h>

void disk_manager_init(struct ev_loop *loop);
void disk_manager_rescan(void);
int disk_manager_disk_list_json(char *buf, int len);
void disk_manager_stop(void);

#endif
