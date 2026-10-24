#include "daily_schedule.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int64_t local_epoch(int y, int m, int d, int h, int minute) {
    struct tm local = {.tm_year=y-1900,.tm_mon=m-1,.tm_mday=d,.tm_hour=h,.tm_min=minute,.tm_isdst=-1};
    return mktime(&local);
}
static void bootstrap_fallback_and_retry(void) {
    gr_daily_t s; gr_daily_init(&s, 100);
    assert(gr_daily_valid(&s) && gr_daily_due(&s, 100));
    gr_daily_result(&s, 200, false);
    assert(!gr_daily_due(&s, 200));
    assert(gr_daily_remaining(&s, 200) == GR_DAILY_RETRY_US);
    assert(gr_daily_due(&s, 200+GR_DAILY_RETRY_US));
    gr_daily_result(&s, 200+GR_DAILY_RETRY_US, true);
    int64_t deadline=s.due_us;
    assert(gr_daily_remaining(&s, 200+GR_DAILY_RETRY_US) == GR_DAY_US);
    for (int64_t now=deadline-GR_DAY_US+1000; now<deadline; now+=30000000) {
        gr_daily_result(&s, now, true);
        assert(s.due_us==deadline && !gr_daily_due(&s, now));
    }
    gr_daily_seal(&s); gr_daily_t rtc=s;
    assert(gr_daily_valid(&rtc) && gr_daily_due(&rtc, deadline));
    rtc.due_us++; assert(!gr_daily_valid(&rtc));
}
static void time_alignment_and_failed_ack_checkpoint(void) {
    gr_daily_t s; gr_daily_init(&s, 0);
    int64_t utc=local_epoch(2026,10,15,15,0), end=local_epoch(2026,10,15,23,55);
    assert(gr_daily_observe_time(&s, 1000000, utc));
    assert(gr_daily_due(&s, 1000000)); /* ACK alone cannot finish a failed ESP save. */
    gr_daily_result(&s, 1000000, false);
    assert(s.retry_us==1000000+GR_DAILY_RETRY_US);
    gr_daily_result(&s, 2000000, true); /* Manual success can recover during cooldown. */
    assert(s.due_us==1000000+(end-utc)*1000000 && s.retry_us==0);
    int64_t due=s.due_us;
    gr_daily_result(&s, due-1, true); assert(s.due_us==due);
    assert(gr_daily_observe_time(&s, 3601000000, utc+3600));
    assert(s.due_us==due);
    gr_daily_result(&s, due, true);
    assert(s.due_us==due+GR_DAY_US);
    gr_daily_t before=s;
    assert(!gr_daily_observe_time(&s, due+1000, 0));
    assert(!gr_daily_observe_time(&s, due+1000, GR_TIME_MAX));
    assert(s.utc_seconds==before.utc_seconds && s.due_us==before.due_us);
}
static void midnight_and_dst(void) {
    int64_t today=local_epoch(2026,10,15,23,55), tomorrow=local_epoch(2026,10,16,23,55);
    assert(gr_next_day_end(today-1)==today);
    assert(gr_next_day_end(today)==tomorrow);
    assert(gr_next_day_end(local_epoch(2026,10,16,0,0))==tomorrow);
    int64_t spring=local_epoch(2026,3,28,23,55), autumn=local_epoch(2026,10,24,23,55);
    assert(gr_next_day_end(spring)-spring==23*3600);
    assert(gr_next_day_end(autumn)-autumn==25*3600);
    gr_daily_t s; gr_daily_init(&s, 0);
    assert(gr_daily_observe_time(&s, 0, autumn));
    gr_daily_result(&s, 0, true);
    assert(s.due_us==25*3600*INT64_C(1000000));
    assert(gr_daily_observe_time(&s, 1000000, local_epoch(2026,10,25,12,0)));
    assert(s.due_us==1000000+(11*3600+55*60)*INT64_C(1000000));
}
int main(void) {
    assert(setenv("TZ", GR_LOCAL_TZ, 1)==0); tzset();
    bootstrap_fallback_and_retry(); time_alignment_and_failed_ack_checkpoint(); midnight_and_dst();
    puts("Daily sync: bootstrap, idle heartbeat, stable deadline, ACK failure, RTC, midnight and DST passed.");
}
