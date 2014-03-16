#ifndef DISKSURVEY_MGR_H
#define DISKSURVEY_MGR_H

void disk_manager_init(void);
void disk_manager_rescan(void);
int disk_manager_disk_list_json(char *buf, int len);
void disk_manager_stop(void);
void disk_manager_save_state(void);

#endif
