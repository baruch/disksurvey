#include <stdlib.h>
#include <check.h>

#include "../src/disk_mgr.c"

#define MARSHALL_FILENAME "test_marshall_file"

void disk_init(disk_t *disk, disk_info_t *disk_info, const char *dev) {}
void disk_cleanup(disk_t *disk) {}
void disk_tick(disk_t *disk) {}
void disk_tur(disk_t *disk) {}
void disk_inquiry(disk_t *disk) {}
void disk_stop(disk_t *disk) {}
int disk_json(disk_t *disk, char *buf, int len) {return 0;}
bool disk_scanner_active(disk_scanner_t *disk_scanner) { return false; }
void disk_scanner_inquiry(disk_scanner_t *disk, const char *sg_dev, scanner_done_cb done_cb) {}
bool system_identifier_read(system_identifier_t *system_id) { memset(system_id, 0, sizeof(*system_id)); return true; }

EV_API_DECL void ev_async_send     (EV_P_ ev_async *w) {}
EV_API_DECL void ev_timer_start    (EV_P_ ev_timer *w) {}
EV_API_DECL void ev_async_start    (EV_P_ ev_async *w) {}
EV_API_DECL void ev_timer_stop     (EV_P_ ev_timer *w) {}
EV_API_DECL void ev_break (EV_P_ int how EV_CPP (= EVBREAK_ONE)) {}
EV_API_DECL void ev_signal_start   (EV_P_ ev_signal *w) {}

static void setup(void)
{
    disk_manager_init_mgr();
}

static void teardown(void)
{
}

static void setup_marshall(void)
{
    setup();
    unlink(MARSHALL_FILENAME);
}

static void teardown_marshall(void)
{
    teardown();
    unlink(MARSHALL_FILENAME);
}

START_TEST(test_disk_mgr_init_mgr)
{
    fail_unless(mgr.alive_head == -1, "Alive list must be empty");
    fail_unless(mgr.dead_head == -1, "Dead list must be empty");
    fail_unless(mgr.first_unused_entry == 0, "First unused entry is zero");
}
END_TEST

START_TEST(test_disk_list_first_disk_add)
{
    int disk_idx = disk_list_get_unused();
    fail_unless(disk_idx == 0, "First disk allocated must get the first entry in the list");
    fail_unless(mgr.alive_head == -1, "Couldn't be that the alive head has anything in it");
    
    disk_list_append(disk_idx, &mgr.alive_head);
    fail_unless(mgr.alive_head == 0, "We just added the first disk to the list, it must be there!");
    fail_unless(mgr.disk_list[0].prev == -1, NULL);
    fail_unless(mgr.disk_list[0].next == -1, NULL);
}
END_TEST

START_TEST(test_disk_list_second_disk_add)
{
    int disk_idx = disk_list_get_unused();
    fail_unless(disk_idx == 0, "First disk allocated must get the first entry in the list");
    disk_list_append(disk_idx, &mgr.alive_head);
    fail_unless(mgr.alive_head == 0, "Alive list head must be zero");

    disk_idx = disk_list_get_unused();
    fail_unless(disk_idx == 1, "Second disk allocated must get the second entry in the list");
    disk_list_append(disk_idx, &mgr.alive_head);
    fail_unless(mgr.alive_head == 0, "Alive list head must stay the same");
    fail_unless(mgr.disk_list[0].next == 1, "The first entry must point to the second");
    fail_unless(mgr.disk_list[0].prev == -1, NULL);
    fail_unless(mgr.disk_list[1].next == -1, NULL);
    fail_unless(mgr.disk_list[1].prev == 0, NULL);
}
END_TEST

START_TEST(test_disk_list_second_disk_removal)
{
    int disk_idx = disk_list_get_unused();
    disk_list_append(disk_idx, &mgr.alive_head);
    disk_idx = disk_list_get_unused();
    disk_list_append(disk_idx, &mgr.alive_head);

    disk_list_remove(disk_idx, &mgr.alive_head);
    fail_unless(mgr.alive_head == 0, "Alive list head must keep pointing to the first element");
    fail_unless(mgr.disk_list[0].next == -1, "First element must now point to end of list");
    fail_unless(mgr.disk_list[1].prev == -1, "Second element must not point anyway previously");
    fail_unless(mgr.disk_list[1].next == -1, "Second element must not point anyway next");
}
END_TEST

START_TEST(test_disk_list_first_disk_removal)
{
    int disk_idx = disk_list_get_unused();
    disk_list_append(disk_idx, &mgr.alive_head);
    disk_idx = disk_list_get_unused();
    disk_list_append(disk_idx, &mgr.alive_head);

    disk_list_remove(0, &mgr.alive_head);
    fail_unless(mgr.alive_head == 1, NULL);
    fail_unless(mgr.disk_list[0].prev == -1, NULL);
    fail_unless(mgr.disk_list[0].next == -1, NULL);
    fail_unless(mgr.disk_list[1].prev == -1, NULL);
    fail_unless(mgr.disk_list[1].prev == -1, NULL);
}
END_TEST

START_TEST(test_disk_list_last_disk_removal)
{
    int disk_idx = disk_list_get_unused();
    fail_unless(disk_idx == 0, NULL);
    disk_list_append(disk_idx, &mgr.alive_head);
    disk_list_remove(disk_idx, &mgr.alive_head);
    fail_unless(mgr.alive_head == -1, NULL);
    fail_unless(mgr.disk_list[0].prev == -1, NULL);
    fail_unless(mgr.disk_list[0].next == -1, NULL);
}
END_TEST

