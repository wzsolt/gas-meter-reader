#include "meter_reset.h"
#include "core.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { TIMEOUT=10, STORAGE_ERROR=11, CONNECTION_ERROR=12 };
typedef struct {
    gr_state_t live, disk;
    gr_outbox_t packets[8], ha_latest;
    uint64_t ha_last, ha_pulses, arrive;
    int prepares, connects, exchanges, fail_prepare, lose_ack, fail_ack, connect_error;
    int64_t now, connection_us, exchange_us;
} fake_t;
static int64_t now(void *ctx) { return ((fake_t *)ctx)->now; }
static bool active(void *ctx) { return ((fake_t *)ctx)->live.outbox.active; }
static bool reset_pending(void *ctx) {
    fake_t *f=ctx; return gr_outbox_is_reset(&f->live.outbox);
}
static void commit(fake_t *f, gr_state_t *candidate) {
    candidate->generation++; gr_seal(candidate); f->live=*candidate; f->disk=*candidate;
}
static int prepare(void *ctx, int mv) {
    fake_t *f=ctx; f->prepares++;
    gr_state_t candidate=f->live;
    assert(gr_prepare_reset(&candidate, mv, "1.4.2"));
    if (f->live.outbox.active) return 0;
    if (f->fail_prepare) return STORAGE_ERROR;
    commit(f, &candidate); return 0;
}
static int connect(void *ctx, int64_t deadline) {
    fake_t *f=ctx; f->connects++; assert(deadline==30000000);
    f->now+=f->connection_us; return f->connect_error;
}
static int exchange(void *ctx, int64_t deadline) {
    fake_t *f=ctx; assert(f->live.outbox.active && f->exchanges<8 && deadline==30000000);
    gr_outbox_t packet=f->live.outbox; f->packets[f->exchanges++]=packet;
    assert(gr_valid(&f->disk) && !memcmp(&packet, &f->disk.outbox, sizeof(packet)));
    f->live.pending+=f->arrive;
    f->now+=f->exchange_us;
    if (f->now>deadline) { f->now=deadline; return TIMEOUT; }
    if (packet.sequence>f->ha_last) {
        assert(packet.sequence==f->ha_last+1);
        if (!gr_outbox_is_reset(&packet)) f->ha_pulses+=packet.pulses;
        f->ha_last=packet.sequence; f->ha_latest=packet;
    } else {
        assert(packet.sequence==f->ha_last && !memcmp(&packet, &f->ha_latest, sizeof(packet)));
    }
    if (f->lose_ack==f->exchanges) return TIMEOUT;
    gr_state_t candidate; assert(gr_ack_candidate(&f->live, packet.sequence, &candidate));
    if (f->fail_ack==f->exchanges) return STORAGE_ERROR;
    commit(f, &candidate); return 0;
}
static int reset(fake_t *f) {
    gr_meter_reset_ops_t ops={.sync={.context=f,.now=now,.connect=connect,
        .exchange=exchange,.timeout_error=TIMEOUT},.active=active,.reset_pending=reset_pending,.prepare_reset=prepare};
    return gr_meter_reset_sync(&ops, 3910, 30000000);
}
static fake_t initial(uint64_t pending) {
    fake_t f={.ha_pulses=12345,.connection_us=5000,.exchange_us=1000};
    gr_state_init(&f.live, "0123456789abcdef0123456789abcdef", 1, 0);
    f.live.pending=pending; gr_seal(&f.live); f.disk=f.live; return f;
}
static void direct_reset_and_new_arrivals(void) {
    fake_t f=initial(5); f.arrive=2;
    assert(reset(&f)==0 && f.connects==1 && f.exchanges==1);
    assert(f.packets[0].pulses==5 && f.packets[0].reserved[0]==GR_OUTBOX_RESET);
    assert(f.ha_pulses==12345 && f.live.pending==2 && !f.live.outbox.active);
    assert(f.live.next_sequence==2 && !strcmp(f.live.instance, f.disk.instance));
}
static void settle_old_packet_before_reset(void) {
    fake_t f=initial(2); assert(gr_prepare(&f.live, 3700, "1.2.0"));
    f.live.pending=7; gr_seal(&f.live); f.disk=f.live; gr_outbox_t old=f.live.outbox;
    assert(reset(&f)==0 && f.connects==1 && f.exchanges==2);
    assert(!memcmp(&old, &f.packets[0], sizeof(old)));
    assert(f.packets[1].sequence==2 && f.packets[1].pulses==5);
    assert(f.packets[1].reserved[0]==GR_OUTBOX_RESET && f.ha_pulses==12347 && f.live.pending==0);
}
static void lost_ack_and_failed_checkpoint_replay_exact_reset(void) {
    for (int storage=0;storage<2;storage++) {
        fake_t f=initial(5);
        if (storage) f.fail_ack=1; else f.lose_ack=1;
        assert(reset(&f)==(storage ? STORAGE_ERROR : TIMEOUT));
        assert(f.ha_pulses==12345 && f.disk.pending==5 && reset_pending(&f));
        gr_outbox_t saved=f.disk.outbox; f.live=f.disk;
        f.ha_pulses=2; /* A later HA correction must survive a duplicate reset. */
        f.lose_ack=f.fail_ack=0;
        assert(reset(&f)==0 && f.exchanges==2);
        assert(!memcmp(&saved, &f.packets[1], sizeof(saved)));
        assert(f.ha_pulses==2 && f.live.pending==0 && f.live.next_sequence==2);
    }
}
static void failures_never_publish_unsaved_reset(void) {
    fake_t f=initial(5); f.fail_prepare=1; gr_state_t old=f.disk;
    assert(reset(&f)==STORAGE_ERROR && f.connects==0 && f.exchanges==0);
    assert(!memcmp(&old, &f.disk, sizeof(old)) && f.live.pending==5);
    f=initial(5); f.connect_error=CONNECTION_ERROR;
    assert(reset(&f)==CONNECTION_ERROR && f.exchanges==0 && reset_pending(&f));
    assert(gr_valid(&f.disk) && f.disk.outbox.pulses==5);
}
static void shared_deadline_and_empty_retry(void) {
    fake_t f=initial(0); assert(gr_prepare(&f.live, 3700, "1.2.0"));
    f.live.pending=5; gr_seal(&f.live); f.disk=f.live;
    f.connection_us=10000000; f.exchange_us=20000000;
    assert(reset(&f)==TIMEOUT && f.exchanges==1 && f.prepares==0 && f.live.pending==5);
    assert(!f.live.outbox.active && f.ha_pulses==12345);
    f=initial(5); f.now=30000000;
    assert(reset(&f)==TIMEOUT && f.prepares==0 && f.connects==0);
    f=initial(0); f.lose_ack=1;
    assert(reset(&f)==TIMEOUT && f.disk.pending==0 && reset_pending(&f));
    assert(!gr_automatic_sync_due(&f.disk, GR_INTERVAL_US-1));
    assert(gr_automatic_sync_due(&f.disk, GR_INTERVAL_US));
    f.lose_ack=0; assert(reset(&f)==0 && f.ha_pulses==12345);
    f=initial(0); assert(gr_prepare(&f.live, 3700, "1.2.0")); gr_seal(&f.live);
    assert(!gr_automatic_sync_due(&f.live, GR_INTERVAL_US));
}
static void tag_validation_and_fault_preservation(void) {
    fake_t f=initial(5); f.live.counter_fault=1;
    assert(!gr_prepare_reset(&f.live, 3910, "1.4.0") && f.live.pending==5);
    f.live.counter_fault=0; assert(gr_prepare_reset(&f.live, 3910, "1.4.0"));
    gr_seal(&f.live); assert(gr_valid(&f.live));
    gr_outbox_t packet=f.live.outbox;
    assert(gr_prepare_reset(&f.live, 4100, "2.0.0"));
    assert(!memcmp(&packet, &f.live.outbox, sizeof(packet)));
    f.live.outbox.reserved[0]=3; gr_seal(&f.live); assert(!gr_valid(&f.live));
}
static void legacy_packet_replays_without_changing_reading(void) {
    fake_t f=initial(5); assert(gr_prepare_reset(&f.live, 3700, "1.4.1"));
    f.live.outbox.reserved[0]=GR_OUTBOX_LEGACY_RESET;
    gr_seal(&f.live); f.disk=f.live; gr_outbox_t saved=f.live.outbox;
    assert(gr_valid(&f.disk) && gr_automatic_sync_due(&f.disk, GR_INTERVAL_US));
    assert(reset(&f)==0 && f.exchanges==1);
    assert(!memcmp(&saved, &f.packets[0], sizeof(saved)));
    assert(f.ha_pulses==12345 && f.live.pending==0 && f.live.next_sequence==2);
}
int main(void) {
    direct_reset_and_new_arrivals(); settle_old_packet_before_reset();
    lost_ack_and_failed_checkpoint_replay_exact_reset(); failures_never_publish_unsaved_reset();
    shared_deadline_and_empty_retry(); tag_validation_and_fault_preservation();
    legacy_packet_replays_without_changing_reading();
    puts("Meter reset: persisted operation, dedup, new pulses, faults and shared deadline passed.");
}
