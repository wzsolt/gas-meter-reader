#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Serialized, bounded sync. exchange() commits the matching HA ACK before
 * returning success. prepare() must persist an immutable outbox before sending. */
typedef struct {
    void *context;
    int64_t (*now)(void *context);
    int (*prepare)(void *context, int battery_mv);
    int (*connect)(void *context, int64_t deadline);
    int (*exchange)(void *context, int64_t deadline);
    bool (*pending)(void *context);
    bool refresh_saved_packet; /* Daily heartbeat needs current battery/version after an old packet. */
    int timeout_error;
} gr_sync_drain_ops_t;

int gr_sync_drain(const gr_sync_drain_ops_t *ops, int battery_mv, int64_t deadline);
