#include "sync_drain.h"
#include "core.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { TIMEOUT = 10, STORAGE_ERROR = 11, CONNECTION_ERROR = 12 };
typedef struct {
    bool heartbeat;
    gr_state_t live, disk;
    gr_outbox_t reports[16], ha_latest;
    uint64_t ha_last, ha_pulses, arrive_first, arrive_each;
    int prepares, connects, exchanges, fail_prepare, lose_ack, fail_ack, connect_error;
    int64_t now, connect_duration, exchange_duration, deadlines[17];
} fake_t;

static int64_t now(void *context) { return ((fake_t *)context)->now; }
static void commit(fake_t *f, gr_state_t *s) {
    s->generation++; gr_seal(s); f->disk = *s; f->live = *s;
}
static int prepare(void *context, int mv) {
    fake_t *f = context; f->prepares++;
    if (f->live.outbox.active) return 0;
    gr_state_t candidate = f->live;
    assert(gr_prepare(&candidate, mv, "1.3.0"));
    if (f->prepares == f->fail_prepare) return STORAGE_ERROR;
    commit(f, &candidate);
    return 0;
}
static int connect(void *context, int64_t deadline) {
    fake_t *f = context; f->connects++; f->deadlines[0] = deadline;
    f->now += f->connect_duration;
    return f->connect_error;
}
static int exchange(void *context, int64_t deadline) {
    fake_t *f = context;
    assert(f->exchanges < 16 && f->live.outbox.active);
    gr_outbox_t packet = f->live.outbox;
    f->reports[f->exchanges++] = packet; f->deadlines[f->exchanges] = deadline;
    assert(gr_valid(&f->disk)); /* The immutable packet was saved before publication. */
    assert(!memcmp(&packet, &f->disk.outbox, sizeof(packet)));
    f->live.pending += f->arrive_each + (f->exchanges == 1 ? f->arrive_first : 0);
    if (f->now + f->exchange_duration > deadline) { f->now = deadline; return TIMEOUT; }
    f->now += f->exchange_duration;
    if (packet.sequence > f->ha_last) {
        assert(packet.sequence == f->ha_last + 1);
        f->ha_pulses += packet.pulses; f->ha_last = packet.sequence; f->ha_latest = packet;
    } else {
        assert(packet.sequence == f->ha_last);
        assert(!memcmp(&packet, &f->ha_latest, sizeof(packet))); /* Exact duplicate, no double count. */
    }
    if (f->exchanges == f->lose_ack) return TIMEOUT;
    gr_state_t candidate;
    assert(gr_ack_candidate(&f->live, packet.sequence, &candidate));
    if (f->exchanges == f->fail_ack) return STORAGE_ERROR;
    commit(f, &candidate);
    return 0;
}
static bool pending(void *context) { return ((fake_t *)context)->live.pending != 0; }
static int sync(fake_t *f, int64_t deadline) {
    gr_sync_drain_ops_t ops = {.context=f,.now=now,.prepare=prepare,.connect=connect,
        .exchange=exchange,.pending=pending,.timeout_error=TIMEOUT,.refresh_saved_packet=f->heartbeat};
    return gr_sync_drain(&ops, 3910, deadline);
}
static fake_t initial(uint64_t pulses, bool old_packet, uint64_t old_pulses) {
    fake_t f = {.connect_duration=5000,.exchange_duration=1000};
    gr_state_init(&f.live, "0123456789abcdef0123456789abcdef", 1, 0);
    if (old_packet) {
        f.live.pending = old_pulses;
        assert(gr_prepare(&f.live, 3700, "1.2.0"));
    }
    f.live.pending = pulses;
    gr_seal(&f.live); f.disk=f.live;
    return f;
}

