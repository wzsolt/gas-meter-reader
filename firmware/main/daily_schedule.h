#pragma once
#include <stdbool.h>
#include <stdint.h>

#define GR_DAY_US INT64_C(86400000000)
#define GR_DAILY_RETRY_US INT64_C(1800000000)
#define GR_LOCAL_TZ "CET-1CEST,M3.5.0/2,M10.5.0/3"
#define GR_TIME_MIN INT64_C(1704067200) /* 2024-01-01 UTC */
#define GR_TIME_MAX INT64_C(4102444800) /* 2100-01-01 UTC */

/* Separate RTC record: the existing pulse/NVS transaction format stays intact. */
typedef struct {
    uint32_t version, crc;
    int64_t due_us, retry_us, utc_seconds, anchor_us;
} gr_daily_t;

void gr_daily_init(gr_daily_t *s, int64_t now);
bool gr_daily_valid(const gr_daily_t *s);
void gr_daily_seal(gr_daily_t *s);
bool gr_daily_due(const gr_daily_t *s, int64_t now);
int64_t gr_daily_remaining(const gr_daily_t *s, int64_t now);
/* Caller initializes TZ/tzset once per boot before learning a time anchor. */
int64_t gr_next_day_end(int64_t utc_seconds);
bool gr_daily_observe_time(gr_daily_t *s, int64_t now, int64_t utc_seconds);
void gr_daily_result(gr_daily_t *s, int64_t now, bool success);
