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
        if(!*is_new) e->previous_rssi=e->observation.rssi;
        else e->previous_rssi=o->rssi;
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
        if(o->primary_hint && (!e->observation.primary_hint || o->confidence>e->observation.confidence)) {
            e->observation.primary_hint=o->primary_hint;
            e->observation.confidence=o->confidence; e->observation.evidence=o->evidence;
        }
        if(o->rid.fields&WardRidBasic) { e->observation.rid.aircraft=o->rid.aircraft; e->observation.rid.fields|=WardRidBasic; }
        if(o->rid.fields&WardRidLocation) {
            e->observation.rid.fields=(e->observation.rid.fields&~(WardRidLocation|WardRidPosition|WardRidAltitude))|
                (o->rid.fields&(WardRidLocation|WardRidPosition|WardRidAltitude));
            e->observation.rid.latitude=o->rid.latitude; e->observation.rid.longitude=o->rid.longitude;
            e->observation.rid.altitude=o->rid.altitude; e->rid_location_ms=o->seen_ms;
        }
        if(o->rid.fields&WardRidSystem) {
            e->observation.rid.fields=(e->observation.rid.fields&~(WardRidSystem|WardRidOperator))|
                (o->rid.fields&(WardRidSystem|WardRidOperator));
            e->observation.rid.operator_latitude=o->rid.operator_latitude;
            e->observation.rid.operator_longitude=o->rid.operator_longitude;
            e->observation.rid.operator_type=o->rid.operator_type; e->rid_operator_ms=o->seen_ms;
        }
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
const char* wardriver_device_label(uint32_t f) {
    if(f&WardriverHintDeauth) return "Deauth burst";
    if(f&WardriverHintEvilTwin) return "Evil twin?";
    if(f&WardriverHintRemoteId) return "Drone / RID";
    if(f&WardriverHintAirTag) return "AirTag";
    if(f&WardriverHintSmartTag) return "SmartTag";
    if(f&WardriverHintGoogle) return "Google tag";
    if(f&WardriverHintTile) return "Tile";
    if(f&WardriverHintChipolo) return "Chipolo";
    if(f&WardriverHintAirPods) return "AirPods";
    if(f&WardriverHintFindMy) return "Find My";
    if(f&WardriverHintFlock) return "Flock?";
    if(f&WardriverHintAxon) return "Axon?";
    if(f&WardriverHintGlasses) return "Meta glasses";
    if(f&WardriverHintMeta) return "Glasses?";
    if(f&WardriverHintRaven) return "Raven?";
    if(f&WardriverHintAlpr) return "ALPR?";
    if(f&WardriverHintRing) return "Ring?";
    if(f&WardriverHintCamera) return "Camera?";
    if(f&WardriverHintFlipper) return "Flipper";
    if(f&WardriverHintPwnagotchi) return "Pwnagotchi";
    if(f&WardriverHintPineapple) return "Pineapple?";
    if(f&WardriverHintDeauther) return "Deauther?";
    if(f&WardriverHintSkimmer) return "Skimmer?";
    if(f&WardriverHintIBeacon) return "iBeacon";
    if(f&WardriverHintEddystone) return "Eddystone";
    if(f&WardriverHintTracker) return "Tracker?";
    return NULL;
}
const char* wardriver_hint_text(uint32_t f) {
    const char* label=wardriver_device_label(f);
    if(label) return label;
    if(f&WardriverHintSerial) return "Generic serial module";
    if(f&WardriverHintXuntong) return "XUNTONG vendor only";
    return "No signature match";
}
const char* wardriver_confidence_text(WardriverConfidence c) {
    return c==WardConfidenceHigh?"HIGH":c==WardConfidenceMedium?"MEDIUM":c==WardConfidenceLow?"LOW":"NONE";
}
const char* wardriver_evidence_text(WardriverEvidence e) {
    switch(e) {
    case WardEvidenceName: return "Advertised name";
    case WardEvidenceService: return "BLE service UUID";
    case WardEvidenceCompany: return "BLE company ID";
    case WardEvidenceOui: return "Vendor prefix only";
    case WardEvidencePayload: return "Broadcast payload";
    case WardEvidenceBehavior: return "Traffic pattern";
    default: return "No evidence";
    }
}

const char* wardriver_filter_name(unsigned filter) {
    static const char* names[]={"All notable","Trackers","Cameras","Radio tools","Wi-Fi activity","Beacons"};
    return names[filter<6?filter:0];
}
bool wardriver_filter_match(const WardriverObservation* o,unsigned filter,WardriverConfidence minimum) {
    static const uint32_t masks[]={WARDRIVER_NOTABLE_HINTS,WardriverHintTracker,
        WardriverHintFlock|WardriverHintAxon|WardriverHintCamera|WardriverHintRing|WardriverHintAlpr|WardriverHintGlasses|WardriverHintMeta|WardriverHintRaven,
        WardriverHintFlipper|WardriverHintPwnagotchi|WardriverHintPineapple|WardriverHintDeauther|WardriverHintSkimmer,
        WardriverHintDeauth|WardriverHintEvilTwin,
        WardriverHintIBeacon|WardriverHintEddystone};
    return (o->hints&masks[filter<6?filter:0]) && o->confidence>=minimum;
}
