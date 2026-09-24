#include "wardriver_types.h"
#include <stdio.h>
#include <string.h>

void wardriver_mac_text(const uint8_t mac[6],char out[18]) {
    snprintf(out,18,"%02X:%02X:%02X:%02X:%02X:%02X",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
}
WardriverNetwork* wardriver_table_observe(WardriverTable* t,const WardriverObservation* o,
                                        uint64_t timestamp,const WardriverGpsData* gps,bool* is_new) {
    *is_new=false;
    if(!t->entries || !t->capacity) return NULL;
    if(t->detections != UINT64_MAX) ++t->detections;
    uint32_t hash=2166136261U;
    for(unsigned i=0;i<6;++i) hash=(hash^o->mac[i])*16777619U;
    hash=(hash^(uint32_t)o->mode)*16777619U;
    hash=(hash^(uint32_t)o->random_address)*16777619U;
    size_t index=hash%t->capacity;
    for(size_t n=0;n<t->capacity;++n,index=(index+1)%t->capacity) {
        WardriverNetwork* e=&t->entries[index];
        if(!e->used) {
            memset(e,0,sizeof(*e)); e->used=true; e->first_seen=timestamp;
            e->first_seen_ms=o->seen_ms;
            e->observation=*o; ++t->count; *is_new=true;
            if(o->mode==WardriverWifi) ++t->wifi_count; else ++t->ble_count;
            if(o->hints & WARDRIVER_NOTABLE_HINTS) ++t->notable_count;
        } else if(e->observation.mode!=o->mode || e->observation.random_address!=o->random_address || memcmp(e->observation.mac,o->mac,6)) continue;
        e->observation.rssi=o->rssi;
        if(o->ssid[0]) memcpy(e->observation.ssid,o->ssid,sizeof(o->ssid));
        e->observation.ssid[32]=0;
        if(o->channel) e->observation.channel=o->channel;
        if(o->auth!=255) {
            e->observation.auth=o->auth; e->observation.pairwise=o->pairwise; e->observation.group=o->group;
        }
        if(!(e->observation.hints & WARDRIVER_NOTABLE_HINTS) && (o->hints & WARDRIVER_NOTABLE_HINTS))
            ++t->notable_count;
        e->observation.hints|=o->hints;
        if(o->remote_id[0]) memcpy(e->observation.remote_id,o->remote_id,sizeof(o->remote_id));
        if(e->detections!=UINT32_MAX) ++e->detections;
        e->last_seen=timestamp;
        e->last_seen_ms=o->seen_ms;
        e->gps=*gps;
        if(gps->has_fix) e->last_fix=*gps;
        e->dirty=true;
        return e;
    }
    if(t->overflow!=UINT32_MAX) ++t->overflow;
    return NULL;
}
const char* wardriver_hint_text(uint32_t f) {
    if(f&WardriverHintRemoteId) return "Remote ID broadcast";
    if(f&WardriverHintFlock) return "Possible Flock device";
    if(f&WardriverHintAxon) return "Possible body camera";
    if(f&WardriverHintTracker) return "Possible tracker";
    if(f&WardriverHintSerial) return "Generic serial module";
    if(f&WardriverHintXuntong) return "XUNTONG vendor only";
    return "No signature match";
}
