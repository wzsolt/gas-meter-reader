#include "daily_schedule.h"
#include "core.h"
#include <string.h>
#include <time.h>

void gr_daily_seal(gr_daily_t *s) {
    s->crc = 0;
    s->crc = gr_crc(s, sizeof(*s));
}
bool gr_daily_valid(const gr_daily_t *s) {
    if (s->version != 1 || s->due_us < 0 || s->retry_us < 0 || s->anchor_us < 0 ||
        (s->utc_seconds && (s->utc_seconds < GR_TIME_MIN || s->utc_seconds >= GR_TIME_MAX))) return false;
    gr_daily_t copy = *s;
    gr_daily_seal(&copy);
    return copy.crc == s->crc;
}
void gr_daily_init(gr_daily_t *s, int64_t now) {
    memset(s, 0, sizeof(*s));
    s->version = 1;
    s->due_us = now; /* Cold boot learns time with one bounded sync attempt. */
    gr_daily_seal(s);
}
static int64_t next_attempt(const gr_daily_t *s) {
    return s->retry_us > s->due_us ? s->retry_us : s->due_us;
}
bool gr_daily_due(const gr_daily_t *s, int64_t now) { return now >= next_attempt(s); }
int64_t gr_daily_remaining(const gr_daily_t *s, int64_t now) {
    int64_t next = next_attempt(s);
    return next > now ? next - now : 1;
}
int64_t gr_next_day_end(int64_t utc_seconds) {
    time_t epoch = (time_t)utc_seconds;
    struct tm local;
    if (!localtime_r(&epoch, &local)) return 0;
    local.tm_hour = 23; local.tm_min = 55; local.tm_sec = 0; local.tm_isdst = -1;
    time_t target = mktime(&local);
    if (target <= epoch) {
        local.tm_mday++;
        local.tm_isdst = -1;
        target = mktime(&local);
    }
    return target > epoch ? (int64_t)target : 0;
}
bool gr_daily_observe_time(gr_daily_t *s, int64_t now, int64_t utc_seconds) {
    if (utc_seconds < GR_TIME_MIN || utc_seconds >= GR_TIME_MAX) return false;
    s->utc_seconds = utc_seconds; s->anchor_us = now;
    /* An already-due attempt is advanced only after its ACK/NVS commit succeeds. */
    if (now < s->due_us) {
        int64_t target = gr_next_day_end(utc_seconds);
        if (target) s->due_us = now + (target - utc_seconds) * INT64_C(1000000);
    }
    return true;
}
void gr_daily_result(gr_daily_t *s, int64_t now, bool success) {
    if (now < s->due_us) return; /* Daytime syncs never postpone the evening sync. */
    if (!success) { s->retry_us = now + GR_DAILY_RETRY_US; return; }
    int64_t utc_now = s->utc_seconds ? s->utc_seconds + (now - s->anchor_us) / INT64_C(1000000) : 0;
    int64_t target = utc_now ? gr_next_day_end(utc_now) : 0;
    s->due_us = target ? now + (target - utc_now) * INT64_C(1000000) : now + GR_DAY_US;
    s->retry_us = 0;
}
