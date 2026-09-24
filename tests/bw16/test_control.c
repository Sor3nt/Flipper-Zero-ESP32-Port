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
int main(void) {
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
    puts("control: indexed lists, confirmed START/STOP, bounds, timeout/wrap, RX/TX faults and no implicit operation PASS");
}
