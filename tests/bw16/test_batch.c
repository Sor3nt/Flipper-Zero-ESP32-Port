#include "bw16_batch.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static char sent[1024][BW16_COMMAND_SIZE];
static unsigned sends;
static bool tx_ok;
static bool transmit(void* context, const char* command) {
    assert(context == &sends && sends < 1024);
    assert(strlen(command) < BW16_COMMAND_SIZE);
    strcpy(sent[sends++], command);
    return tx_ok;
}
static void init(Bw16Batch* b, Bw16Control* c, unsigned aps) {
    memset(b, 0, sizeof(*b));
    bw16_control_init(c, transmit, &sends);
    c->native_selected = true;
    c->ap_count = c->ap_total = aps;
    c->aps_valid = c->indices_valid = true;
    for(unsigned i = 0; i < aps; ++i) {
        snprintf(c->aps[i].mac, sizeof(c->aps[i].mac), "0011223344%02X", i & 0xffU);
        c->aps[i].channel = i & 1U ? 36 : 6;
    }
    sends = 0;
    tx_ok = true;
}
static void tick(Bw16Batch* b, Bw16Control* c, uint32_t now, bool quiet) {
    bw16_control_tick(c, now);
    bw16_batch_tick(b, c, now, quiet);
}
static void line(Bw16Batch* b, Bw16Control* c, const char* text, uint32_t now) {
    bw16_control_line(c, text, now);
    tick(b, c, now, false);
}
static void command(unsigned at, const char* expected) {
    assert(at < sends && !strcmp(sent[at], expected));
}
static void validation(void) {
    Bw16Batch b;
    Bw16Control c;
    unsigned selected[] = {0, 1, 2};
    init(&b, &c, 3);
    assert(!bw16_batch_start(&b, &c, selected, 0, 0));
    assert(!bw16_batch_start(&b, &c, selected, BW16_MAX_APS + 1, 0));
    assert(!bw16_batch_start(&b, &c, NULL, 1, 0));
    assert(!bw16_batch_start(NULL, &c, selected, 1, 0));
    assert(!bw16_batch_start(&b, NULL, selected, 1, 0));
    selected[2] = 3;
    assert(!bw16_batch_start(&b, &c, selected, 3, 0));
    selected[2] = UINT_MAX;
    assert(!bw16_batch_start(&b, &c, selected, 3, 0));
    selected[2] = 0;
    assert(!bw16_batch_start(&b, &c, selected, 3, 0));
    selected[2] = 2;
    c.aps[2].mac[0] = 0;
    assert(!bw16_batch_start(&b, &c, selected, 3, 0));
    strcpy(c.aps[2].mac, "001122334402");
    c.indices_valid = false;
    assert(!bw16_batch_start(&b, &c, selected, 3, 0));
    c.indices_valid = true;
    c.aps_valid = false;
    assert(!bw16_batch_start(&b, &c, selected, 3, 0));
    c.aps_valid = true;
    c.restart_required = true;
    assert(!bw16_batch_start(&b, &c, selected, 3, 0));
    c.restart_required = false;
    c.remote_unknown = true;
    assert(!bw16_batch_start(&b, &c, selected, 3, 0));
    c.remote_unknown = false;
    const Bw16State unavailable[] = {Bw16Scan, Bw16Stations, Bw16Clients, Bw16Capabilities,
        Bw16Starting, Bw16Active, Bw16Stopping, Bw16Fault};
    for(unsigned i = 0; i < sizeof(unavailable) / sizeof(unavailable[0]); ++i) {
        c.state = unavailable[i];
        assert(!bw16_batch_start(&b, &c, selected, 3, 0));
    }
    c.state = Bw16Idle;
    c.native_selected = false;
    assert(!bw16_batch_start(&b, &c, selected, 3, 0));
    assert(sends == 0 && !b.running && !b.count);
    c.native_selected = true;
    c.state = Bw16Stopped;
    assert(bw16_batch_start(&b, &c, selected, 3, 20));
    assert(!bw16_batch_start(&b, &c, selected, 3, 21));
    command(0, "DEAUTH_SELECTED 0000000000007\n");
    assert(sends == 1 && b.native && c.target == Bw16TargetSelected);
}
static void native_session(void) {
    Bw16Batch b;
    Bw16Control c;
    unsigned selected[] = {3, 1};
    init(&b, &c, 5);
    assert(bw16_batch_start(&b, &c, selected, 2, 100));
    command(0, "DEAUTH_SELECTED 000000000000a\n");
    assert(c.selected_mask == ((UINT64_C(1) << 3) | (UINT64_C(1) << 1)));
    selected[0] = 4; /* The transmitted selection and batch snapshot are immutable. */
    assert(b.targets[0] == 3 && b.targets[1] == 1);
    tick(&b, &c, 1700, true);
    assert(c.state == Bw16Starting && sends == 1);
    line(&b, &c, "DEAUTH_ALL_START", 1701);
    line(&b, &c, "TARGETED_START", 1702);
    line(&b, &c, "TARGETED_DONE", 1703);
    assert(c.state == Bw16Starting && sends == 1);
    line(&b, &c, "SELECTED_START", 1800);
    assert(c.state == Bw16Active && b.running);
    line(&b, &c, "SELECTED_AP:1", 1801);
    assert(c.selected_ap_valid && c.selected_ap == 1 && b.targets[b.position] == 1);
    line(&b, &c, "TX:17", 1802);
    assert(bw16_batch_counter(&b, &c) == 17);
    line(&b, &c, "SELECTED_AP:3", 2000);
    assert(c.selected_ap == 3 && b.targets[b.position] == 3);
    for(uint32_t now = 3000; now < 30100; now += 1000) {
        tick(&b, &c, now, (now & 1U) != 0);
        assert(c.state == Bw16Active && b.running && sends == 1);
    }
    line(&b, &c, "TX:900", 30099);
    tick(&b, &c, 30100, true);
    command(1, "STOP\n");
    assert(c.state == Bw16Stopping && b.running && sends == 2);
    line(&b, &c, "TARGETED_DONE", 30101);
    line(&b, &c, "DEAUTH_ALL_STOPPED", 30102);
    line(&b, &c, "SELECTED_START", 30103);
    assert(c.state == Bw16Stopping && c.remote_unknown);
    line(&b, &c, "TX:903", 30104);
    line(&b, &c, "SELECTED_DONE", 30105);
    assert(c.state == Bw16Stopped && !b.running && !c.remote_unknown);
    assert(bw16_batch_counter(&b, &c) == 903 && sends == 2);
    tick(&b, &c, 40000, true);
    line(&b, &c, "TX:9999", 40001);
    assert(sends == 2 && bw16_batch_counter(&b, &c) == 903);
}
static void cancellation(void) {
    Bw16Batch b;
    Bw16Control c;
    const unsigned selected[] = {0, 2};
    init(&b, &c, 3);
    assert(bw16_batch_start(&b, &c, selected, 2, 0));
    bw16_batch_stop(&b, &c, 1); /* Cancel while awaiting START. */
    command(1, "STOP\n");
    assert(b.running && b.cancel_requested && c.state == Bw16Stopping);
    line(&b, &c, "SELECTED_START", 2);
    line(&b, &c, "SELECTED_DONE", 3);
    assert(!b.running && !c.remote_unknown && sends == 2);
    tick(&b, &c, 1000, true);
    assert(sends == 2);

    init(&b, &c, 3);
    assert(bw16_batch_start(&b, &c, selected, 2, 0));
    line(&b, &c, "SELECTED_START", 1);
    line(&b, &c, "TX:12", 2);
    bw16_batch_stop(&b, &c, 3);
    bw16_batch_stop(&b, &c, 4);
    assert(sends == 2);
    line(&b, &c, "TX:15", 5);
    line(&b, &c, "SELECTED_DONE", 6);
    assert(!b.running && bw16_batch_counter(&b, &c) == 15);
    line(&b, &c, "SELECTED_AP:2", 7);
    tick(&b, &c, 1000, true);
    assert(sends == 2);
}
static void faults(void) {
    Bw16Batch b;
    Bw16Control c;
    const unsigned selected[] = {0, 1};
    init(&b, &c, 2);
    assert(bw16_batch_start(&b, &c, selected, 2, 0));
    tick(&b, &c, 5000, true);
    assert(c.state == Bw16Stopping && c.restart_required && b.cancel_requested && sends == 2);
    line(&b, &c, "SELECTED_START", 5001);
    line(&b, &c, "SELECTED_DONE", 5002);
    assert(c.state == Bw16Fault && !b.running && sends == 2);
    assert(!bw16_batch_start(&b, &c, selected, 2, 5103));

    init(&b, &c, 2);
    assert(bw16_batch_start(&b, &c, selected, 2, 0));
    line(&b, &c, "SELECTED_START", 1);
    bw16_batch_stop(&b, &c, 10);
    tick(&b, &c, 5010, true);
    assert(c.state == Bw16Fault && c.remote_unknown && !b.running);
    line(&b, &c, "SELECTED_DONE", 5011);
    assert(c.state == Bw16Fault && sends == 2 && !b.running);

    init(&b, &c, 2);
    assert(bw16_batch_start(&b, &c, selected, 2, 0));
    line(&b, &c, "SELECTED_DONE", 1); /* Completion cannot substitute for START. */
    assert(c.state == Bw16Fault && c.restart_required && !b.running && sends == 1);

    init(&b, &c, 2);
    tx_ok = false;
    assert(!bw16_batch_start(&b, &c, selected, 2, 0));
    assert(c.state == Bw16Fault && c.remote_unknown && !b.running && sends == 1);

    init(&b, &c, 2);
    assert(bw16_batch_start(&b, &c, selected, 2, 0));
    line(&b, &c, "SELECTED_START", 1);
    line(&b, &c, "TX:5", 2);
    bw16_control_error(&c, "RX overflow", 3);
    tick(&b, &c, 3, true);
    assert(b.cancel_requested && c.state == Bw16Stopping && b.running);
    line(&b, &c, "TX:7", 4);
    line(&b, &c, "SELECTED_DONE", 5);
    assert(c.state == Bw16Fault && !b.running && sends == 2);
    assert(bw16_batch_counter(&b, &c) == 7);
}
static void current_target_validation(void) {
    static const char* invalid[] = {"SELECTED_AP:0", "SELECTED_AP:5", "SELECTED_AP:50",
        "SELECTED_AP:-1", "SELECTED_AP:", "SELECTED_AP:4294967296", "SELECTED_AP:1x"};
    Bw16Batch b;
    Bw16Control c;
    const unsigned selected[] = {1, 3};
    for(unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        init(&b, &c, 5);
        assert(bw16_batch_start(&b, &c, selected, 2, 0));
        line(&b, &c, "SELECTED_START", 1);
        line(&b, &c, invalid[i], 2);
        assert(c.state == Bw16Stopping && c.restart_required && b.cancel_requested && sends == 2);
        command(1, "STOP\n");
        line(&b, &c, "SELECTED_DONE", 3);
        assert(c.state == Bw16Fault && !b.running && !c.remote_unknown);
    }
}
static void deadline_and_single(void) {
    Bw16Batch b;
    Bw16Control c;
    const unsigned selected[] = {1, 0};
    const uint32_t start = UINT32_MAX - 100U;
    init(&b, &c, 2);
    assert(bw16_batch_start(&b, &c, selected, 2, start));
    line(&b, &c, "SELECTED_START", start + 1U);
    tick(&b, &c, start + BW16_OPERATION_MS - 1U, true);
    assert(b.running && c.state == Bw16Active && sends == 1);
    tick(&b, &c, start + BW16_OPERATION_MS, true);
    assert(b.running && b.cancel_requested && c.state == Bw16Stopping && sends == 2);
    tick(&b, &c, start + BW16_OPERATION_MS + BW16_STOP_TIMEOUT_MS, true);
    assert(!b.running && c.state == Bw16Fault && c.remote_unknown);

    for(unsigned capability = 0; capability < 2; ++capability) {
        init(&b, &c, 2);c.native_selected = capability != 0;
        assert(bw16_batch_start(&b, &c, selected, 1, start));
        assert(!b.native && !strcmp(sent[0], "DEAUTH_STATION 1\n"));
        line(&b, &c, "SELECTED_START", start + 2U);
        line(&b, &c, "SELECTED_AP:999", start + 3U);
        line(&b, &c, "SELECTED_DONE", start + 4U);
        assert(c.state == Bw16Starting && sends == 1);
        line(&b, &c, "TARGETED_START", start + 10U);
        tick(&b, &c, start + BW16_OPERATION_MS - 1U, true);
        assert(c.state == Bw16Active && sends == 1);
        tick(&b, &c, start + BW16_OPERATION_MS, true);
        command(1, "STOP\n");
        line(&b, &c, "TARGETED_DONE", start + BW16_OPERATION_MS + 1U);
        assert(!b.running && sends == 2 && !c.remote_unknown);
    }
}
static void all_fifty_and_counters(void) {
    Bw16Batch b;
    Bw16Control c;
    unsigned selected[BW16_MAX_APS];
    for(unsigned i = 0; i < BW16_MAX_APS; ++i) selected[i] = BW16_MAX_APS - 1U - i;
    init(&b, &c, BW16_MAX_APS);
    assert(bw16_batch_start(&b, &c, selected, BW16_MAX_APS, 0));
    command(0, "DEAUTH_SELECTED 3ffffffffffff\n");
    line(&b, &c, "SELECTED_START", 1);
    for(unsigned i = 0; i < BW16_MAX_APS; ++i) {
        char status[32];snprintf(status, sizeof(status), "SELECTED_AP:%u", i);
        line(&b, &c, status, 10 + i);
        assert(c.selected_ap == i && b.targets[b.position] == i && sends == 1);
    }
    line(&b, &c, "TX:4294967295", 100);
    assert(bw16_batch_counter(&b, &c) == UINT32_MAX);
    bw16_batch_stop(&b, &c, 101);
    line(&b, &c, "SELECTED_DONE", 102);
    assert(!b.running && !c.remote_unknown && c.state == Bw16Stopped && sends == 2);
    assert(bw16_batch_counter(&b, &c) == UINT32_MAX);
    for(unsigned i = 0; i < sends; ++i) assert(strcmp(sent[i], "DEAUTH_ALL\n"));

    init(&b, &c, BW16_MAX_APS);
    assert(bw16_batch_start(&b, &c, selected, BW16_MAX_APS, 0));
    line(&b, &c, "SELECTED_START", 1);
    line(&b, &c, "TX:4294967296", 2);
    assert(c.state == Bw16Stopping && c.restart_required && sends == 2);
    line(&b, &c, "SELECTED_DONE", 3);
    assert(c.state == Bw16Fault && !b.running);
}
int main(void) {
    validation();native_session();cancellation();faults();current_target_validation();
    deadline_and_single();all_fifty_and_counters();
    puts("batch: native selected-only mask, one START/STOP session, capability gating, validation, cancellation, wrap-safe deadline, 50 targets and counters PASS");
}
