#include "maintenance_control.h"
#include <assert.h>
#include <stdio.h>

static void test_entry_press_does_not_exit(void) {
    gr_exit_button_t b; gr_exit_button_init(&b, false, 0);
    assert(!gr_exit_button_sample(&b, false, 30000));
    assert(!gr_exit_button_sample(&b, false, 60000000));
    /* A brief HIGH bounce must not rearm the entry press. */
    assert(!gr_exit_button_sample(&b, true, 60010000));
    assert(!gr_exit_button_sample(&b, false, 60020000));
    assert(!gr_exit_button_sample(&b, false, 70000000));
    /* Release for 30 ms, then a fresh short press. Exit on stable release. */
    assert(!gr_exit_button_sample(&b, true, 70010000));
    assert(!gr_exit_button_sample(&b, true, 70040000));
    assert(!gr_exit_button_sample(&b, false, 70100000));
    assert(!gr_exit_button_sample(&b, false, 70130000));
    assert(!gr_exit_button_sample(&b, true, 70200000));
    assert(!gr_exit_button_sample(&b, true, 70229999));
    assert(gr_exit_button_sample(&b, true, 70230000));
    assert(!gr_exit_button_sample(&b, true, 90000000));
}

static void test_short_press_and_release_bounce(void) {
    gr_exit_button_t b; gr_exit_button_init(&b, true, 0);
    assert(!gr_exit_button_sample(&b, true, 30000));
    assert(!gr_exit_button_sample(&b, false, 100000));
    assert(!gr_exit_button_sample(&b, false, 130000));
    assert(!gr_exit_button_sample(&b, true, 200000));
    assert(!gr_exit_button_sample(&b, false, 210000));
    assert(!gr_exit_button_sample(&b, true, 220000));
    assert(gr_exit_button_sample(&b, true, 250000));
    assert(!gr_exit_button_sample(&b, true, 260000));
    assert(!gr_exit_button_sample(&b, false, 2000000));
    assert(!gr_exit_button_sample(&b, false, 2030000));
    /* A long press is not a short-press exit; another short press still works. */
    assert(!gr_exit_button_sample(&b, true, 4000000));
    assert(!gr_exit_button_sample(&b, false, 4010000));
    assert(!gr_exit_button_sample(&b, false, 6999999));
    assert(!gr_exit_button_sample(&b, false, 7000000));
    assert(!gr_exit_button_sample(&b, true, 7100000));
    assert(!gr_exit_button_sample(&b, true, 7130000));
    assert(!gr_exit_button_sample(&b, false, 8000000));
    assert(!gr_exit_button_sample(&b, true, 8020000)); /* LOW too short to debounce. */
    assert(!gr_exit_button_sample(&b, true, 8050000));
    assert(!gr_exit_button_sample(&b, false, 9000000));
    assert(!gr_exit_button_sample(&b, false, 9030000));
    assert(!gr_exit_button_sample(&b, true, 9100000));
    assert(gr_exit_button_sample(&b, true, 9130000));
}

static void test_exit_closes_admission_and_waits_for_upload(void) {
    gr_portal_state_t s = {0};
    assert(gr_portal_begin_upload(&s));
    assert(!gr_portal_begin_upload(&s));
    gr_portal_request_exit(&s);
    assert(!gr_portal_can_exit(&s));
    assert(!gr_portal_begin_upload(&s));
    gr_portal_finish_upload(&s, false);
    assert(gr_portal_can_exit(&s));
    assert(!gr_portal_begin_upload(&s));
}

static void test_valid_ota_keeps_reboot_priority(void) {
    gr_portal_state_t s = {0};
    assert(gr_portal_begin_upload(&s));
    gr_portal_request_exit(&s);
    gr_portal_finish_upload(&s, true);
    assert(s.reboot_requested);
    assert(!gr_portal_can_exit(&s));
    assert(!gr_portal_begin_upload(&s));
    s = (gr_portal_state_t){0};
    gr_portal_request_exit(&s);
    assert(gr_portal_can_exit(&s));
    assert(!gr_portal_begin_upload(&s));
}
static void test_reset_blocks_exit_and_ota(void) {
    gr_portal_state_t s = {0};
    assert(gr_portal_begin_reset(&s));
    assert(!gr_portal_begin_reset(&s));
    assert(!gr_portal_begin_upload(&s));
    gr_portal_request_exit(&s);
    assert(!gr_portal_can_exit(&s));
    gr_portal_finish_reset(&s);
    assert(gr_portal_can_exit(&s));
    assert(!gr_portal_begin_reset(&s));
    s = (gr_portal_state_t){0};
    assert(gr_portal_begin_upload(&s));
    assert(!gr_portal_begin_reset(&s));
}

int main(void) {
    test_entry_press_does_not_exit();
    test_short_press_and_release_bounce();
    test_exit_closes_admission_and_waits_for_upload();
    test_valid_ota_keeps_reboot_priority();
    test_reset_blocks_exit_and_ota();
    puts("Maintenance button debounce, exit admission and OTA priority tests passed.");
}
