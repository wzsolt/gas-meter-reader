#include "wifi_join.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { FAILED = 1, TIMED_OUT = 2, STOP_FAILED = 3, START_FAILED = 4 };
typedef struct {
    int64_t now, delay[2], deadlines[2];
    int start_error[2], result[2], stop_error, starts, waits, stops;
    char order[16];
    unsigned order_length;
    bool selected;
} fake_t;
static void record(fake_t *f, char event) { f->order[f->order_length++] = event; }
static int64_t now(void *context) { return ((fake_t *)context)->now; }
static int start(void *context, bool fallback) {
    fake_t *f = context;
    f->selected = fallback; f->starts++;
    record(f, fallback ? 'F' : 'P');
    return f->start_error[fallback];
}
static int wait_for_connection(void *context, int64_t deadline) {
    fake_t *f = context;
    int index = f->selected;
    f->waits++; f->deadlines[index] = deadline;
    if (f->delay[index] >= deadline - f->now) {
        f->now = deadline;
        return TIMED_OUT;
    }
    f->now += f->delay[index];
    return f->result[index];
}
static int stop(void *context, int64_t deadline) {
    fake_t *f = context;
    assert(f->now < deadline);
    f->stops++; record(f, 'S');
    return f->stop_error;
}
static int join(fake_t *f, bool fallback, int64_t deadline) {
    gr_wifi_join_ops_t ops = {.context = f, .now = now, .start = start,
        .wait = wait_for_connection, .stop = stop, .timeout_error = TIMED_OUT};
    return gr_wifi_join(&ops, fallback, deadline);
}
int main(void) {
    fake_t f = {.delay = {1000000, 1000000}};
    assert(join(&f, true, 30000000) == 0);
    assert(f.starts == 1 && f.stops == 0 && !strcmp(f.order, "P"));
    f = (fake_t){.delay = {1000000, 2000000}, .result = {FAILED, 0}};
    assert(join(&f, true, 30000000) == 0);
    assert(!strcmp(f.order, "PSF") && f.waits == 2 && f.now == 3000000);
    assert(f.deadlines[0] == 10000000 && f.deadlines[1] == 11000000);
    f = (fake_t){.delay = {30000000, 2000000}};
    assert(join(&f, true, 30000000) == 0);
    assert(f.now == 12000000 && !strcmp(f.order, "PSF"));
    f = (fake_t){.delay = {30000000, 30000000}};
    assert(join(&f, true, 30000000) == TIMED_OUT);
    assert(f.now == 20000000 && f.deadlines[1] == 20000000);
    f = (fake_t){.delay = {25000000, 0}};
    assert(join(&f, false, 30000000) == 0);
    assert(f.now == 25000000 && f.starts == 1 && f.stops == 0);
    assert(f.deadlines[0] == 30000000);
    f = (fake_t){.result = {FAILED, 0}};
    assert(join(&f, false, 30000000) == FAILED && f.starts == 1);
    f = (fake_t){.now = 27000000, .delay = {5000000, 0}};
    assert(join(&f, true, 30000000) == TIMED_OUT);
    assert(f.now == 30000000 && f.starts == 1 && f.stops == 0);
    f = (fake_t){.now = 30000000};
    assert(join(&f, true, 30000000) == TIMED_OUT && f.starts == 0);
    f = (fake_t){.result = {FAILED, FAILED}};
    assert(join(&f, true, 30000000) == FAILED && f.starts == 2);
    f = (fake_t){.result = {FAILED, 0}, .stop_error = STOP_FAILED};
    assert(join(&f, true, 30000000) == STOP_FAILED && f.starts == 1);
    f = (fake_t){.start_error = {START_FAILED, 0}};
    assert(join(&f, true, 30000000) == 0 && f.waits == 1 && !strcmp(f.order, "PSF"));
    f = (fake_t){.result = {FAILED, 0}, .start_error = {0, START_FAILED}};
    assert(join(&f, true, 30000000) == START_FAILED && f.waits == 1);
    puts("Wi-Fi primary/fallback and shared deadline tests passed.");
}
