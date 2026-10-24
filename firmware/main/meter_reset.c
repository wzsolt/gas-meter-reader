#include "meter_reset.h"

int gr_meter_reset_sync(const gr_meter_reset_ops_t *ops, int battery_mv, int64_t deadline) {
    const gr_sync_drain_ops_t *s = &ops->sync;
    bool connected = false;
    if (s->now(s->context) >= deadline) return s->timeout_error;
    if (ops->active(s->context) && !ops->reset_pending(s->context)) {
        int err = s->connect(s->context, deadline);
        if (err) return err;
        connected = true;
        if (s->now(s->context) >= deadline) return s->timeout_error;
        err = s->exchange(s->context, deadline);
        if (err) return err;
    }
    if (s->now(s->context) >= deadline) return s->timeout_error;
    int err = ops->prepare_reset(s->context, battery_mv);
    if (err) return err;
    if (s->now(s->context) >= deadline) return s->timeout_error;
    if (!connected) {
        err = s->connect(s->context, deadline);
        if (err) return err;
    }
    if (s->now(s->context) >= deadline) return s->timeout_error;
    return s->exchange(s->context, deadline);
}
