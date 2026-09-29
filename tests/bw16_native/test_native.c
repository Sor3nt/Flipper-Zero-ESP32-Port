/* Exercise the production BW16 core and its protocol with all RF/serial I/O mocked. */
#include "bw16_native.h"
#include "bw16_control.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

static char lines[2048][128];
static unsigned line_count, scan_calls, observe_calls, observe_ends, channel_count, frame_count;
static bool scan_ok, observe_ok, channel_ok;
static bool capture_ok, capture_drain, capture_armed;
static unsigned capture_calls, capture_ends, capture_deauth_calls, capture_attempts;
static uint32_t capture_id;
static BwnAp capture_ap;
static uint8_t channel_now;
static struct { uint8_t source[6], dest[6], channel; uint16_t reason; } frames[4096];
static uint8_t channels[4096];
static BwnController fw;
static uint32_t clock_now;
static void emit(void* ctx,const char* text) {
    assert(ctx==&fw && line_count<2048 && strlen(text)<128);
    if(!strncmp(text,"CAPTURE_START:",14)) assert(capture_armed);
    strcpy(lines[line_count++],text);
}
static bool scan_begin(void* ctx) { assert(ctx==&fw);++scan_calls;return scan_ok; }
static bool observe_begin(void* ctx,const BwnAp* ap) { assert(ctx==&fw && ap->channel);++observe_calls;return observe_ok; }
static void observe_end(void* ctx) { assert(ctx==&fw);++observe_ends; }
static bool set_channel(void* ctx,uint8_t channel) {
    assert(ctx==&fw && channel_count<4096);channels[channel_count++]=channel;channel_now=channel;return channel_ok;
}
static void transmit(void* ctx,const uint8_t source[6],const uint8_t dest[6],uint16_t reason) {
    assert(ctx==&fw && frame_count<4096);
    memcpy(frames[frame_count].source,source,6);memcpy(frames[frame_count].dest,dest,6);
    frames[frame_count].channel=channel_now;frames[frame_count++].reason=reason;
}
static bool capture_begin(void* ctx,const BwnAp* ap,uint32_t session) {
    assert(ctx==&fw);++capture_calls;capture_ap=*ap;capture_id=session;capture_armed=capture_ok;channel_now=ap->channel;return capture_ok;
}
static void capture_end(void* ctx) {
    assert(ctx==&fw && fw.state==BwnCapture);assert(!fw.capture_deauth_pending);++capture_ends;capture_armed=false;
    if(capture_drain) {
        emit(ctx,"CAPTURE_TEST_DRAIN");
        bwn_capture_progress(&fw,fw.capture_emitted+1,fw.capture_dropped,false);
    }
}
static unsigned capture_deauth(void* ctx,const BwnAp* ap) {
    assert(ctx==&fw && capture_armed && fw.state==BwnCapture && fw.capture_with_deauth);
    assert(!fw.capture_deauth_pending && ap==&fw.targets[0] && fw.target_count==1);
    assert(!memcmp(ap->mac,capture_ap.mac,6) && ap->channel==capture_ap.channel);
    char start[40];snprintf(start,sizeof(start),"CAPTURE_START:%08lX",(unsigned long)fw.capture_session);
    bool start_seen=false;
    for(unsigned i=0;i<line_count;++i)if(!strcmp(lines[i],start))start_seen=true;
    assert(start_seen);++capture_deauth_calls;
    const uint8_t dest[6]={255,255,255,255,255,255};const uint16_t reasons[3]={1,4,16};
    for(unsigned i=0;i<capture_attempts;++i)transmit(ctx,ap->mac,dest,reasons[i]);
    return capture_attempts;
}
static const BwnIo io={emit,scan_begin,observe_begin,observe_end,set_channel,transmit,capture_begin,capture_end,capture_deauth};
static bool has(const char* s) { for(unsigned i=0;i<line_count;++i)if(!strcmp(lines[i],s))return true;return false; }
static bool prefix(const char* s) { for(unsigned i=0;i<line_count;++i)if(!strncmp(lines[i],s,strlen(s)))return true;return false; }
static BwnAp ap(unsigned i) {
    BwnAp value={0};snprintf(value.ssid,sizeof(value.ssid),"Fixture%u",i);
    value.mac[0]=2;value.mac[4]=(uint8_t)i;value.mac[5]=0x55;
    value.channel=i%2?36:6;value.rssi=-42;return value;
}
static void reset(void) {
    line_count=scan_calls=observe_calls=observe_ends=channel_count=frame_count=0;
    capture_calls=capture_ends=capture_deauth_calls=0;capture_id=0;capture_drain=capture_armed=false;capture_ok=true;capture_attempts=3;
    scan_ok=observe_ok=channel_ok=true;clock_now=1000;
    bwn_init(&fw,&io,&fw);
}
static void cmd(const char* text) { bwn_command(&fw,text,clock_now); }
static void feed(const char* text) { for(;*text;++text)bwn_feed(&fw,(uint8_t)*text,clock_now); }
static void tick(uint32_t now) { clock_now=now;bwn_tick(&fw,now); }
static void loaded(unsigned count) {
    reset();cmd("SCAN");assert(scan_calls==1 && fw.state==BwnScan && !fw.scan_valid);
    for(unsigned i=0;i<count;++i){BwnAp value=ap(i);bwn_scan_result(&fw,&value);}
    bwn_scan_complete(&fw,true);assert(fw.state==BwnIdle && fw.scan_valid && has("SCAN_DONE"));
    cmd("DEAUTH_STATION");assert(has("LIST_STATION_START") && has("LIST_STATION_DONE"));
    assert(fw.indices_valid && fw.ap_count==(count>BWN_MAX_APS?BWN_MAX_APS:count));
    line_count=channel_count=frame_count=0;
}
static void capabilities_and_validation(void) {
    reset();cmd("BW16_CAPS");assert(has("BW16_CAPS:1") && !frame_count && !channel_count && !scan_calls);
    assert(line_count==3 && !strcmp(lines[0],"BW16_PCAP:1") &&
        !strcmp(lines[1],"BW16_PCAP_DEAUTH:1") && !strcmp(lines[2],"BW16_CAPS:1"));
    cmd("DEAUTH_SELECTED 0000000000001");assert(prefix("ERR ") && fw.state==BwnIdle && !frame_count && !channel_count);
    const char* bad[]={"DEAUTH_SELECTED 0000000000000","DEAUTH_SELECTED 4000000000000",
        "DEAUTH_SELECTED 0000000000008","DEAUTH_SELECTED -000000000001",
        "DEAUTH_SELECTED 000000000000G","DEAUTH_SELECTED 0000000000001 trailing",
        "DEAUTH_SELECTED 00000000000001","DEAUTH_SELECTED","DEAUTH_SELECTED 1"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);++i){
        loaded(3);cmd(bad[i]);assert(fw.state==BwnIdle && prefix("ERR ") && !channel_count && !frame_count);
    }
    loaded(3);feed("DEAUTH_SELECTED 0000000000005");assert(fw.state==BwnIdle && !frame_count);
    feed("\r\n");assert(fw.state==BwnSelected && has("SELECTED_START"));
    assert(fw.target_count==2 && fw.target_indices[0]==0 && fw.target_indices[1]==2);
    bwn_stop(&fw);assert(fw.state==BwnIdle && has("SELECTED_DONE"));
    puts("native: read-only capability, atomic full-mask validation and fragmented command PASS");
}
static void selected_round_robin(void) {
    loaded(3);cmd("DEAUTH_SELECTED 0000000000003");
    assert(fw.state==BwnSelected && fw.target_count==2);
    /* The operation owns a snapshot, independent of mutable scan-list storage. */
    fw.aps[0].channel=149;memset(fw.aps[0].mac,0xaa,6);
    for(unsigned ms=0;ms<500;++ms)tick(1000+ms);
    assert(channel_count==5 && frame_count==15);
    for(unsigned i=0;i<frame_count;++i){
        unsigned target=(i/3)%2;BwnAp original=ap(target);
        assert(!memcmp(frames[i].source,original.mac,6));
        assert(!memcmp(frames[i].dest,"\xff\xff\xff\xff\xff\xff",6));
        assert(frames[i].channel==original.channel);
        assert(frames[i].reason==((uint16_t[]){1,4,16})[i%3]);
    }
    assert(prefix("SELECTED_AP:") && prefix("TX:") && !has("SELECTED_DONE"));
    cmd("STOP");unsigned sent=frame_count;tick(9000);
    assert(fw.state==BwnIdle && frame_count==sent && has("SELECTED_DONE"));
    puts("native: selected-only device-local 100ms round robin, target snapshot and STOP PASS");
}
static void deadline_and_faults(void) {
    loaded(2);clock_now=UINT32_MAX-200U;cmd("DEAUTH_SELECTED 0000000000003");
    uint32_t start=clock_now;tick(start);assert(frame_count==3);
    tick(start+BWN_OPERATION_MS-1U);assert(fw.state==BwnSelected);
    unsigned sent=frame_count;feed("S");tick(start+BWN_OPERATION_MS);
    assert(fw.state==BwnIdle && has("SELECTED_DONE") && frame_count==sent);
    tick(start+BWN_OPERATION_MS+1000U);assert(frame_count==sent);
    loaded(2);cmd("DEAUTH_SELECTED 0000000000003");channel_ok=false;tick(clock_now);
    assert(fw.state==BwnIdle && !frame_count && prefix("ERR ") && has("SELECTED_DONE"));
    loaded(2);cmd("DEAUTH_SELECTED 0000000000003");tick(clock_now);
    for(unsigned i=0;i<BWN_LINE_SIZE+20;++i)bwn_feed(&fw,'X',clock_now);
    assert(fw.state==BwnIdle);sent=frame_count;feed("\n");tick(clock_now+1000);assert(frame_count==sent);
    loaded(2);cmd("DEAUTH_SELECTED 0000000000003");bwn_feed(&fw,0,clock_now);
    assert(fw.state==BwnIdle && has("SELECTED_DONE"));
    loaded(2);cmd("DEAUTH_SELECTED 0000000000003");cmd("SCAN");
    assert(scan_calls==1);cmd("STOP");assert(fw.state==BwnIdle);
    puts("native: independent wrap-safe 30s cutoff, partial input, RX overflow/NUL, radio error and no active rescan PASS");
}
static void fifty_targets(void) {
    loaded(55);cmd("DEAUTH_SELECTED 3ffffffffffff");assert(fw.target_count==50);
    for(unsigned i=0;i<50;++i)tick(1000+i*BWN_VISIT_MS);
    assert(frame_count==150 && channel_count==50);
    for(unsigned i=0;i<50;++i){BwnAp original=ap(i);assert(!memcmp(frames[i*3].source,original.mac,6));}
    cmd("STOP");assert(has("SELECTED_DONE"));
    puts("native: all 50 explicit bits visited with original indices; excess scan records bounded PASS");
}
static void legacy_and_clients(void) {
    loaded(2);cmd("DEAUTH_STATION 1");assert(has("TARGETED_START"));tick(clock_now);
    assert(frame_count>=3 && !memcmp(frames[0].source,ap(1).mac,6));cmd("STOP");assert(has("TARGETED_DONE"));
    loaded(2);cmd("DEAUTH_ALL");assert(has("DEAUTH_ALL_START"));tick(clock_now);tick(clock_now+1);
    assert(frame_count>=2);cmd("STOP");assert(has("DEAUTH_ALL_STOPPED"));
    loaded(2);cmd("LIST_CLIENTS 1");assert(fw.state==BwnObserve && observe_calls==1);
    const uint8_t client[6]={2,0x11,0x22,0x33,0x44,0x55};
    bwn_client_result(&fw,client);bwn_client_result(&fw,client);
    tick(clock_now+5000);assert(fw.state==BwnIdle && fw.client_count==1 && observe_ends==1);
    assert(has("list-client:021122334455") && has("LIST_CLIENTS_DONE"));
    cmd("DEAUTH_CLIENT 1 0");assert(has("TARGETED_START"));tick(clock_now);
    assert(frame_count>=3 && !memcmp(frames[0].dest,client,6));cmd("STOP");assert(has("TARGETED_DONE"));
    cmd("SCAN");assert(!fw.clients_valid && !fw.indices_valid);
    bwn_scan_complete(&fw,false);assert(!fw.scan_valid && prefix("ERR "));
    reset();scan_ok=false;cmd("SCAN");assert(fw.state==BwnIdle && prefix("ERR "));
    loaded(2);observe_ok=false;cmd("LIST_CLIENTS 0");assert(fw.state==BwnIdle && prefix("ERR "));
    puts("native: legacy station/client/all markers, client observation/deduplication, scan invalidation and hardware failures PASS");
}
static void frame_parsing(void) {
    BwnAp target=ap(0);uint8_t packet[32]={0},peer[6]={2,3,4,5,6,7},got[6];
    packet[0]=8;packet[1]=1; /* Data, ToDS: receiver is AP; sender is client. */
    memcpy(packet+4,target.mac,6);memcpy(packet+10,peer,6);memset(packet+16,0xff,6);
    assert(bwn_client_from_frame(packet,24,target.mac,got) && !memcmp(got,peer,6));
    assert(!bwn_client_from_frame(packet,23,target.mac,got));
    BwnAp other=ap(1);assert(!bwn_client_from_frame(packet,24,other.mac,got));
    packet[1]=2;memcpy(packet+4,peer,6);memcpy(packet+10,target.mac,6);
    assert(bwn_client_from_frame(packet,24,target.mac,got) && !memcmp(got,peer,6));
    packet[1]=3;assert(!bwn_client_from_frame(packet,30,target.mac,got));
    puts("native: bounded 802.11 client extraction and BSSID filtering PASS");
}
static void passive_capture(void) {
    const char* invalid[]={"CAPTURE 0 00000000","CAPTURE 2 11223344","CAPTURE -1 11223344",
        "CAPTURE 0 123","CAPTURE 0 11223344x","CAPTURE 0 11223344 trailing",
        "CAPTURE 0 1122334G","CAPTURE 0  11223344"};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);++i) {
        loaded(2);cmd(invalid[i]);assert(fw.state==BwnIdle && !capture_calls && !frame_count);
    }
    reset();cmd("CAPTURE 0 11223344");assert(!capture_calls && has("ERR INVALID_CAPTURE"));
    loaded(2);fw.indices_valid=false;cmd("CAPTURE 0 11223344");assert(!capture_calls);
    loaded(2);fw.aps[0].mac[0]=1;cmd("CAPTURE 0 11223344");assert(!capture_calls);
    loaded(2);fw.aps[0].channel=0;cmd("CAPTURE 0 11223344");assert(!capture_calls);
    loaded(2);capture_ok=false;cmd("CAPTURE 0 11223344");
    assert(capture_calls==1 && fw.state==BwnIdle && has("ERR CAPTURE_FAILED") && !capture_ends);
    loaded(2);fw.io.capture_end=NULL;cmd("CAPTURE 0 11223344");
    assert(!capture_calls && fw.state==BwnIdle && has("ERR CAPTURE_FAILED"));

    loaded(2);feed("CAPTURE 1 aabbCCdd\r\n");
    assert(fw.state==BwnCapture && capture_calls==1 && capture_id==0xaabbccddU);
    assert(!memcmp(capture_ap.mac,ap(1).mac,6) && has("CAPTURE_START:AABBCCDD"));
    const uint32_t start=clock_now;
    for(unsigned ms=0;ms<BWN_CAPTURE_MS;ms+=1000)tick(start+ms);
    assert(fw.state==BwnCapture && !frame_count && !channel_count && !capture_ends);
    cmd("SCAN");cmd("DEAUTH_STATION 0");assert(scan_calls==1 && has("ERR BUSY") && !frame_count);
    bwn_capture_progress(&fw,3,2,false);capture_drain=true;cmd("STOP");
    assert(fw.state==BwnIdle && capture_ends==1 && !frame_count);
    assert(!strcmp(lines[line_count-3],"CAPTURE_TEST_DRAIN"));
    assert(!strcmp(lines[line_count-2],"CAPTURE_STATS:AABBCCDD:4:2"));
    assert(!strcmp(lines[line_count-1],"CAPTURE_DONE:AABBCCDD"));
    tick(clock_now+BWN_CAPTURE_MS);cmd("STOP");assert(capture_ends==1 && !frame_count);

    loaded(1);clock_now=UINT32_MAX-200U;cmd("CAPTURE 0 01020304");
    uint32_t wrapping=clock_now;tick(wrapping+BWN_CAPTURE_MS-1U);assert(fw.state==BwnCapture);
    tick(wrapping+BWN_CAPTURE_MS);assert(fw.state==BwnIdle && capture_ends==1 && !frame_count);
    assert(has("CAPTURE_STATS:01020304:0:0") && has("CAPTURE_DONE:01020304"));
    loaded(1);cmd("CAPTURE 0 01020304");bwn_capture_progress(&fw,256,7,true);tick(clock_now);
    assert(fw.state==BwnIdle && capture_ends==1 && !frame_count && has("CAPTURE_STATS:01020304:256:7"));
    loaded(1);cmd("CAPTURE 0 01020304");feed("S");tick(clock_now+500);
    assert(fw.state==BwnIdle && capture_ends==1 && !frame_count && has("ERR UART_LINE_TIMEOUT"));
    assert(!capture_deauth_calls && !fw.capture_with_deauth && !fw.capture_deauth_pending);
    puts("native: passive capture validation, zero transmit, queued STOP, packet bound and wrap-safe 120s deadline PASS");
}
static void capture_with_deauth(void) {
    const char* invalid[]={"CAPTURE_DEAUTH 0 00000000","CAPTURE_DEAUTH 2 11223344",
        "CAPTURE_DEAUTH -1 11223344","CAPTURE_DEAUTH 0 123","CAPTURE_DEAUTH 0 11223344 trailing",
        "CAPTURE_DEAUTH 0 1122334G","CAPTURE_DEAUTH 0  11223344"};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);++i) {
        loaded(2);cmd(invalid[i]);tick(clock_now);
        assert(fw.state==BwnIdle && !capture_calls && !capture_deauth_calls && !frame_count);
    }
    reset();cmd("CAPTURE_DEAUTH 0 11223344");assert(!capture_calls && !frame_count);
    loaded(2);fw.indices_valid=false;cmd("CAPTURE_DEAUTH 0 11223344");assert(!capture_calls);
    loaded(2);fw.aps[0].mac[0]=1;cmd("CAPTURE_DEAUTH 0 11223344");assert(!capture_calls);
    loaded(2);fw.aps[0].channel=0;cmd("CAPTURE_DEAUTH 0 11223344");assert(!capture_calls);
    loaded(2);capture_ok=false;cmd("CAPTURE_DEAUTH 0 11223344");tick(clock_now);
    assert(fw.state==BwnIdle && capture_calls==1 && !capture_deauth_calls && !frame_count);
    assert(!fw.capture_deauth_pending && !has("CAPTURE_START:11223344"));
    loaded(2);fw.io.capture_deauth=NULL;cmd("CAPTURE_DEAUTH 0 11223344");tick(clock_now);
    assert(fw.state==BwnIdle && !capture_calls && !frame_count && has("ERR CAPTURE_FAILED"));

    loaded(2);feed("CAPTURE_DEAUTH 1 aaBBccDD\r\n");
    assert(fw.state==BwnCapture && capture_armed && fw.capture_with_deauth && fw.capture_deauth_pending);
    assert(capture_calls==1 && !capture_deauth_calls && !frame_count && !prefix("CAPTURE_DEAUTH:"));
    assert(has("CAPTURE_START:AABBCCDD") && fw.target_count==1 && fw.target_indices[0]==1);
    fw.aps[1].channel=149;memset(fw.aps[1].mac,0xaa,6); /* burst uses the armed snapshot */
    tick(clock_now);
    assert(capture_deauth_calls==1 && frame_count==3 && !fw.capture_deauth_pending);
    assert(has("CAPTURE_DEAUTH:AABBCCDD:3") && capture_armed && !capture_ends && !channel_count);
    for(unsigned i=0;i<3;++i) {
        BwnAp original=ap(1);
        assert(!memcmp(frames[i].source,original.mac,6) && frames[i].channel==original.channel);
        assert(!memcmp(frames[i].dest,"\xff\xff\xff\xff\xff\xff",6));
        assert(frames[i].reason==((uint16_t[]){1,4,16})[i]);
    }
    for(unsigned i=0;i<120;++i)tick(1000U+i*1000U);
    assert(capture_deauth_calls==1 && frame_count==3 && fw.state==BwnCapture);
    unsigned telemetry=0;for(unsigned i=0;i<line_count;++i)if(!strncmp(lines[i],"CAPTURE_DEAUTH:",15))++telemetry;
    assert(telemetry==1);cmd("STOP");tick(clock_now+2000);
    assert(fw.state==BwnIdle && !capture_armed && capture_ends==1 && frame_count==3);

    loaded(1);cmd("CAPTURE_DEAUTH 0 11223344");cmd("STOP");tick(clock_now);
    assert(!capture_deauth_calls && !frame_count && !fw.capture_deauth_pending && capture_ends==1);
    loaded(1);cmd("CAPTURE_DEAUTH 0 11223344");bwn_capture_progress(&fw,256,0,true);tick(clock_now);
    assert(!capture_deauth_calls && !frame_count && !fw.capture_deauth_pending && capture_ends==1);
    loaded(1);cmd("CAPTURE_DEAUTH 0 11223344");tick(clock_now+BWN_CAPTURE_MS);
    assert(!capture_deauth_calls && !frame_count && !fw.capture_deauth_pending && capture_ends==1);
    loaded(1);cmd("CAPTURE_DEAUTH 0 11223344");tick(clock_now+BWN_CAPTURE_DEAUTH_MS);
    assert(fw.state==BwnCapture && !capture_deauth_calls && !frame_count && !fw.capture_deauth_pending);
    assert(has("CAPTURE_DEAUTH:11223344:0"));tick(clock_now+1000);assert(!frame_count);
    loaded(1);cmd("CAPTURE_DEAUTH 0 11223344");feed("S");tick(clock_now+500);
    assert(fw.state==BwnIdle && !capture_deauth_calls && !frame_count);

    loaded(1);clock_now=UINT32_MAX-200U;cmd("CAPTURE_DEAUTH 0 11223344");
    tick(clock_now+BWN_CAPTURE_DEAUTH_MS-1U);assert(capture_deauth_calls==1 && frame_count==3);
    tick(clock_now+1);assert(capture_deauth_calls==1 && frame_count==3);cmd("STOP");
    loaded(1);clock_now=UINT32_MAX-200U;cmd("CAPTURE_DEAUTH 0 11223344");
    tick(clock_now+BWN_CAPTURE_DEAUTH_MS);assert(!capture_deauth_calls && !frame_count);
    loaded(1);capture_attempts=2;cmd("CAPTURE_DEAUTH 0 11223344");tick(clock_now);
    assert(frame_count==2 && has("CAPTURE_DEAUTH:11223344:2"));cmd("STOP");
    cmd("CAPTURE 0 22334455");tick(clock_now);assert(frame_count==2 && capture_deauth_calls==1);
    assert(!fw.capture_with_deauth && !fw.capture_deauth_pending);cmd("STOP");
    puts("native: explicit armed capture burst, snapshot/START order, one-shot attempts, cancel/fail/stale and rollover PASS");
}
static bool fap_send(void* context,const char* command) {
    assert(context==&fw);feed(command);return true;
}
static unsigned consumed;
static void pump(Bw16Control* fap) {
    while(consumed<line_count)bw16_control_line(fap,lines[consumed++],clock_now);
}
static void protocol_integration(void) {
    reset();consumed=0;Bw16Control fap;bw16_control_init(&fap,fap_send,&fw);
    assert(bw16_control_capabilities(&fap,clock_now));pump(&fap);assert(fap.native_selected && fap.state==Bw16Idle);
    assert(bw16_control_scan(&fap,false,clock_now));
    for(unsigned i=0;i<3;++i){BwnAp value=ap(i);bwn_scan_result(&fw,&value);}
    bwn_scan_complete(&fw,true);pump(&fap);assert(fap.aps_valid && fap.ap_count==3);
    assert(bw16_control_scan(&fap,true,clock_now));pump(&fap);assert(fap.indices_valid);
    const unsigned selected[]={0,1};
    assert(bw16_control_start_selected(&fap,selected,2,clock_now));pump(&fap);
    assert(fap.state==Bw16Active && fw.state==BwnSelected);
    tick(clock_now);pump(&fap);tick(clock_now+250);pump(&fap);
    assert(fap.counter>0 && fap.selected_ap_valid && fap.selected_ap<2);
    assert(bw16_control_stop(&fap,clock_now));pump(&fap);
    assert(fap.state==Bw16Stopped && !fap.remote_unknown && fw.state==BwnIdle);
    assert(fap.capture_supported);
    unsigned previous_tx=frame_count;
    assert(bw16_control_start_capture(&fap,1,0x1234abcdU,clock_now));pump(&fap);
    assert(fap.state==Bw16Active && fw.state==BwnCapture && fap.start_confirmed);
    bwn_capture_progress(&fw,4,2,false);tick(clock_now+31000);pump(&fap);
    assert(fap.state==Bw16Active && fw.state==BwnCapture && frame_count==previous_tx);
    assert(bw16_control_stop(&fap,clock_now));pump(&fap);
    assert(fap.state==Bw16Stopped && !fap.remote_unknown && fw.state==BwnIdle);
    assert(fap.capture_emitted==4 && fap.capture_dropped==2 && frame_count==previous_tx);
    assert(bw16_control_start_capture(&fap,1,0x2345bcdeU,clock_now));pump(&fap);
    tick(clock_now+BWN_CAPTURE_MS);pump(&fap);
    assert(fap.state==Bw16Stopped && !fap.remote_unknown && fw.state==BwnIdle && frame_count==previous_tx);
    assert(fap.capture_deauth_supported);
    previous_tx=frame_count;
    assert(bw16_control_start_capture_deauth(&fap,1,0x3456cdefU,clock_now));pump(&fap);
    assert(fap.state==Bw16Active && fap.capture_with_deauth && !fap.capture_deauth_reported);
    assert(fw.state==BwnCapture && fw.capture_with_deauth && capture_armed && frame_count==previous_tx);
    tick(clock_now);pump(&fap);
    assert(fap.capture_deauth_reported && fap.capture_tx_attempts==3 && frame_count==previous_tx+3);
    tick(clock_now+10000);pump(&fap);assert(frame_count==previous_tx+3 && fap.state==Bw16Active);
    assert(bw16_control_stop(&fap,clock_now));pump(&fap);
    assert(fap.state==Bw16Stopped && !fap.remote_unknown && fw.state==BwnIdle);
    previous_tx=frame_count;
    assert(bw16_control_start_capture_deauth(&fap,1,0x4567def0U,clock_now));pump(&fap);
    assert(bw16_control_stop(&fap,clock_now));pump(&fap);tick(clock_now);pump(&fap);
    assert(fap.state==Bw16Stopped && !fap.capture_deauth_reported && frame_count==previous_tx);
    assert(bw16_control_start_capture_deauth(&fap,1,0x5678ef01U,clock_now));pump(&fap);
    tick(clock_now+BWN_CAPTURE_DEAUTH_MS);pump(&fap);
    assert(fap.state==Bw16Active && fap.capture_deauth_reported && !fap.capture_tx_attempts);
    assert(frame_count==previous_tx);assert(bw16_control_stop(&fap,clock_now));pump(&fap);
    assert(bw16_control_start_capture(&fap,1,0x6789f012U,clock_now));pump(&fap);tick(clock_now);pump(&fap);
    assert(!fap.capture_with_deauth && !fap.capture_deauth_reported && !fap.capture_tx_attempts && frame_count==previous_tx);
    assert(bw16_control_stop(&fap,clock_now));pump(&fap);
    puts("native/FAP: capabilities, selected TX, passive capture and explicit one-shot capture wire contracts PASS");
}
int main(void) {
    capabilities_and_validation();selected_round_robin();deadline_and_faults();fifty_targets();
    legacy_and_clients();frame_parsing();passive_capture();capture_with_deauth();protocol_integration();
    puts("No serial port opened; all radio calls are host stubs.");return 0;
}
