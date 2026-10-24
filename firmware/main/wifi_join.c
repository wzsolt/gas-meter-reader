#include "wifi_join.h"

#define ATTEMPT_US INT64_C(10000000)

static int64_t attempt_deadline(const gr_wifi_join_ops_t *ops, int64_t deadline, bool bounded) {
    int64_t now = ops->now(ops->context);
    return bounded && deadline - now > ATTEMPT_US ? now + ATTEMPT_US : deadline;
}

int gr_wifi_join(const gr_wifi_join_ops_t *ops, bool fallback_enabled, int64_t deadline) {
    if (ops->now(ops->context) >= deadline) return ops->timeout_error;
    int64_t first_deadline = attempt_deadline(ops, deadline, fallback_enabled);
    int error = ops->start(ops->context, false);
    if (!error) error = ops->wait(ops->context, first_deadline);
    if (!error || !fallback_enabled) return error;
    if (ops->now(ops->context) >= deadline) return ops->timeout_error;
    error = ops->stop(ops->context, deadline);
    if (error) return error;
    if (ops->now(ops->context) >= deadline) return ops->timeout_error;
    int64_t second_deadline = attempt_deadline(ops, deadline, true);
    error = ops->start(ops->context, true);
    return error ? error : ops->wait(ops->context, second_deadline);
}
