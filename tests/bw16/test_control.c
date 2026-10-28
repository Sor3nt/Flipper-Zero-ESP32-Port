#include "bw16_control.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static char sent[48];
static unsigned sends;
static bool tx_ok = true;
static bool transmit(void* context, const char* text) {
    assert(context == &sends);
    assert(strlen(text) < sizeof(sent));
    strcpy(sent, text); ++sends;
    return tx_ok;
}
static void init(Bw16Control* c) {
    bw16_control_init(c, transmit, &sends); tx_ok=true; sends=0; sent[0]=0;
}
static void scan(Bw16Control* c) {
    assert(bw16_control_scan(c, false, 10));
    assert(!strcmp(sent,"SCAN\n") && c->state==Bw16Scan && !c->aps_valid);
    bw16_control_line(c,"Cafe, west,6,001122334455,-42",20);
    bw16_control_line(c,"Cafe, west,6,001122334455,-42",21); /* Do not deduplicate. */
    bw16_control_line(c,"Other,36,AA1122334455,-50",22);
    bw16_control_line(c,"LIST_STATION_DONE",23);
    assert(c->state==Bw16Scan && !c->aps_valid);
    bw16_control_line(c,"SCAN_DONE",30);
    assert(c->state==Bw16Idle && c->aps_valid && c->ap_count==3);
    assert(!strcmp(c->aps[0].ssid,"Cafe, west"));
    assert(!c->indices_valid && !bw16_control_start(c,Bw16TargetStation,0,0,31));
    assert(bw16_control_scan(c,true,32));
    bw16_control_line(c,"LIST_STATION_START",33);
    bw16_control_line(c,"list-station:Cafe, west,6,001122334455,-42",34);
    bw16_control_line(c,"list-station:Cafe, west,6,001122334455,-42",35);
    bw16_control_line(c,"list-station:Other,36,AA1122334455,-50",36);
    bw16_control_line(c,"LIST_STATION_DONE",37);
    assert(c->indices_valid && c->ap_count==3);
}
static void capabilities(void) {
    Bw16Control c;init(&c);
    assert(!c.native_selected);
    assert(bw16_control_capabilities(&c, 100));
    assert(c.state == Bw16Capabilities && bw16_control_busy(&c));
    assert(!strcmp(sent, "BW16_CAPS\n") && sends == 1 && !c.remote_unknown);
    assert(!bw16_control_scan(&c, false, 101));
    assert(!bw16_control_capabilities(&c, 102));
    assert(!bw16_control_stop(&c, 103) && sends == 1);
    bw16_control_line(&c, "TARGETED_START", 104);
    bw16_control_line(&c, "BW16_CAPS:2", 105);
    bw16_control_line(&c, "BW16_CAPS:01", 106);
    bw16_control_line(&c, "BW16_CAPS:1 extra", 107);
    assert(c.state == Bw16Capabilities && !c.native_selected && sends == 1);
    bw16_control_line(&c, "BW16_CAPS:1", 108);
    assert(c.state == Bw16Idle && c.native_selected && !c.restart_required);
    scan(&c);assert(c.native_selected); /* Scans do not lose an observed capability. */

    init(&c);const uint32_t start = UINT32_MAX - 50U;
    assert(bw16_control_capabilities(&c, start));
    bw16_control_tick(&c, start + 999U);
    assert(c.state == Bw16Capabilities && !c.native_selected);
    bw16_control_tick(&c, start + 1000U);
    assert(c.state == Bw16Idle && !c.native_selected && !c.restart_required && !c.remote_unknown);
    bw16_control_line(&c, "BW16_CAPS:1", start + 1001U);
    assert(!c.native_selected); /* A late, uncorrelated reply cannot enable a new session. */
    assert(sends == 1);scan(&c);assert(!c.native_selected);

    init(&c);
    assert(bw16_control_capabilities(&c, start));
    bw16_control_line(&c, "BW16_CAPS:1", start + 1000U); /* RX precedes tick in the app. */
    assert(c.state == Bw16Idle && !c.native_selected && !c.restart_required && sends == 1);

    init(&c);tx_ok = false;
    assert(!bw16_control_capabilities(&c, 10));
    assert(c.state == Bw16Fault && !c.native_selected && !c.remote_unknown);
    puts("capabilities: read-only query, exact version, legacy timeout, wrap and TX failure PASS");
}
static void native_control(void) {
    Bw16Control c;init(&c);scan(&c);
    const unsigned targets[] = {2, 0};
    unsigned before = sends;
    assert(!bw16_control_start_selected(&c, targets, 2, 100) && sends == before);
    c.native_selected = true;
    assert(!bw16_control_start(&c, Bw16TargetSelected, 0, 0, 100) && sends == before);
    assert(!bw16_control_start_selected(&c, NULL, 2, 100));
    assert(!bw16_control_start_selected(&c, targets, 0, 100));
    assert(!bw16_control_start_selected(&c, targets, BW16_MAX_APS + 1, 100));
    const unsigned duplicate[] = {2, 2}, bad[] = {0, 3};
    assert(!bw16_control_start_selected(&c, duplicate, 2, 100));
    assert(!bw16_control_start_selected(&c, bad, 2, 100) && sends == before);
    assert(bw16_control_start_selected(&c, targets, 2, 100));
    assert(!strcmp(sent, "DEAUTH_SELECTED 0000000000005\n"));
    assert(c.target == Bw16TargetSelected && c.selected_mask == UINT64_C(5));
    assert(c.state == Bw16Starting && c.remote_unknown && !c.selected_ap_valid);
    bw16_control_line(&c, "SELECTED_AP:0", 100);
    assert(c.state == Bw16Starting && c.selected_ap_valid && c.selected_ap == 0);
    bw16_control_line(&c, "TARGETED_START", 101);
    bw16_control_line(&c, "DEAUTH_ALL_START", 102);
    assert(c.state == Bw16Starting);
    bw16_control_line(&c, "SELECTED_START", 103);
    bw16_control_line(&c, "SELECTED_AP:2", 104);
    assert(c.state == Bw16Active && c.selected_ap_valid && c.selected_ap == 2);
    bw16_control_line(&c, "TX:12345", 105);assert(c.counter == 12345);
    bw16_control_line(&c, "TARGETED_DONE", 106);
    bw16_control_line(&c, "DEAUTH_ALL_STOPPED", 107);
    assert(c.state == Bw16Active && c.remote_unknown);
    before = sends;
    bw16_control_tick(&c, 30099);assert(c.state == Bw16Active && sends == before);
    bw16_control_tick(&c, 30100);assert(c.state == Bw16Stopping && sends == before + 1);
    assert(!strcmp(sent, "STOP\n"));
    bw16_control_line(&c, "SELECTED_DONE", 30101);
    assert(c.state == Bw16Stopped && !c.remote_unknown);
    assert(bw16_control_start(&c, Bw16TargetStation, 0, 0, 40000));
    bw16_control_line(&c, "SELECTED_START", 40001);
    bw16_control_line(&c, "SELECTED_AP:999", 40002);
    bw16_control_line(&c, "SELECTED_DONE", 40003);
    assert(c.state == Bw16Starting && !c.restart_required);
    puts("native control: complete mask validation, selected status, marker isolation and one 30s stop PASS");
}
static void capture_ready(Bw16Control* c) {
    init(c);assert(bw16_control_capabilities(c,0));
    bw16_control_line(c,"BW16_PCAP:1",1);
    assert(c->state==Bw16Capabilities && c->capture_supported && !c->native_selected);
    bw16_control_line(c,"BW16_CAPS:1",2);
    assert(c->state==Bw16Idle && c->capture_supported && c->native_selected);
    scan(c);assert(c->capture_supported);
}
static void capture_capabilities(void) {
    Bw16Control c;init(&c);assert(!c.capture_supported && bw16_control_capabilities(&c,0));
    bw16_control_line(&c,"BW16_PCAP:01",1);bw16_control_line(&c,"BW16_PCAP:1 extra",2);
    assert(!c.capture_supported);
    bw16_control_line(&c,"BW16_CAPS:1",3);assert(c.native_selected && !c.capture_supported);
    bw16_control_line(&c,"BW16_PCAP:1",4);assert(!c.capture_supported);
    init(&c);assert(bw16_control_capabilities(&c,UINT32_MAX-10U));
    bw16_control_line(&c,"BW16_PCAP:1",UINT32_MAX-9U);
    bw16_control_tick(&c,989U);
    assert(c.state==Bw16Idle && !c.capture_supported && !c.native_selected);
    init(&c);assert(bw16_control_capabilities(&c,0));
    bw16_control_line(&c,"BW16_PCAP:1",1);
    bw16_control_line(&c,"BW16_CAPS:1",BW16_CAPS_TIMEOUT_MS);
    assert(c.state==Bw16Idle && !c.capture_supported && !c.native_selected);
    puts("capture caps: exact flag, legacy fallback and partial reply timeout PASS");
}
static void capture_control(void) {
    Bw16Control c;init(&c);scan(&c);unsigned before=sends;
    assert(!bw16_control_start_capture(&c,0,1,100) && sends==before);
    capture_ready(&c);before=sends;
    assert(!bw16_control_start_capture(NULL,0,1,100));
    assert(!bw16_control_start_capture(&c,0,0,100));
    assert(!bw16_control_start_capture(&c,c.ap_count,1,100));
    assert(!bw16_control_start(&c,Bw16TargetCapture,0,0,100));
    c.indices_valid=false;assert(!bw16_control_start_capture(&c,0,1,100));
    c.indices_valid=true;c.aps_valid=false;assert(!bw16_control_start_capture(&c,0,1,100));
    c.aps_valid=true;c.remote_unknown=true;assert(!bw16_control_start_capture(&c,0,1,100));
    c.remote_unknown=false;assert(sends==before);
    assert(bw16_control_start_capture(&c,2,0xa1b2c3d4U,100));
    assert(!strcmp(sent,"CAPTURE 2 a1b2c3d4\n") && c.target==Bw16TargetCapture);
    assert(c.state==Bw16Starting && c.remote_unknown && !c.start_confirmed &&
        c.selected_ap_valid && c.selected_ap==2 && c.capture_session==0xa1b2c3d4U);
    before=sends;
    assert(!bw16_control_start_capture(&c,0,1,101) && !bw16_control_scan(&c,false,101));
    const char* ignored[]={"TARGETED_START","SELECTED_START","TARGETED_DONE","CAPTURE_START:",
        "CAPTURE_START:a1b2","CAPTURE_START:a1b2c3d5","CAPTURE_START:a1b2c3d4 extra",
        "CAPTURE_DONE:a1b2c3d5","TX:123"};
    for(unsigned i=0;i<sizeof(ignored)/sizeof(*ignored);++i)bw16_control_line(&c,ignored[i],102+i);
    assert(c.state==Bw16Starting && !c.start_confirmed && !c.counter && sends==before);
    bw16_control_line(&c,"CAPTURE_START:A1B2C3D4",120);
    assert(c.state==Bw16Active && c.start_confirmed);
    bw16_control_line(&c,"CAPTURE_STATS:a1b2c3d4:7:2",121);
    assert(c.capture_emitted==7 && c.capture_dropped==2);
    const char* bad_stats[]={"CAPTURE_STATS:a1b2c3d5:99:99","CAPTURE_STATS:a1b2c3d4:6:2",
        "CAPTURE_STATS:a1b2c3d4:7:1","CAPTURE_STATS:a1b2c3d4:4294967296:2",
        "CAPTURE_STATS:a1b2c3d4:7:4294967296","CAPTURE_STATS:a1b2c3d4:7:2 extra",
        "CAPTURE_STATS:a1b2c3d4::2","CAPTURE_STATS:a1b2c3d4:7:"};
    for(unsigned i=0;i<sizeof(bad_stats)/sizeof(*bad_stats);++i)bw16_control_line(&c,bad_stats[i],122+i);
    assert(c.capture_emitted==7 && c.capture_dropped==2);
    bw16_control_tick(&c,100+BW16_OPERATION_MS);assert(c.state==Bw16Active && sends==before);
    bw16_control_tick(&c,100+BW16_CAPTURE_MS-1U);assert(c.state==Bw16Active);
    bw16_control_tick(&c,100+BW16_CAPTURE_MS);
    assert(c.state==Bw16Stopping && sends==before+1 && !strcmp(sent,"STOP\n"));
    assert(bw16_control_stop(&c,101+BW16_CAPTURE_MS) && sends==before+1);
    bw16_control_line(&c,"CAPTURE_DONE:a1b2c3d4 extra",102+BW16_CAPTURE_MS);
    assert(c.state==Bw16Stopping && c.remote_unknown);
    bw16_control_line(&c,"CAPTURE_DONE:A1B2C3D4",103+BW16_CAPTURE_MS);
    assert(c.state==Bw16Stopped && !c.remote_unknown && !c.restart_required);
    bw16_control_line(&c,"CAPTURE_START:a1b2c3d4",104+BW16_CAPTURE_MS);assert(c.state==Bw16Stopped);
    puts("capture control: nonce isolation, counters, 120s deadline and confirmed STOP PASS");
}
static void capture_stop_races(void) {
    Bw16Control c;capture_ready(&c);
    assert(bw16_control_start_capture(&c,0,1,100));
    bw16_control_line(&c,"CAPTURE_DONE:00000001",101);
    assert(c.state==Bw16Fault && c.restart_required && !c.remote_unknown && !c.start_confirmed);
    capture_ready(&c);assert(bw16_control_start_capture(&c,0,1,100));assert(bw16_control_stop(&c,101));
    bw16_control_line(&c,"CAPTURE_START:00000001",102);
    assert(c.state==Bw16Stopping && c.start_confirmed);
    bw16_control_line(&c,"CAPTURE_DONE:00000001",103);assert(c.state==Bw16Stopped && !c.remote_unknown);
    capture_ready(&c);assert(bw16_control_start_capture(&c,0,1,100));assert(bw16_control_stop(&c,101));
    bw16_control_line(&c,"CAPTURE_DONE:00000001",102);
    assert(c.state==Bw16Stopped && !c.start_confirmed && !c.remote_unknown);
    capture_ready(&c);assert(bw16_control_start_capture(&c,0,1,100));
    bw16_control_tick(&c,100+BW16_START_TIMEOUT_MS);
    assert(c.state==Bw16Stopping && c.restart_required && c.remote_unknown);
    bw16_control_line(&c,"CAPTURE_START:00000001",101+BW16_START_TIMEOUT_MS);
    assert(c.state==Bw16Stopping && !c.start_confirmed);
    bw16_control_line(&c,"CAPTURE_DONE:00000001",102+BW16_START_TIMEOUT_MS);
    assert(c.state==Bw16Fault && !c.remote_unknown);
    capture_ready(&c);assert(bw16_control_start_capture(&c,0,1,100));
    bw16_control_line(&c,"CAPTURE_START:00000001",100+BW16_START_TIMEOUT_MS);
    assert(c.state==Bw16Stopping && c.restart_required && !c.start_confirmed);
    capture_ready(&c);assert(bw16_control_start_capture(&c,0,1,100));
    bw16_control_line(&c,"CAPTURE_START:00000001",101);assert(bw16_control_stop(&c,102));
    bw16_control_line(&c,"CAPTURE_DONE:00000001",102+BW16_STOP_TIMEOUT_MS);
    assert(c.state==Bw16Fault && !c.remote_unknown && c.restart_required);
    capture_ready(&c);const uint32_t start=UINT32_MAX-50U;
    assert(bw16_control_start_capture(&c,0,1,start));
    bw16_control_line(&c,"CAPTURE_START:00000001",start+1U);
    bw16_control_tick(&c,start+BW16_CAPTURE_MS-1U);assert(c.state==Bw16Active);
    bw16_control_tick(&c,start+BW16_CAPTURE_MS);assert(c.state==Bw16Stopping);
    capture_ready(&c);tx_ok=false;assert(!bw16_control_start_capture(&c,0,1,100));
    assert(c.state==Bw16Fault && c.remote_unknown);
    puts("capture races: early DONE, cancel before START, late replies, wrap and TX failure PASS");
}
static void capture_deauth_ready(Bw16Control* c) {
    init(c);
    assert(!c->capture_deauth_supported && !c->capture_with_deauth &&
           !c->capture_deauth_reported && !c->capture_tx_attempts);
    assert(bw16_control_capabilities(c,0));
    bw16_control_line(c,"BW16_PCAP:1",1);
    bw16_control_line(c,"BW16_PCAP_DEAUTH:1",2);
    assert(c->state==Bw16Capabilities && c->capture_supported && c->capture_deauth_supported);
    bw16_control_line(c,"BW16_CAPS:1",3);
    assert(c->state==Bw16Idle && c->native_selected);
    scan(c);assert(c->capture_supported && c->capture_deauth_supported);
}
static void capture_deauth_capabilities(void) {
    Bw16Control c;capture_ready(&c);
    unsigned before=sends;
    assert(!c.capture_deauth_supported);
    assert(!bw16_control_start_capture_deauth(&c,0,1,100) && sends==before);
    assert(c.state==Bw16Idle); /* 4.3 never receives an active fallback command. */
    assert(bw16_control_start_capture(&c,0,1,100) && !strcmp(sent,"CAPTURE 0 00000001\n"));
    assert(!c.capture_with_deauth);
    capture_deauth_ready(&c);
    assert(bw16_control_capabilities(&c,100));
    assert(!c.capture_supported && !c.capture_deauth_supported && !c.native_selected);
    const char* malformed[]={"BW16_PCAP_DEAUTH:01","BW16_PCAP_DEAUTH:2",
        "BW16_PCAP_DEAUTH:1 extra","BW16_PCAP_DEAUTH:"};
    for(unsigned i=0;i<sizeof(malformed)/sizeof(*malformed);++i)bw16_control_line(&c,malformed[i],101+i);
    assert(!c.capture_deauth_supported);
    bw16_control_line(&c,"BW16_PCAP:1",110);bw16_control_line(&c,"BW16_CAPS:1",111);
    assert(c.capture_supported && !c.capture_deauth_supported && c.state==Bw16Idle);
    bw16_control_line(&c,"BW16_PCAP_DEAUTH:1",112);assert(!c.capture_deauth_supported);
    init(&c);assert(bw16_control_capabilities(&c,UINT32_MAX-10U));
    bw16_control_line(&c,"BW16_PCAP:1",UINT32_MAX-9U);
    bw16_control_line(&c,"BW16_PCAP_DEAUTH:1",UINT32_MAX-8U);
    bw16_control_tick(&c,989U);
    assert(c.state==Bw16Idle && !c.capture_supported && !c.capture_deauth_supported);
    init(&c);assert(bw16_control_capabilities(&c,0));
    bw16_control_line(&c,"BW16_PCAP:1",1);bw16_control_line(&c,"BW16_PCAP_DEAUTH:1",2);
    bw16_control_line(&c,"BW16_CAPS:1",BW16_CAPS_TIMEOUT_MS);
    assert(c.state==Bw16Idle && !c.capture_supported && !c.capture_deauth_supported);
    init(&c);assert(bw16_control_capabilities(&c,0));
    bw16_control_line(&c,"BW16_PCAP_DEAUTH:1",1);bw16_control_line(&c,"BW16_CAPS:1",2);
    scan(&c);before=sends;
    assert(!bw16_control_start_capture_deauth(&c,0,1,100) && sends==before);
    puts("deauth capture caps: exact negotiation, reset/expiry, missing base protocol and 4.3 refusal PASS");
}
static void capture_deauth_control(void) {
    Bw16Control c;capture_deauth_ready(&c);unsigned before=sends;
    assert(!bw16_control_start_capture_deauth(NULL,0,1,100));
    assert(!bw16_control_start_capture_deauth(&c,0,0,100));
    assert(!bw16_control_start_capture_deauth(&c,c.ap_count,1,100));
    c.indices_valid=false;assert(!bw16_control_start_capture_deauth(&c,0,1,100));c.indices_valid=true;
    c.remote_unknown=true;assert(!bw16_control_start_capture_deauth(&c,0,1,100));c.remote_unknown=false;
    assert(sends==before && bw16_control_start_capture_deauth(&c,2,0xa1b2c3d4U,100));
    assert(!strcmp(sent,"CAPTURE_DEAUTH 2 a1b2c3d4\n"));
    assert(c.state==Bw16Starting && c.target==Bw16TargetCapture && c.capture_with_deauth &&
           c.capture_session==0xa1b2c3d4U && !c.capture_deauth_reported && !c.capture_tx_attempts);
    before=sends;
    assert(!bw16_control_start_capture_deauth(&c,0,2,101));
    assert(!bw16_control_start_capture(&c,0,2,101) && sends==before);
    bw16_control_line(&c,"CAPTURE_DEAUTH:a1b2c3d4:3",102);
    assert(c.state==Bw16Starting && !c.capture_deauth_reported && !c.capture_tx_attempts);
    bw16_control_line(&c,"CAPTURE_START:a1b2c3d5",103);assert(!c.start_confirmed);
    bw16_control_line(&c,"CAPTURE_START:A1B2C3D4",104);
    assert(c.state==Bw16Active && c.start_confirmed);
    const char* malformed[]={"CAPTURE_DEAUTH:a1b2c3d5:3","CAPTURE_DEAUTH:a1b2c3d4:",
        "CAPTURE_DEAUTH:a1b2c3d4:-1","CAPTURE_DEAUTH:a1b2c3d4:+1","CAPTURE_DEAUTH:a1b2c3d4:4",
        "CAPTURE_DEAUTH:a1b2c3d4:4294967296","CAPTURE_DEAUTH:a1b2c3d4:99999999999999999999999999",
        "CAPTURE_DEAUTH:a1b2c3d4:3 extra","CAPTURE_DEAUTH:a1b2c3d4:3:0",
        "CAPTURE_DEAUTH:a1b2c3d4: 3","CAPTURE_DEAUTH:a1b2c3d4:03",
        "CAPTURE_DEAUTH:a1b2c3d4:00","CAPTURE_DEAUTH:a1b2:3","CAPTURE_DEAUTH::3"};
    for(unsigned i=0;i<sizeof(malformed)/sizeof(*malformed);++i)bw16_control_line(&c,malformed[i],105+i);
    assert(c.state==Bw16Active && !c.capture_deauth_reported && !c.capture_tx_attempts && sends==before);
    bw16_control_line(&c,"CAPTURE_DEAUTH:A1B2C3D4:3",130);
    assert(c.capture_deauth_reported && c.capture_tx_attempts==3 && !c.counter &&
           !c.capture_emitted && c.state==Bw16Active);
    bw16_control_line(&c,"CAPTURE_DEAUTH:a1b2c3d4:0",131);assert(c.capture_tx_attempts==3);
    bw16_control_line(&c,"CAPTURE_DONE:a1b2c3d5",132);assert(c.state==Bw16Active);
    bw16_control_tick(&c,100+BW16_OPERATION_MS);assert(c.state==Bw16Active && sends==before);
    bw16_control_tick(&c,100+BW16_CAPTURE_MS-1U);assert(c.state==Bw16Active);
    bw16_control_tick(&c,100+BW16_CAPTURE_MS);
    assert(c.state==Bw16Stopping && sends==before+1 && !strcmp(sent,"STOP\n"));
    assert(bw16_control_stop(&c,101+BW16_CAPTURE_MS) && sends==before+1);
    bw16_control_line(&c,"CAPTURE_DONE:a1b2c3d4",102+BW16_CAPTURE_MS);
    assert(c.state==Bw16Stopped && !c.remote_unknown && !c.restart_required);
    assert(c.capture_tx_attempts==3 && !c.capture_emitted); /* Attempts prove no received handshake. */
    assert(bw16_control_start_capture(&c,0,2,200000));
    assert(!strcmp(sent,"CAPTURE 0 00000002\n") && !c.capture_with_deauth &&
           !c.capture_deauth_reported && !c.capture_tx_attempts);
    bw16_control_line(&c,"CAPTURE_START:00000002",200001);
    bw16_control_line(&c,"CAPTURE_DEAUTH:00000002:3",200002);
    assert(!c.capture_deauth_reported && !c.capture_tx_attempts && c.state==Bw16Active);
    assert(bw16_control_stop(&c,200003));bw16_control_line(&c,"CAPTURE_DONE:00000002",200004);
    assert(bw16_control_start_capture_deauth(&c,0,3,300000));
    assert(c.capture_with_deauth && !c.capture_deauth_reported && !c.capture_tx_attempts);
    bw16_control_line(&c,"CAPTURE_START:00000003",300001);
    bw16_control_line(&c,"CAPTURE_DEAUTH:a1b2c3d4:3",300002);
    assert(!c.capture_deauth_reported && !c.capture_tx_attempts);
    bw16_control_line(&c,"CAPTURE_DEAUTH:00000003:0",300003);
    assert(c.capture_deauth_reported && !c.capture_tx_attempts);
    bw16_control_line(&c,"CAPTURE_DONE:00000003",300004);
    assert(c.state==Bw16Stopped);
    puts("deauth capture: exact command, nonce/bounds, attempts-only telemetry, 120s stop and passive reset PASS");
}
static void capture_deauth_failures(void) {
    Bw16Control c;
    for(unsigned count=0;count<=3;++count) {
        capture_deauth_ready(&c);assert(bw16_control_start_capture_deauth(&c,0,1,100));
        bw16_control_line(&c,"CAPTURE_START:00000001",101);
        char line[48];snprintf(line,sizeof(line),"CAPTURE_DEAUTH:00000001:%u",count);
        bw16_control_line(&c,line,102);assert(c.capture_deauth_reported && c.capture_tx_attempts==count);
    }
    capture_deauth_ready(&c);assert(bw16_control_start_capture_deauth(&c,0,1,100));
    bw16_control_line(&c,"CAPTURE_DONE:00000001",101);
    assert(c.state==Bw16Fault && c.restart_required && !c.remote_unknown);
    capture_deauth_ready(&c);assert(bw16_control_start_capture_deauth(&c,0,1,100));
    assert(bw16_control_stop(&c,101));unsigned before=sends;
    bw16_control_line(&c,"CAPTURE_DEAUTH:00000001:3",102);
    assert(!c.capture_deauth_reported && !c.capture_tx_attempts);
    bw16_control_line(&c,"CAPTURE_START:00000001",103);
    assert(c.state==Bw16Stopping && c.start_confirmed && sends==before);
    bw16_control_line(&c,"CAPTURE_DEAUTH:00000001:0",104);
    assert(c.state==Bw16Stopping && c.capture_deauth_reported && !c.capture_tx_attempts && sends==before);
    bw16_control_line(&c,"CAPTURE_DONE:00000001",105);assert(c.state==Bw16Stopped && !c.remote_unknown);
    capture_deauth_ready(&c);assert(bw16_control_start_capture_deauth(&c,0,1,100));
    bw16_control_tick(&c,100+BW16_START_TIMEOUT_MS);
    assert(c.state==Bw16Stopping && c.restart_required && c.remote_unknown && !strcmp(sent,"STOP\n"));
    before=sends;bw16_control_line(&c,"CAPTURE_START:00000001",101+BW16_START_TIMEOUT_MS);
    bw16_control_line(&c,"CAPTURE_DEAUTH:00000001:3",102+BW16_START_TIMEOUT_MS);
    assert(c.state==Bw16Stopping && !c.start_confirmed && !c.capture_deauth_reported && sends==before);
    bw16_control_tick(&c,100+BW16_START_TIMEOUT_MS+BW16_STOP_TIMEOUT_MS);
    assert(c.state==Bw16Fault && c.remote_unknown);
    capture_deauth_ready(&c);assert(bw16_control_start_capture_deauth(&c,0,1,100));
    bw16_control_line(&c,"ERR CAPTURE_FAILED",101);
    assert(c.state==Bw16Stopping && c.restart_required && !c.start_confirmed && !strcmp(sent,"STOP\n"));
    capture_deauth_ready(&c);assert(bw16_control_start_capture_deauth(&c,0,1,100));
    bw16_control_line(&c,"CAPTURE_START:00000001",101);
    bw16_control_error(&c,"SD write failed",102);before=sends;
    assert(c.state==Bw16Stopping && c.restart_required && !strcmp(sent,"STOP\n"));
    assert(!bw16_control_start_capture_deauth(&c,0,2,103) && sends==before);
    bw16_control_line(&c,"CAPTURE_DONE:00000001",104);
    assert(c.state==Bw16Fault && !c.remote_unknown);
    capture_deauth_ready(&c);tx_ok=false;
    assert(!bw16_control_start_capture_deauth(&c,0,1,100));
    assert(c.state==Bw16Fault && c.remote_unknown && c.capture_with_deauth);
    puts("deauth capture failures: start/stop races, missing START, SD fault and TX failure PASS");
}
int main(void) {
    capabilities();native_control();capture_capabilities();capture_control();capture_stop_races();capture_deauth_capabilities();capture_deauth_control();capture_deauth_failures();
    Bw16Control c;
    init(&c);
    assert(!bw16_control_start(&c,Bw16TargetAll,0,0,0) && sends==0);
    assert(!bw16_control_clients(&c,0,0));
    scan(&c);
    assert(!bw16_control_start(&c,Bw16TargetStation,3,0,40));
    assert(!bw16_control_start(&c,Bw16TargetClient,0,0,40));
    assert(bw16_control_clients(&c,2,40) && !strcmp(sent,"LIST_CLIENTS 2\n"));
    bw16_control_line(&c,"SCAN_DONE",50); assert(!c.clients_valid);
    bw16_control_line(&c,"list-client:102030405060",50);
    bw16_control_line(&c,"list-client:102030405060",51);
    bw16_control_line(&c,"LIST_CLIENTS_DONE",60);
    assert(c.clients_valid && c.client_count==2 && c.client_ap==2);
    assert(!bw16_control_start(&c,Bw16TargetClient,1,1,100));
    assert(bw16_control_start(&c,Bw16TargetClient,2,1,100));
    assert(!strcmp(sent,"DEAUTH_CLIENT 2 1\n") && c.state==Bw16Starting && c.remote_unknown);
    bw16_control_line(&c,"DEAUTH_ALL_START",110); assert(c.state==Bw16Starting);
    bw16_control_line(&c,"TARGETED_START",120); assert(c.state==Bw16Active);
    bw16_control_line(&c,"TX:123",130); assert(c.counter==123);
    assert(!bw16_control_scan(&c,false,140));
    bw16_control_tick(&c,30100); assert(c.state==Bw16Stopping && !strcmp(sent,"STOP\n"));
    unsigned before=sends; assert(bw16_control_stop(&c,30110) && sends==before);
    bw16_control_line(&c,"DEAUTH_ALL_STOPPED",30120); assert(c.state==Bw16Stopping);
    bw16_control_line(&c,"TARGETED_START",30121); assert(c.state==Bw16Stopping);
    bw16_control_line(&c,"TARGETED_DONE",30130); assert(c.state==Bw16Stopped && !c.remote_unknown);

    assert(bw16_control_start(&c,Bw16TargetAll,0,0,40000));
    assert(!strcmp(sent,"DEAUTH_ALL\n"));
    bw16_control_line(&c,"DEAUTH_ALL_START",40001);
    bw16_control_line(&c,"CYCLES: 4294967295",40002); assert(c.counter==UINT32_MAX);
    bw16_control_line(&c,"CYCLES: 4294967296",40003);
    assert(c.state==Bw16Stopping && c.restart_required && !strcmp(sent,"STOP\n"));
    bw16_control_line(&c,"DEAUTH_ALL_STOPPED",40004);
    assert(c.state==Bw16Fault && !c.remote_unknown && !bw16_control_scan(&c,false,50000));

    init(&c); scan(&c);
    assert(bw16_control_start(&c,Bw16TargetStation,1,0,100));
    assert(!strcmp(sent,"DEAUTH_STATION 1\n"));
    bw16_control_tick(&c,5100);
    assert(c.state==Bw16Stopping && c.remote_unknown && c.restart_required);
    bw16_control_tick(&c,10100);
    assert(c.state==Bw16Fault && c.remote_unknown && strstr(c.error,"STOP unconfirmed"));

    init(&c); scan(&c); tx_ok=false;
    assert(!bw16_control_start(&c,Bw16TargetStation,0,0,100));
    assert(c.state==Bw16Fault && c.remote_unknown);
    init(&c); scan(&c);
    assert(bw16_control_start(&c,Bw16TargetStation,0,0,UINT32_MAX-100));
    bw16_control_line(&c,"TARGETED_START",UINT32_MAX-90);
    bw16_control_tick(&c,BW16_OPERATION_MS-101); assert(c.state==Bw16Stopping);
    tx_ok=false; /* STOP was already sent; a further click must not repeat it. */
    before=sends; bw16_control_stop(&c,40000); assert(sends==before);

    init(&c); assert(bw16_control_scan(&c,true,0));
    assert(!strcmp(sent,"DEAUTH_STATION\n"));
    bw16_control_line(&c,"Bootstrap,1,001122334455,-20",1);
    bw16_control_line(&c,"SCAN_DONE",2); assert(!c.ap_count && !c.aps_valid);
    bw16_control_line(&c,"LIST_STATION_START",3);
    bw16_control_line(&c,"list-station:Home,6,112233445566,-30",4);
    bw16_control_line(&c,"LIST_STATION_DONE",5); assert(c.aps_valid && c.ap_count==1);
    assert(bw16_control_clients(&c,0,6));
    bw16_control_line(&c,"LIST_CLIENTS_DONE",7); assert(c.clients_valid && !c.client_count);

    init(&c); scan(&c); assert(bw16_control_clients(&c,0,100));
    for(unsigned i=0;i<40;++i) bw16_control_line(&c,"list-client:001122334455",101+i);
    bw16_control_line(&c,"LIST_CLIENTS_DONE",200);
    assert(c.client_count==BW16_MAX_CLIENTS && c.client_total==40);
    assert(!bw16_control_start(&c,Bw16TargetClient,0,32,300));
    assert(bw16_control_start(&c,Bw16TargetClient,0,31,300));
    bw16_control_error(&c,"UART RX overflow",301);
    assert(c.state==Bw16Stopping && c.restart_required && !strcmp(sent,"STOP\n"));
    bw16_control_line(&c,"TARGETED_DONE",302); assert(c.state==Bw16Fault && !c.remote_unknown);

    init(&c); assert(bw16_control_scan(&c,false,0));
    for(unsigned i=0;i<51;++i) bw16_control_line(&c,"AP,1,001122334455,-20",i+1);
    bw16_control_line(&c,"SCAN_DONE",100);
    assert(c.ap_count==50 && c.ap_total==51 && c.aps_valid);
    assert(!bw16_control_start(&c,Bw16TargetAll,0,0,200));
    assert(bw16_control_scan(&c,false,300) && !c.clients_valid && !c.aps_valid);
    bw16_control_line(&c,"Bad,1,BADMAC,-20",301); assert(c.state==Bw16Fault);

    init(&c); assert(bw16_control_scan(&c,false,0));
    bw16_control_line(&c,"SCAN_DONE",1); assert(c.aps_valid && !c.ap_count);
    assert(!bw16_control_start(&c,Bw16TargetAll,0,0,2));
    init(&c); scan(&c); assert(bw16_control_clients(&c,0,0));
    bw16_control_line(&c,"list-client:BAD",1); assert(c.state==Bw16Fault);
    init(&c); assert(bw16_control_scan(&c,false,UINT32_MAX-5));
    bw16_control_tick(&c,19994); assert(c.state==Bw16Fault);
    for(unsigned indexed=0;indexed<2;++indexed) {
        init(&c);assert(bw16_control_scan(&c,indexed!=0,10));
        bw16_control_line(&c,"ERR scan busy",11);
        assert(c.state==Bw16Fault && c.restart_required && !c.remote_unknown && sends==1);
        assert(strstr(c.error,"rejected scan/list"));
    }
    puts("control: indexed lists, confirmed START/STOP, bounds, timeout/wrap, RX/TX faults and no implicit operation PASS");
}