START_TEST(test_marshall_save)
{
    int fd = creat("test_disk_marshall", 0600);
    fail_unless(fd > 0);

    disk_t disk;
    bool save_success = disk_manager_save_disk_state(&disk, fd);
    close(fd);
    fail_unless(save_success == true);

    struct stat statbuf;
    fd = open("test_disk_marshall", O_RDONLY);
    fail_unless(fd >= 0);
    int stat_ret = fstat(fd, &statbuf);
    close(fd);
    fail_unless(stat_ret >= 0);
    fail_unless(statbuf.st_size > 0);
}
END_TEST

START_TEST(test_marshall_disk_info)
{
    int fd = creat("test_disk_marshall", 0600);
    fail_unless(fd > 0);

    disk_info_t disk_info = {
        .vendor = "VENDOR",
        .model = "MODEL",
        .fw_rev = "AB92",
        .serial = "KFHFKNC32221A",
        .device_type = 4,
        .disk_type = DISK_TYPE_ATA,
        .ata.smart_ok = true,
        .ata.smart_supported = true,
    };

    bool save_success = disk_manager_save_disk_info(&disk_info, fd);
    close(fd);
    fail_unless(save_success == true);

    unsigned char *buf;

    fd = open("test_disk_marshall", O_RDONLY);
    fail_unless(fd >= 0);
    struct stat statbuf;
    int ret = fstat(fd, &statbuf);
    fail_unless(ret >= 0);
    buf = malloc(statbuf.st_size);
    ret = read(fd, buf, statbuf.st_size);
    close(fd);
    fail_unless(ret == statbuf.st_size);

    uint32_t offset = 0;
    disk_info_t disk_info_load;
    memset(&disk_info_load, 0, sizeof(disk_info_load));

    bool success = disk_manager_load_disk_info(&disk_info_load, buf, &offset, statbuf.st_size);
    fail_unless(success == true);
    fail_unless(offset == statbuf.st_size, "file size is %u and final offset is %u, diff of %d", statbuf.st_size, offset, (int)(statbuf.st_size - offset));

    // Compare the data
    ck_assert_str_eq(disk_info.vendor, disk_info_load.vendor);
    ck_assert_str_eq(disk_info.model, disk_info_load.model);
    ck_assert_str_eq(disk_info.serial, disk_info_load.serial);
    ck_assert_str_eq(disk_info.fw_rev, disk_info_load.fw_rev);
    ck_assert_int_eq(disk_info.device_type, disk_info_load.device_type);
    ck_assert_int_eq(disk_info.disk_type, disk_info_load.disk_type);

    // Catch all comparison
    fail_unless(memcmp(&disk_info, &disk_info_load, sizeof(disk_info)) == 0);
}
END_TEST

START_TEST(test_marshall_latency)
{
    int fd = creat("test_disk_marshall", 0600);
    fail_unless(fd > 0);

    latency_t latency;
    bool save_success = disk_manager_save_disk_latency(&latency, fd);
    close(fd);
    fail_unless(save_success == true);

    unsigned char *buf;

    fd = open("test_disk_marshall", O_RDONLY);
    fail_unless(fd >= 0);
    struct stat statbuf;
    int ret = fstat(fd, &statbuf);
    fail_unless(ret >= 0);
    buf = malloc(statbuf.st_size);
    ret = read(fd, buf, statbuf.st_size);
    close(fd);
    fail_unless(ret == statbuf.st_size);

    uint32_t offset = 0;
    latency_t latency_load;
    memset(&latency_load, 0, sizeof(latency_load));

    bool success = disk_manager_load_latency(&latency_load, buf, &offset, statbuf.st_size);
    fail_unless(success == true);
    fail_unless(offset == statbuf.st_size);
    fail_unless(memcmp(&latency, &latency_load, sizeof(latency)) == 0);
}
END_TEST

Suite *disk_mgr_suite(void)
{
  Suite *s = suite_create("Disk Manager");

  /* Core test case */
  TCase *tc_disk_list = tcase_create("Disk List");
  tcase_add_checked_fixture(tc_disk_list, setup, teardown);
  tcase_add_test(tc_disk_list, test_disk_mgr_init_mgr);
  tcase_add_test(tc_disk_list, test_disk_list_first_disk_add);
  tcase_add_test(tc_disk_list, test_disk_list_second_disk_add);
  tcase_add_test(tc_disk_list, test_disk_list_second_disk_removal);
  tcase_add_test(tc_disk_list, test_disk_list_first_disk_removal);
  tcase_add_test(tc_disk_list, test_disk_list_last_disk_removal);
  suite_add_tcase(s, tc_disk_list);

  /* Test marshall/unmarshall */
  TCase *tc_marshall = tcase_create("Marshalling");
  tcase_add_checked_fixture(tc_marshall, setup_marshall, teardown_marshall);
  tcase_add_test(tc_marshall, test_marshall_save);
  tcase_add_test(tc_marshall, test_marshall_disk_info);
  tcase_add_test(tc_marshall, test_marshall_latency);
  suite_add_tcase(s, tc_marshall);

  return s;
}

int main(void)
{
    int number_failed;
    Suite *s = disk_mgr_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return number_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
