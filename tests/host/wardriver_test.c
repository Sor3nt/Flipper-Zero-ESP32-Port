#include "wardriver_nmea.h"
#include "wardriver_detect.h"
#include "wardriver_monitor.h"
#include "wardriver_types.h"
#include "wardriver_csv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
static unsigned checks;
#define CHECK(condition) do { ++checks; if(!(condition)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#condition); exit(1); } } while(0)
static size_t nmea(char* out,const char* body) {
    unsigned sum=0; for(const char* p=body;*p;++p) sum^=(unsigned char)*p;
    return (size_t)sprintf(out,"$%s*%02X\r\n",body,sum);
}
static void feed(WardriverNmea* p,const char* body,uint32_t now) {
    char line[256]; size_t n=nmea(line,body); wardriver_nmea_feed(p,line,n,now);
}
static void gps_tests(void) {
    WardriverNmea p; wardriver_nmea_init(&p); char line[256],text[20];
    CHECK(!wardriver_nmea_snapshot(&p,0).connected);
    feed(&p,"GNGGA,,,,,,0,00,99.99,,,,,,",10);
    CHECK(p.data.connected && !p.data.has_fix && !p.data.timestamp_valid);
    wardriver_nmea_init(&p);
    size_t n=nmea(line,"GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W");
    for(size_t i=0;i<n;++i) wardriver_nmea_feed(&p,line+i,1,100);
    CHECK(p.data.has_fix && p.data.timestamp_valid);
    CHECK(fabs(p.data.latitude-48.1173)<1e-7); CHECK(fabs(p.data.longitude-11.5166666667)<1e-7);
    wardriver_format_time(p.data.timestamp,text); CHECK(!strcmp(text,"1994-03-23 12:35:19"));
    feed(&p,"GNGGA,123519,4807.038,N,01131.000,E,1,09,0.9,545.4,M,46.9,M,,",110);
    CHECK(p.data.has_fix && p.data.satellites==9 && p.data.hdop_valid && p.data.altitude_valid);
    CHECK(fabs(p.data.altitude-545.4)<1e-7);
    n=nmea(line,"GPRMC,123520,A,4807.038,N,01131.000,E,0,0,230926,,,A");
    line[n-4]=line[n-4]=='0'?'1':'0'; wardriver_nmea_feed(&p,line,n,120);
    CHECK(p.last_sentence==110);
    feed(&p,"GNRMC,123520,V,,,,,,,230926,,,N",130);
    CHECK(p.data.connected && !p.data.has_fix);
    CHECK(!wardriver_nmea_snapshot(&p,3131).connected);
    CHECK(!wardriver_nmea_snapshot(&p,3131).timestamp_valid);
    wardriver_nmea_init(&p);
    feed(&p,"GNRMC,000000,A,0000.000,N,00000.000,E,0,0,230926,,,A",0);
    CHECK(p.data.has_fix); /* Regression: absent GGA must not negate midnight fix. */
    feed(&p,"GNGGA,000000,,,,,0,00,99.9,,M,,M,,",1);
    CHECK(!p.data.has_fix); /* Same-epoch contradiction. */
    feed(&p,"GNRMC,000001,A,9000.000,S,18000.000,W,0,0,230926,,,A",1000);
    CHECK(p.data.has_fix && p.data.latitude==-90 && p.data.longitude==-180);
    feed(&p,"GNRMC,000002,A,9060.000,N,18100.000,E,0,0,230926,,,A",1200);
    CHECK(p.last_sentence==1000);
    feed(&p,"GNRMC,0000030,A,0000.000,N,00000.000,E,0,0,230926,,,A",1201);
    CHECK(p.last_sentence==1000);
    feed(&p,"GNRMC,000003,A,123.4,N,00000.000,E,0,0,230926,,,A",1202);
    CHECK(p.last_sentence==1000);
    feed(&p,"GNRMC,235959,A,4807.038,N,01131.000,E,0,0,311226,,,A",2000);
    feed(&p,"GNGGA,000000,4807.038,N,01131.000,E,1,12,1.1,12.3,M,0,M,,",2500);
    wardriver_format_time(p.data.timestamp,text); CHECK(!strcmp(text,"2027-01-01 00:00:00"));
    feed(&p,"GNRMC,000001,A,4807.038,N,01131.000,E,0,0,310226,,,A",3000);
    CHECK(!p.data.timestamp_valid);
    char many[600]; memset(many,'A',sizeof(many)); many[0]='$'; many[599]='\n';
    wardriver_nmea_feed(&p,many,sizeof(many),3100); CHECK(p.last_sentence==3000);
    char batch[512]; n=nmea(batch,"GNRMC,010203,A,4807.038,N,01131.000,E,0,0,230926,,,A");
    n+=nmea(batch+n,"GNGGA,010203,4807.038,N,01131.000,E,1,10,0.8,5,M,0,M,,");
    wardriver_nmea_feed(&p,batch,n,3200); CHECK(p.data.has_fix && p.data.satellites==10);
    const char* bare="$GPRMC,010204,A,4807.038,N,01131.000,E,0,0,230926,,,A\n";
    wardriver_nmea_feed(&p,bare,strlen(bare),UINT32_MAX-1000);
    CHECK(wardriver_nmea_snapshot(&p,500).connected);
    CHECK(!wardriver_nmea_snapshot(&p,3000).connected);
}
static void detection_tests(void) {
    WardriverObservation o={0}; uint8_t id[25]={2,0x10}; memcpy(id+2,"TESTDRONE123",12);
    CHECK(wardriver_detect_remote_id(id,25,&o)); CHECK(!strcmp(o.remote_id,"TESTDRONE123"));
    CHECK(o.hints&WardriverHintRemoteId); CHECK(!wardriver_detect_remote_id(id,24,&o));
    id[0]=3; CHECK(!wardriver_detect_remote_id(id,25,&o)); id[0]=2;
    uint8_t pack[28]={0xF2,25,1}; memcpy(pack+3,id,25);
    CHECK(wardriver_detect_remote_id(pack,sizeof(pack),&o)); pack[2]=2;
    CHECK(!wardriver_detect_remote_id(pack,sizeof(pack),&o));
    uint8_t advertisement[31]={30,0x16,0xFA,0xFF,0x0D,1}; memcpy(advertisement+6,id,25);
    o=(WardriverObservation){0}; CHECK(wardriver_detect_ble(advertisement,sizeof(advertisement),&o));
    CHECK(o.hints&WardriverHintRemoteId);
    o=(WardriverObservation){0}; uint8_t supplier[]={3,0xFF,0xC8,0x09};
    CHECK(wardriver_detect_ble(supplier,sizeof(supplier),&o)); CHECK(o.hints&WardriverHintXuntong);
    CHECK(!(o.hints&WardriverHintFlock));
    o=(WardriverObservation){0}; strcpy(o.ssid,"penguin-1234567890");
    CHECK(wardriver_detect_ble(NULL,0,&o)); CHECK(o.hints&WardriverHintFlock);
    WardriverObservation before=o; uint8_t bad[]={10,0x09,'a'};
    CHECK(!wardriver_detect_ble(bad,sizeof(bad),&o)); CHECK(!memcmp(&before,&o,sizeof(o)));
    CHECK(!wardriver_detect_ble(NULL,10,&o));
    uint8_t beacon[69]={0x80}; memset(beacon+16,0xAB,6); beacon[36]=221; beacon[37]=30;
    uint8_t header[]={0xFA,0x0B,0xBC,0x0D,1}; memcpy(beacon+38,header,5); memcpy(beacon+43,id,25);
    o=(WardriverObservation){0}; CHECK(wardriver_detect_beacon(beacon,68,&o)); CHECK(o.mac[0]==0xAB);
    CHECK(!wardriver_detect_beacon(beacon,69,&o)); CHECK(!wardriver_detect_beacon(beacon,67,&o));
}
static WardriverObservation ble(const uint8_t* data,size_t size) {
    WardriverObservation o={.mode=WardriverBle,.random_address=true};
    CHECK(wardriver_detect_ble(data,size,&o)); return o;
}
static void classifier_tests(void) {
    uint8_t tag[31]={30,0xFF,0x4C,0,0x12,0x19,0x10};
    for(unsigned battery=0;battery<4;++battery) {
        tag[6]=(uint8_t)(0x10|(battery<<6));
        WardriverObservation o=ble(tag,sizeof(tag));
        CHECK(o.hints&WardriverHintAirTag); CHECK(o.confidence==WardConfidenceHigh);
        CHECK(!strcmp(wardriver_device_label(o.primary_hint),"AirTag"));
    }
    uint8_t nearby[]={7,0xFF,0x4C,0,0x12,2,0x10,0};
    CHECK(ble(nearby,sizeof(nearby)).hints&WardriverHintAirTag);
    nearby[6]=0x20; CHECK(!(ble(nearby,sizeof(nearby)).hints&WardriverHintAirTag));
    CHECK(ble(nearby,sizeof(nearby)).hints&WardriverHintTracker);
    nearby[6]=0x30; CHECK(ble(nearby,sizeof(nearby)).hints&WardriverHintAirPods);
    nearby[6]=0; CHECK(!(ble(nearby,sizeof(nearby)).hints&WARDRIVER_NOTABLE_HINTS));
    tag[4]=7; CHECK(!(ble(tag,sizeof(tag)).hints&WardriverHintAirTag)); /* Pairing is not uniquely AirTag. */
    tag[4]=0x12; tag[5]=0x19; tag[0]=7;
    CHECK(!(ble(tag,8).hints&WardriverHintAirTag)); /* Truncated inner payload. */
    uint8_t fake[]={9,0x09,'X',0x1E,0xFF,0x4C,0,0x12,0x19,'X'};
    CHECK(!(ble(fake,sizeof(fake)).hints&WardriverHintAirTag));
    const struct { uint16_t uuid; uint32_t flag; } cases[]={
        {0xFD5A,WardriverHintSmartTag},{0xFEED,WardriverHintTile},{0xFEEC,WardriverHintTile},
        {0xFD84,WardriverHintChipolo},{0xFD44,WardriverHintFindMy},{0xFD5F,WardriverHintGlasses},
        {0x3081,WardriverHintFlipper},{0x3082,WardriverHintFlipper},{0x3083,WardriverHintFlipper},
        {0x3100,WardriverHintRaven},{0x3200,WardriverHintRaven},{0x3300,WardriverHintRaven},
        {0x3400,WardriverHintRaven},{0x3500,WardriverHintRaven},{0x1101,WardriverHintSkimmer}};
    for(unsigned i=0;i<sizeof(cases)/sizeof(*cases);++i) {
        uint8_t a[]={3,3,(uint8_t)cases[i].uuid,(uint8_t)(cases[i].uuid>>8)};
        CHECK(ble(a,sizeof(a)).hints&cases[i].flag);
    }
    uint8_t company[]={3,0xFF,0xAB,1}; CHECK(ble(company,4).hints&WardriverHintMeta);
    CHECK(ble(company,4).confidence==WardConfidenceLow);
    company[2]=0x29; company[3]=0x0E; CHECK(ble(company,4).hints&WardriverHintFlipper);
    company[2]=0xBA; company[3]=0x0F; CHECK(!ble(company,4).hints);
    uint8_t google[26]={25,0x16,0xAA,0xFE,0x40};
    CHECK(ble(google,26).hints&WardriverHintGoogle);
    google[4]=0x41; CHECK(ble(google,26).hints&WardriverHintGoogle);
    google[0]=3; CHECK(!ble(google,4).hints);
    google[0]=23; google[4]=0; CHECK(ble(google,24).hints&WardriverHintEddystone);
    CHECK(!(ble(google,24).hints&WardriverHintTracker));
    uint8_t ib[27]={26,0xFF,0x4C,0,2,21}; CHECK(ble(ib,27).hints&WardriverHintIBeacon);
    CHECK(!(ble(ib,27).hints&WardriverHintAirTag));
    ib[0]=25; CHECK(!(ble(ib,26).hints&WardriverHintIBeacon));
    const struct { uint8_t mac[3]; uint32_t flag; } vendors[]={
        {{0xB4,0x1E,0x52},WardriverHintFlock},{{0,0x25,0xDF},WardriverHintAxon},
        {{0xAC,0x9F,0xC3},WardriverHintRing},{{0xE0,0xA7,0},WardriverHintCamera},
        {{0,0xBF,0x15},WardriverHintAlpr}};
    for(unsigned i=0;i<sizeof(vendors)/sizeof(*vendors);++i) {
        WardriverObservation o={.mode=WardriverWifi}; memcpy(o.mac,vendors[i].mac,3);
        wardriver_detect_wifi(&o); CHECK(o.hints&vendors[i].flag); CHECK(o.evidence==WardEvidenceOui);
        o=(WardriverObservation){.mode=WardriverBle,.random_address=true}; memcpy(o.mac,vendors[i].mac,3);
        CHECK(wardriver_detect_ble(NULL,0,&o)); CHECK(!o.hints);
    }
    WardriverObservation o={.mac={0x24,0x0A,0xC4},.mode=WardriverWifi};
    wardriver_detect_wifi(&o); CHECK(!o.hints); /* Ordinary ESP32 is not Flock. */
    o=(WardriverObservation){.mac={0,0x0E,0x58},.mode=WardriverWifi};
    wardriver_detect_wifi(&o); CHECK(!o.hints); /* Sonos is not ALPR. */
    strcpy(o.ssid,"My AirTag WiFi"); wardriver_detect_wifi(&o); CHECK(!o.hints);
    strcpy(o.ssid,"Pineapple_1234"); wardriver_detect_wifi(&o); CHECK(o.hints&WardriverHintPineapple);
    o=(WardriverObservation){.mode=WardriverWifi}; strcpy(o.ssid,"AB4-12345"); wardriver_detect_wifi(&o); CHECK(o.hints&WardriverHintAxon);
    o=ble(nearby,sizeof(nearby)); CHECK(!wardriver_filter_match(&o,0,WardConfidenceNone));
    o=ble(ib,sizeof(ib)-1); CHECK(!wardriver_filter_match(&o,5,WardConfidenceNone));
    ib[0]=26; o=ble(ib,sizeof(ib)); CHECK(wardriver_filter_match(&o,5,WardConfidenceHigh));
    CHECK(!wardriver_filter_match(&o,0,WardConfidenceNone));
}
static void put32(uint8_t* p,int32_t value) {
    for(unsigned i=0;i<4;++i) p[i]=(uint8_t)((uint32_t)value>>(i*8));
}
static void rid_tests(void) {
    uint8_t p[25]={0x12,0x20}; put32(p+5,481173000); put32(p+9,115167000); p[15]=0xD0; p[16]=7;
    WardriverObservation o={0}; CHECK(wardriver_detect_remote_id(p,25,&o));
    CHECK((o.rid.fields&(WardRidPosition|WardRidAltitude))==(WardRidPosition|WardRidAltitude));
    CHECK(o.rid.latitude==481173000 && o.rid.altitude==2000);
    WardriverNetwork entries[4]={0}; WardriverTable t={.entries=entries,.capacity=4}; WardriverGpsData gps={0}; bool fresh;
    o.seen_ms=100; WardriverNetwork* n=wardriver_table_observe(&t,&o,1,&gps,&fresh);
    p[0]=0x42; p[1]=1; put32(p+2,480000000); put32(p+6,110000000);
    o=(WardriverObservation){.seen_ms=200}; CHECK(wardriver_detect_remote_id(p,25,&o));
    CHECK(wardriver_table_observe(&t,&o,2,&gps,&fresh)==n);
    CHECK((n->observation.rid.fields&(WardRidPosition|WardRidOperator))==(WardRidPosition|WardRidOperator));
    CHECK(n->rid_location_ms==100 && n->rid_operator_ms==200);
    memset(p,0,sizeof(p)); p[0]=0x12;
    o=(WardriverObservation){.seen_ms=300}; CHECK(wardriver_detect_remote_id(p,25,&o));
    wardriver_table_observe(&t,&o,3,&gps,&fresh);
    CHECK(!(n->observation.rid.fields&(WardRidPosition|WardRidAltitude))); /* Unknown clears old fix. */
    CHECK(n->observation.rid.fields&WardRidOperator);
    put32(p+5,910000000); o=(WardriverObservation){0}; CHECK(!wardriver_detect_remote_id(p,25,&o)); CHECK(!o.hints);
}
static size_t make_beacon(uint8_t* f,uint8_t mac,bool encrypted,const char* ssid) {
    memset(f,0,128); f[0]=0x80; f[16]=0xAC; f[21]=mac; f[34]=encrypted?0x10:0;
    size_t n=strlen(ssid); f[37]=(uint8_t)n; memcpy(f+38,ssid,n); return 38+n;
}
static void monitor_tests(void) {
    WardriverMonitor m={0}; WardriverObservation o={.auth=255}; uint8_t f[128];
    size_t n=make_beacon(f,1,true,"Office"); CHECK(!wardriver_monitor_frame(&m,f,n,1,&o));
    n=make_beacon(f,2,true,"Office"); CHECK(!wardriver_monitor_frame(&m,f,n,2,&o)); /* Mesh */
    n=make_beacon(f,3,false,"Other"); CHECK(!wardriver_monitor_frame(&m,f,n,3,&o));
    n=make_beacon(f,3,false,"Office"); CHECK(wardriver_monitor_frame(&m,f,n,4,&o));
    CHECK(o.hints&WardriverHintEvilTwin); CHECK(!wardriver_monitor_frame(&m,f,n,5,&o));
    memset(&m,0,sizeof(m)); CHECK(!wardriver_monitor_frame(&m,f,n,6,&o)); /* New session clears APs. */
    memset(f,0,sizeof(f)); f[0]=0xC0; f[10]=0xAC; f[15]=1;
    for(unsigned i=0;i<5;++i) { f[22]=(uint8_t)(i<<4); CHECK(!wardriver_monitor_frame(&m,f,26,100+i,&o)); }
    f[22]=0x50; CHECK(wardriver_monitor_frame(&m,f,26,105,&o)); CHECK(o.hints&WardriverHintDeauth);
    CHECK(!wardriver_monitor_frame(&m,f,26,106,&o));
    memset(&m,0,sizeof(m));
    for(unsigned i=0;i<12;++i) { f[1]=8; CHECK(!wardriver_monitor_frame(&m,f,26,100+i,&o)); } /* Retry duplicates. */
    memset(&m,0,sizeof(m)); f[1]=0;
    for(unsigned i=0;i<6;++i) { f[15]=(uint8_t)i; CHECK(!wardriver_monitor_frame(&m,f,26,200+i,&o)); } /* Independent senders. */
    memset(&m,0,sizeof(m));
    for(unsigned i=0;i<6;++i) { f[22]=(uint8_t)(i<<4); bool hit=wardriver_monitor_frame(&m,f,26,UINT32_MAX-2+i,&o); CHECK(hit==(i==5)); }
    n=make_beacon(f,1,true,"test"); f[n++]=222; const char* json="{\"name\":\"pet\",\"pwnd_tot\":1}";
    f[n++]=(uint8_t)strlen(json); memcpy(f+n,json,strlen(json)); n+=strlen(json);
    o=(WardriverObservation){0}; CHECK(wardriver_detect_beacon(f,n,&o)); CHECK(o.hints&WardriverHintPwnagotchi);
}
static void repeated_detection_test(void) {
    WardriverNetwork entries[8]; WardriverGpsData gps={0}; bool fresh;
    const uint8_t long_tag[31]={30,0xFF,0x4C,0,0x12,0x19,0x10};
    const uint8_t short_tag[]={7,0xFF,0x4C,0,0x12,2,0x10,0};
    for(unsigned session=0;session<8;++session) {
        memset(entries,0,sizeof(entries)); WardriverTable t={.entries=entries,.capacity=8};
        for(unsigned i=0;i<5;++i) {
            WardriverObservation o=ble(session%2?short_tag:long_tag,session%2?sizeof(short_tag):sizeof(long_tag));
            memcpy(o.mac,"\xCC\x01\x02\x03\x04\x05",6);
            WardriverNetwork* n=wardriver_table_observe(&t,&o,100+i,&gps,&fresh);
            CHECK(n && fresh==(i==0) && t.notable_count==1 && t.count==1);
            o.hints=o.primary_hint=0; o.confidence=WardConfidenceNone; o.evidence=WardEvidenceNone;
            wardriver_table_observe(&t,&o,200+i,&gps,&fresh);
            CHECK(n->observation.hints&WardriverHintAirTag); CHECK(t.notable_count==1);
        }
        WardriverObservation rotated=ble(short_tag,sizeof(short_tag)); rotated.mac[0]=0xDD;
        CHECK(wardriver_table_observe(&t,&rotated,300,&gps,&fresh) && fresh && t.notable_count==2);
    }
}
static void table_csv_tests(void) {
    WardriverNetwork entries[2]={0}; WardriverTable t={.entries=entries,.capacity=2}; bool fresh;
    WardriverGpsData gps={0}; WardriverObservation o={.mode=WardriverWifi,.mac={0,1,2,3,4,5},.rssi=-70};
    strcpy(o.ssid,"One"); WardriverNetwork* n=wardriver_table_observe(&t,&o,100,&gps,&fresh);
    CHECK(n && fresh && t.count==1 && n->detections==1); o.rssi=-42; o.ssid[0]=0;
    WardriverNetwork* again=wardriver_table_observe(&t,&o,200,&gps,&fresh);
    CHECK(n==again && !fresh && n->detections==2 && n->observation.rssi==-42);
    CHECK(n->first_seen==100 && n->last_seen==200 && !strcmp(n->observation.ssid,"One"));
    o.mac[5]=6; CHECK(wardriver_table_observe(&t,&o,300,&gps,&fresh) && fresh);
    o.mac[5]=7; CHECK(!wardriver_table_observe(&t,&o,400,&gps,&fresh)); CHECK(t.overflow==1 && t.count==2);
    o.mac[5]=5; CHECK(wardriver_table_observe(&t,&o,500,&gps,&fresh)==n); CHECK(n->detections==3 && t.detections==5);
    gps.has_fix=true; gps.latitude=48.1; gps.timestamp=600;
    wardriver_table_observe(&t,&o,600,&gps,&fresh);
    gps.has_fix=false;
    wardriver_table_observe(&t,&o,700,&gps,&fresh);
    CHECK(!n->gps.has_fix && n->last_fix.has_fix && n->last_fix.latitude==48.1 && n->last_fix.timestamp==600);
    memset(entries,0,sizeof(entries)); t=(WardriverTable){.entries=entries,.capacity=2};
    o.mode=WardriverBle; o.random_address=false;
    CHECK(wardriver_table_observe(&t,&o,800,&gps,&fresh) && fresh);
    o.random_address=true;
    CHECK(wardriver_table_observe(&t,&o,800,&gps,&fresh) && fresh && t.count==2);
    char out[80]; CHECK(wardriver_csv_quote(out,sizeof(out),"a,\"b\"\n")>0);
    CHECK(!strcmp(out,"\"a,\"\"b\"\"\n\"")); CHECK(wardriver_csv_quote(out,4,"long")==0);
    CHECK(!strcmp(wardriver_security(3),"[WPA2-PSK][ESS]"));
}
static void fuzz_inputs(void) {
    uint32_t state=0x12345678; uint8_t bytes[512]; WardriverNmea p; wardriver_nmea_init(&p);
    for(unsigned iteration=0;iteration<50000;++iteration) {
        state=state*1664525U+1013904223U; size_t n=state%sizeof(bytes);
        for(size_t i=0;i<n;++i) { state=state*1664525U+1013904223U; bytes[i]=(uint8_t)(state>>24); }
        WardriverObservation o={0}; wardriver_detect_ble(bytes,n,&o);
        wardriver_detect_beacon(bytes,n,&o); wardriver_detect_remote_id(bytes,n,&o);
        wardriver_nmea_feed(&p,bytes,n,iteration); wardriver_nmea_snapshot(&p,iteration);
    }
}
static void combined_counts_test(void) {
    WardriverNetwork entries[8]={0}; WardriverTable t={.entries=entries,.capacity=8};
    WardriverGpsData gps={0}; bool fresh;
    WardriverObservation o={.mode=WardriverWifi,.mac={0,1,2,3,4,5}};
    wardriver_table_observe(&t,&o,1,&gps,&fresh);
    CHECK(t.wifi_count==1 && t.ble_count==0 && t.notable_count==0);
    o.mode=WardriverBle; wardriver_table_observe(&t,&o,2,&gps,&fresh);
    CHECK(fresh && t.count==2 && t.wifi_count==1 && t.ble_count==1);
    o.hints=WardriverHintXuntong|WardriverHintSerial;
    wardriver_table_observe(&t,&o,3,&gps,&fresh); CHECK(!fresh && !t.notable_count);
    o.hints=WardriverHintFlock; wardriver_table_observe(&t,&o,4,&gps,&fresh);
    CHECK(!fresh && t.notable_count==1);
    o.hints|=WardriverHintRemoteId;
    for(unsigned i=0;i<50;i++) wardriver_table_observe(&t,&o,5+i,&gps,&fresh);
    CHECK(t.notable_count==1 && t.ble_count==1 && t.count==2);
    o.mac[5]=6; o.hints=WardriverHintRemoteId;
    wardriver_table_observe(&t,&o,60,&gps,&fresh);
    CHECK(fresh && t.notable_count==2 && t.ble_count==2 && t.count==3);
}
int main(void) {
    gps_tests(); detection_tests(); classifier_tests(); rid_tests(); monitor_tests(); repeated_detection_test(); table_csv_tests(); combined_counts_test(); fuzz_inputs();
    printf("PASS: %u checks; 50,000 malformed-input iterations\n",checks); return 0;
}
