#include <stdlib.h>
#include <check.h>

#include "../src/disk_mgr.c"

void disk_init(disk_t *disk, const char *dev) {}
void disk_cleanup(disk_t *disk) {}
void disk_tick(disk_t *disk) {}
void disk_tur(disk_t *disk) {}
void disk_inquiry(disk_t *disk) {}
int disk_json(disk_t *disk, char *buf, int len) {return 0;}


EV_API_DECL void ev_async_send     (EV_P_ ev_async *w) {}
EV_API_DECL void ev_timer_start    (EV_P_ ev_timer *w) {}
EV_API_DECL void ev_async_start    (EV_P_ ev_async *w) {}


void setup(void)
{
    disk_manager_init_mgr();
}

void teardown(void)
{
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
    disk_list_append(disk_idx, &mgr.alive_head);
    disk_list_remove(disk_idx, &mgr.alive_head);
    fail_unless(mgr.alive_head == -1, NULL);
    fail_unless(mgr.disk_list[0].prev == -1, NULL);
    fail_unless(mgr.disk_list[0].next == -1, NULL);
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

#if 0
  /* Limits test case */
  TCase *tc_limits = tcase_create ("Limits");
  tcase_add_test (tc_limits, test_money_create_neg);
  tcase_add_test (tc_limits, test_money_create_zero);
  suite_add_tcase (s, tc_limits);
#endif

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
