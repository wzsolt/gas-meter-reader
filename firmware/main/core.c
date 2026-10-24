#include "core.h"
#include <string.h>

uint32_t gr_crc(const void *data, size_t length) {
    const uint8_t *p = data;
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < length; i++) {
        crc ^= p[i];
        for (int j = 0; j < 8; j++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

void gr_seal(gr_state_t *s) {
    s->crc = 0;
    s->crc = gr_crc(s, sizeof(*s));
}

bool gr_valid(const gr_state_t *s) {
    if (s->version != GR_STATE_VERSION || !s->next_sequence || s->reed_level > 1 ||
        s->instance[32] != '\0' || strlen(s->instance) != 32 ||
        s->outbox.active > 1 || s->outbox.reserved[0] > GR_OUTBOX_RESET || s->outbox.firmware[GR_MAX_VERSION-1] != '\0' ||
        (s->outbox.active && (!s->outbox.sequence || s->outbox.sequence >= s->next_sequence ||
                             s->outbox.pulses > s->pending))) return false;
    gr_state_t copy = *s;
    uint32_t expected = copy.crc;
    gr_seal(&copy);
    return expected == copy.crc;
}

void gr_state_init(gr_state_t *s, const char *instance, int reed, int64_t now) {
    memset(s, 0, sizeof(*s));
    s->version = GR_STATE_VERSION;
    s->next_sequence = 1;
    s->reed_level = !!reed;
    memcpy(s->instance, instance, 32);
    s->next_sync_us = now + GR_INTERVAL_US;
    gr_seal(s);
}

void gr_debounce_init(gr_debounce_t *d, int stable, int sample, int64_t now) {
    *d = (gr_debounce_t){.stable = !!stable, .candidate = !!sample, .since_us = now};
}

bool gr_debounce_sample(gr_debounce_t *d, int level, int64_t now) {
    bool pulse = false;
    /* Commit the interval preceding this edge, including edges buffered during flash writes. */
    if (d->candidate != d->stable && now - d->since_us >= GR_DEBOUNCE_US) {
        pulse = d->stable == 1 && d->candidate == 0;
        d->stable = d->candidate;
    }
    level = !!level;
    if (level != d->candidate) { d->candidate = level; d->since_us = now; }
    return pulse;
}

bool gr_prepare(gr_state_t *s, int battery_mv, const char *firmware) {
    if (s->outbox.active) return true;
    if (s->next_sequence == UINT64_MAX || s->counter_fault) return false;
    memset(&s->outbox, 0, sizeof(s->outbox));
    s->outbox.sequence = s->next_sequence++;
    s->outbox.pulses = s->pending;
    s->outbox.battery_mv = battery_mv;
    strncpy(s->outbox.firmware, firmware, GR_MAX_VERSION - 1);
    s->outbox.active = 1;
    return true;
}

bool gr_ack_candidate(const gr_state_t *s, uint64_t sequence, gr_state_t *candidate) {
    if (!s->outbox.active || s->outbox.sequence != sequence || s->pending < s->outbox.pulses) return false;
    *candidate = *s;
    candidate->pending -= s->outbox.pulses;
    memset(&candidate->outbox, 0, sizeof(candidate->outbox));
    return true;
}

bool gr_outbox_is_reset(const gr_outbox_t *outbox) {
    return outbox->active && (outbox->reserved[0] == GR_OUTBOX_RESET ||
        outbox->reserved[0] == GR_OUTBOX_LEGACY_RESET);
}

bool gr_prepare_reset(gr_state_t *s, int battery_mv, const char *firmware) {
    if (s->outbox.active) return gr_outbox_is_reset(&s->outbox);
    if (!gr_prepare(s, battery_mv, firmware)) return false;
    /* Use a previously reserved byte: the persistent blob layout stays unchanged. */
    s->outbox.reserved[0] = GR_OUTBOX_RESET;
    return true;
}

int64_t gr_sleep_remaining(const gr_state_t *s, int64_t now) {
    return s->next_sync_us > now ? s->next_sync_us - now : 1;
}

bool gr_automatic_sync_due(const gr_state_t *s, int64_t now) {
    /* A failed manual zero-pulse status report must not create idle radio retries. */
    return now >= s->next_sync_us && (s->pending > 0 || gr_outbox_is_reset(&s->outbox));
}
