#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define GR_STATE_VERSION 1u
#define GR_DEBOUNCE_US 30000LL
#define GR_INTERVAL_US 1800000000LL
#define GR_MAX_VERSION 32
#define GR_OUTBOX_REPORT 0u
#define GR_OUTBOX_LEGACY_RESET 1u
#define GR_OUTBOX_RESET 2u

typedef struct {
    uint64_t sequence;
    uint64_t pulses;
    int32_t battery_mv;
    char firmware[GR_MAX_VERSION];
    uint8_t active;
    uint8_t reserved[7];
} gr_outbox_t;

/* One blob is the transaction boundary. Keep this layout compatible across OTA. */
typedef struct {
    uint32_t version;
    uint32_t crc;
    uint64_t generation;
    uint64_t pending;
    uint64_t next_sequence;
    int64_t next_sync_us;
    gr_outbox_t outbox;
    char instance[33];
    uint8_t reed_level;
    uint8_t button_consumed;
    uint8_t counter_fault;
    uint8_t reserved[4];
} gr_state_t;

typedef struct {
    int stable;
    int candidate;
    int64_t since_us;
} gr_debounce_t;

uint32_t gr_crc(const void *data, size_t length);
void gr_seal(gr_state_t *s);
bool gr_valid(const gr_state_t *s);
void gr_state_init(gr_state_t *s, const char *instance, int reed, int64_t now);
void gr_debounce_init(gr_debounce_t *d, int stable, int sample, int64_t now);
/* Return true only on an accepted high -> low transition. */
bool gr_debounce_sample(gr_debounce_t *d, int level, int64_t now);
bool gr_prepare(gr_state_t *s, int battery_mv, const char *firmware);
bool gr_prepare_reset(gr_state_t *s, int battery_mv, const char *firmware);
bool gr_outbox_is_reset(const gr_outbox_t *outbox);
bool gr_ack_candidate(const gr_state_t *s, uint64_t sequence, gr_state_t *candidate);
int64_t gr_sleep_remaining(const gr_state_t *s, int64_t now);
bool gr_automatic_sync_due(const gr_state_t *s, int64_t now);
