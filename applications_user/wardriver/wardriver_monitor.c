#include "wardriver_monitor.h"
#include <string.h>

static bool deauth(WardriverMonitor* m,const uint8_t* f,size_t n,uint32_t now,WardriverObservation* o) {
    if(n<26 || (f[10]&1)) return false;
    unsigned slot=0; uint32_t oldest=0;
    for(unsigned i=0;i<WARD_BURST_WATCH;++i) {
        WardriverBurstWatch* b=&m->bursts[i];
        if(b->used && !memcmp(b->mac,f+10,6)) { slot=i; break; }
        if(!b->used) { slot=i; break; }
        uint32_t age=now-b->seen; if(age>=oldest) { oldest=age; slot=i; }
    }
    WardriverBurstWatch* b=&m->bursts[slot];
    bool same=b->used && !memcmp(b->mac,f+10,6);
    uint16_t sequence=(uint16_t)f[22]|((uint16_t)f[23]<<8);
    if(same && (f[1]&8) && b->sequence==sequence) return false;
    if(!same) { memset(b,0,sizeof(*b)); b->used=true; memcpy(b->mac,f+10,6); b->window=now; }
    if(now-b->window>3000) { b->window=now; b->count=0; }
    b->seen=now; b->sequence=sequence; if(b->count<6) ++b->count;
    if(b->count<6 || (b->alerted && now-b->last_alert<15000)) return false;
    b->alerted=true; b->last_alert=now;
    memcpy(o->mac,f+10,6);
    wardriver_match(o,WardriverHintDeauth,WardConfidenceMedium,WardEvidenceBehavior);
    return true;
}
static bool beacon(WardriverMonitor* m,const uint8_t* f,size_t n,uint32_t now,WardriverObservation* out) {
    if(n<36 || (f[16]&1)) return false;
    char ssid[33]={0}; bool have_ssid=false;
    size_t pos;
    for(pos=36;pos+2<=n;) {
        unsigned type=f[pos++],size=f[pos++]; if(size>n-pos) return false;
        if(type==0) {
            if(size>32 || have_ssid) return false;
            memcpy(ssid,f+pos,size); ssid[size]=0; have_ssid=true;
            for(unsigned i=0;i<size;++i) if((unsigned char)ssid[i]<32 || (unsigned char)ssid[i]>126) return false;
        }
        pos+=size;
    }
    if(pos!=n || !ssid[0]) return false;
    /* Self-declared Pwnagotchi SSIDs are not infrastructure AP identities. */
    WardriverObservation detected=*out;
    bool matched=wardriver_detect_beacon(f,n,&detected);
    if(matched && (detected.hints&WardriverHintPwnagotchi)) return false;
    bool encrypted=(f[34]&0x10)!=0,conflict=false;
    unsigned slot=0; uint32_t oldest=0; bool existing=false,empty=false;
    for(unsigned i=0;i<WARD_AP_WATCH;++i) {
        WardriverApWatch* a=&m->aps[i];
        if(a->used && !memcmp(a->mac,f+16,6)) { slot=i; existing=true; }
        else {
            if(a->used && now-a->seen<=30000 && !strcmp(a->ssid,ssid) && a->encrypted!=encrypted) conflict=true;
            if(!existing) {
                if(!a->used && !empty) { slot=i; empty=true; }
                else if(!empty && now-a->seen>=oldest) { oldest=now-a->seen; slot=i; }
            }
        }
    }
    WardriverApWatch* a=&m->aps[slot];
    if(!existing || strcmp(a->ssid,ssid)) { memset(a,0,sizeof(*a)); a->used=true; memcpy(a->mac,f+16,6); strcpy(a->ssid,ssid); }
    a->seen=now; a->encrypted=encrypted;
    if(!conflict || (a->alerted && now-a->last_alert<15000)) return false;
    a->alerted=true; a->last_alert=now;
    memcpy(out->mac,f+16,6); strcpy(out->ssid,ssid);
    wardriver_match(out,WardriverHintEvilTwin,WardConfidenceMedium,WardEvidenceBehavior);
    return true;
}
bool wardriver_monitor_frame(WardriverMonitor* m,const uint8_t* f,size_t n,uint32_t now,WardriverObservation* out) {
    if(!m || !f || !out || n<24 || (f[0]&0x0F) || (f[1]&3)) return false;
    unsigned subtype=f[0]&0xF0;
    if(subtype==0xC0 || subtype==0xA0) return deauth(m,f,n,now,out);
    if(subtype==0x80) return beacon(m,f,n,now,out);
    return false;
}
