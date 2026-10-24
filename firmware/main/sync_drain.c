#include "sync_drain.h"

int gr_sync_drain(const gr_sync_drain_ops_t *ops, int battery_mv, int64_t deadline) {
    bool connected = false;
    bool refresh = ops->refresh_saved_packet;
    for (;;) {
        if (ops->now(ops->context) >= deadline) return ops->timeout_error;
        int err = ops->prepare(ops->context, battery_mv);
        if (err) return err;
        if (ops->now(ops->context) >= deadline) return ops->timeout_error;
        if (!connected) {
            err = ops->connect(ops->context, deadline);
            if (err) return err;
            connected = true;
        }
        if (ops->now(ops->context) >= deadline) return ops->timeout_error;
        err = ops->exchange(ops->context, deadline);
        if (err) return err;
        if (!ops->pending(ops->context) && !refresh) return 0;
        refresh = false;
        /* ACK was committed; the next iteration saves a NEW packet using only
         * the remaining pulses. The deadline is never restarted. */
    }
}
