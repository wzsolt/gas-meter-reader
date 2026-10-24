#pragma once
#include "sync_drain.h"

typedef struct {
    gr_sync_drain_ops_t sync;
    bool (*active)(void *context);
    bool (*reset_pending)(void *context);
    int (*prepare_reset)(void *context, int battery_mv);
} gr_meter_reset_ops_t;

/* Resolve an older report before a reset. Retry an existing reset unchanged.
 * Pulses arriving after the discard snapshot remain pending for normal sync. */
int gr_meter_reset_sync(const gr_meter_reset_ops_t *ops, int battery_mv, int64_t deadline);
