#include "core.h"
#include "image_check.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *id = "0123456789abcdef0123456789abcdef";
static void test_debounce(void) {
    gr_debounce_t d;
    gr_debounce_init(&d, 1, 1, 0);
    assert(!gr_debounce_sample(&d, 0, 1000));
    assert(!gr_debounce_sample(&d, 1, 10000));
    assert(!gr_debounce_sample(&d, 0, 12000));
    assert(!gr_debounce_sample(&d, 0, 41999));
    assert(gr_debounce_sample(&d, 0, 42000));
    assert(!gr_debounce_sample(&d, 0, 3600000000LL)); /* hour-long closure */
    assert(!gr_debounce_sample(&d, 1, 3600001000LL));
    assert(!gr_debounce_sample(&d, 0, 3600010000LL)); /* bouncing release is not a new pulse */
    assert(!gr_debounce_sample(&d, 1, 3600020000LL));
    assert(!gr_debounce_sample(&d, 1, 3600050000LL));
    assert(!gr_debounce_sample(&d, 0, 3600060000LL));
    assert(gr_debounce_sample(&d, 0, 3600090000LL));
    gr_debounce_init(&d, 0, 0, 0); /* closed at cold boot */
    assert(!gr_debounce_sample(&d, 0, 100000));
    gr_debounce_init(&d, 1, 0, 0); /* deep wake from a closure */
    assert(gr_debounce_sample(&d, 0, 30000));
}

static void test_buffered_edges(void) {
    gr_debounce_t d; gr_debounce_init(&d, 1, 1, 0);
    assert(!gr_debounce_sample(&d, 0, 1000));
    assert(gr_debounce_sample(&d, 1, 100000)); /* interval accepted when flash ends */
    assert(!gr_debounce_sample(&d, 0, 200000));
    assert(gr_debounce_sample(&d, 1, 300000));
}

static void test_transaction(void) {
    gr_state_t s; gr_state_init(&s, id, 1, 0);
    assert(gr_valid(&s));
    assert(gr_sleep_remaining(&s, 1000000) == GR_INTERVAL_US - 1000000);
    assert(gr_sleep_remaining(&s, GR_INTERVAL_US + 1) == 1);
    assert(!gr_automatic_sync_due(&s, GR_INTERVAL_US));
    assert(gr_prepare(&s, 3910, "1.0.0")); /* failed manual zero-pulse report */
    assert(!gr_automatic_sync_due(&s, GR_INTERVAL_US));
    s.pending = 1;
    assert(!gr_automatic_sync_due(&s, GR_INTERVAL_US - 1));
    assert(gr_automatic_sync_due(&s, GR_INTERVAL_US));
    gr_state_init(&s, id, 1, 0);
    s.pending = 4;
    assert(gr_prepare(&s, 3910, "1.0.0"));
    assert(s.outbox.sequence == 1 && s.next_sequence == 2 && s.outbox.pulses == 4);
    gr_seal(&s); gr_state_t disk = s;
    assert(gr_valid(&disk));
    s.pending += 2; /* pulses while radio is active */
    gr_outbox_t packet = s.outbox;
    assert(gr_prepare(&s, 3700, "2.0.0"));
    assert(!memcmp(&packet, &s.outbox, sizeof(packet)));
    gr_state_t candidate;
    assert(!gr_ack_candidate(&s, 2, &candidate));
    assert(gr_ack_candidate(&s, 1, &candidate));
    assert(candidate.pending == 2 && !candidate.outbox.active);
    assert(s.pending == 6 && s.outbox.active); /* failed flash write cannot mutate live state */
    /* Reset before ACK commit restores the old packet; HA will deduplicate it. */
    assert(disk.pending == 4 && disk.outbox.sequence == 1);
    s = candidate; gr_seal(&s);
    assert(gr_valid(&s));
    assert(!gr_ack_candidate(&s, 1, &candidate));
    assert(gr_prepare(&s, 3900, "1.0.0") && s.outbox.sequence == 2 && s.outbox.pulses == 2);
    s.pending = 0; gr_seal(&s);
    assert(!gr_valid(&s)); /* impossible outbox invariant, even with a matching CRC */
    gr_state_init(&s, id, 1, 0); s.pending++; assert(!gr_valid(&s));
    gr_state_init(&s, id, 1, 0); s.next_sequence = UINT64_MAX;
    assert(!gr_prepare(&s, 3900, "1.0.0"));
    gr_state_init(&s, id, 1, 0); s.counter_fault = 1;
    assert(!gr_prepare(&s, 3900, "1.0.0"));
}

static void test_ota_header(void) {
    uint8_t image[GR_IMAGE_PREFIX_SIZE] = {0};
    image[0] = 0xe9; image[1] = 3; image[12] = 0x0d;
    image[32] = 0x32; image[33] = 0x54; image[34] = 0xcd; image[35] = 0xab;
    memcpy(image + 80, "gas_reader", 10);
    assert(gr_image_prefix_valid(image, sizeof(image), 1000, 2000));
    assert(!gr_image_prefix_valid(image, 100, 1000, 2000));
    assert(!gr_image_prefix_valid(image, sizeof(image), 3000, 2000));
    image[12] = 5; assert(!gr_image_prefix_valid(image, sizeof(image), 1000, 2000));
    image[12] = 0x0d; image[80] = 'x'; assert(!gr_image_prefix_valid(image, sizeof(image), 1000, 2000));
    image[0] = 0xff; assert(!gr_image_prefix_valid(image, sizeof(image), 1000, 2000));
}

int main(void) {
    test_debounce(); test_buffered_edges(); test_transaction(); test_ota_header();
    puts("Firmware core: debounce, buffered edges, deadlines, CRC, transactions and OTA headers passed.");
}