static void old_zero_then_current_pulses(void) {
    fake_t f = initial(5, true, 0); gr_outbox_t old = f.live.outbox;
    assert(sync(&f, 30000000) == 0);
    assert(f.connects == 1 && f.exchanges == 2);
    assert(!memcmp(&f.reports[0], &old, sizeof(old)));
    assert(f.reports[1].sequence == 2 && f.reports[1].pulses == 5);
    assert(f.reports[1].battery_mv == 3910 && !strcmp(f.reports[1].firmware,"1.3.0"));
    assert(f.live.pending == 0 && !f.live.outbox.active && f.ha_pulses == 5);
}
static void old_packet_deducts_only_its_pulses(void) {
    fake_t f = initial(7, true, 2);
    assert(sync(&f, 30000000) == 0);
    assert(f.exchanges == 2 && f.reports[0].pulses == 2 && f.reports[1].pulses == 5);
    assert(f.ha_pulses == 7 && f.live.pending == 0);
}
static void no_extra_zero_and_count_while_sending(void) {
    fake_t f = initial(0, false, 0);
    assert(sync(&f, 30000000) == 0 && f.exchanges == 1);
    f = initial(4, false, 0);
    assert(sync(&f, 30000000) == 0 && f.exchanges == 1 && f.ha_pulses == 4);
    f = initial(4, false, 0); f.arrive_first = 3;
    assert(sync(&f, 30000000) == 0 && f.exchanges == 2);
    assert(f.reports[0].pulses == 4 && f.reports[1].pulses == 3 && f.ha_pulses == 7);
}
static void lost_second_ack_replays_saved_packet(void) {
    fake_t f = initial(5, true, 0); f.lose_ack = 2;
    assert(sync(&f, 30000000) == TIMEOUT);
    assert(f.live.pending == 5 && f.disk.outbox.active && f.disk.outbox.sequence == 2);
    gr_outbox_t saved = f.disk.outbox; f.live = f.disk; f.lose_ack = 0;
    assert(sync(&f, 30000000) == 0);
    assert(!memcmp(&saved, &f.reports[2], sizeof(saved)));
    assert(f.ha_pulses == 5 && f.live.pending == 0);
}
static void storage_failures_preserve_remaining_pulses(void) {
    fake_t f = initial(5, true, 0); f.fail_prepare = 2;
    assert(sync(&f, 30000000) == STORAGE_ERROR);
    assert(f.exchanges == 1 && f.live.pending == 5 && !f.live.outbox.active);
    f.live = f.disk; f.fail_prepare = 0;
    assert(sync(&f, 30000000) == 0 && f.ha_pulses == 5);
    f = initial(5, true, 0); f.fail_ack = 2;
    assert(sync(&f, 30000000) == STORAGE_ERROR);
    assert(f.live.pending == 5 && f.disk.outbox.sequence == 2 && f.ha_pulses == 5);
    f.live = f.disk; f.fail_ack = 0;
    assert(sync(&f, 30000000) == 0 && f.ha_pulses == 5 && f.live.pending == 0);
}
static void one_shared_deadline_and_connection_failure(void) {
    fake_t f = initial(5, true, 0);
    f.connect_duration = 10000000; f.exchange_duration = 20000000;
    assert(sync(&f, 30000000) == TIMEOUT);
    assert(f.connects == 1 && f.exchanges == 1 && f.prepares == 1 && f.live.pending == 5);
    assert(f.deadlines[0] == 30000000 && f.deadlines[1] == 30000000);
    f = initial(5, false, 0); f.now = 30000000;
    assert(sync(&f, 30000000) == TIMEOUT && f.prepares == 0 && f.connects == 0);
    f = initial(5, false, 0); f.connect_error = CONNECTION_ERROR;
    assert(sync(&f, 30000000) == CONNECTION_ERROR && f.exchanges == 0 && f.disk.outbox.active);
}
static void continuous_pulses_cannot_extend_deadline(void) {
    fake_t f = initial(1, false, 0); f.arrive_each = 1; f.exchange_duration = 9000000;
    assert(sync(&f, 30000000) == TIMEOUT);
    assert(f.connects == 1 && f.exchanges == 4 && f.now == 30000000);
    for (int i=0;i<=f.exchanges;i++) assert(f.deadlines[i] == 30000000);
    assert(f.live.pending > 0 && f.disk.outbox.active);
}
static void daily_refreshes_saved_zero_packet(void) {
    fake_t f = initial(0, true, 0); f.heartbeat=true;
    gr_outbox_t saved=f.live.outbox;
    assert(sync(&f, 30000000)==0 && f.exchanges==2 && f.connects==1);
    assert(!memcmp(&saved, &f.reports[0], sizeof(saved)));
    assert(f.reports[1].pulses==0 && f.reports[1].battery_mv==3910);
    assert(!strcmp(f.reports[1].firmware,"1.3.0") && f.ha_pulses==0);
    f=initial(4, true, 4); f.heartbeat=true;
    assert(sync(&f, 30000000)==0 && f.exchanges==2 && f.ha_pulses==4);
    assert(f.reports[1].pulses==0);
}
int main(void) {
    daily_refreshes_saved_zero_packet();
    old_zero_then_current_pulses(); old_packet_deducts_only_its_pulses();
    no_extra_zero_and_count_while_sending(); lost_second_ack_replays_saved_packet();
    storage_failures_preserve_remaining_pulses(); one_shared_deadline_and_connection_failure();
    continuous_pulses_cannot_extend_deadline();
    puts("Sync drain: old packets, new pulses, ACK/save failures and shared deadline passed.");
}
