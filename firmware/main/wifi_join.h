#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Serialized adapter: wait() ends at its deadline or on connection/failure.
 * stop() must drain the old station's events before start() selects another network.
 * Callbacks return zero for success and preserve the platform's error codes. */
typedef struct {
    void *context;
    int64_t (*now)(void *context);
    int (*start)(void *context, bool fallback);
    int (*wait)(void *context, int64_t deadline);
    int (*stop)(void *context, int64_t deadline);
    int timeout_error;
} gr_wifi_join_ops_t;

int gr_wifi_join(const gr_wifi_join_ops_t *ops, bool fallback_enabled, int64_t deadline);
