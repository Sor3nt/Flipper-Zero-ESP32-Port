#include "wardriver_detect.h"
#include <string.h>

/* Packet layouts and signature provenance are documented in DETECTIONS.md.
 * Never search arbitrary payload bytes for a signature outside an AD/IE field. */
static uint16_t le16(const uint8_t* p) { return p[0] | ((uint16_t)p[1]<<8); }
static int32_t le32(const uint8_t* p) {
    return (int32_t)((uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24));
}
static void text_copy(char* to,size_t size,const uint8_t* from,size_t length) {
    size_t n=length<size-1?length:size-1;
    for(size_t i=0;i<n;++i) to[i]=(from[i]>=32 && from[i]<127)?(char)from[i]:'.';
    to[n]=0;
}
void wardriver_match(WardriverObservation* o,uint32_t hint,WardriverConfidence confidence,WardriverEvidence evidence) {
    o->hints|=hint;
    if(!o->primary_hint || confidence>o->confidence) {
        o->primary_hint=hint; o->confidence=confidence; o->evidence=evidence;
    }
}
static bool position(int32_t lat,int32_t lon) {
    return lat>=-900000000 && lat<=900000000 && lon>=-1800000000 && lon<=1800000000 && (lat || lon);
}
static bool rid_message(const uint8_t* p,WardriverObservation* out) {
    if((p[0]&15)>2) return false;
    unsigned type=p[0]>>4;
    if(type==0) {
        unsigned id_type=p[1]>>4;
        if(id_type<1 || id_type>4) return false;
        if(id_type<=2) {
            if(p[2]<33 || p[2]>126) return false;
            size_t n=0;
            while(n<20 && p[2+n]) { if(p[2+n]<32 || p[2+n]>126) return false; ++n; }
            text_copy(out->remote_id,sizeof(out->remote_id),p+2,n);
        }
        out->rid.aircraft=p[1]&15; out->rid.fields|=WardRidBasic;
    } else if(type==1) {
        int32_t lat=le32(p+5),lon=le32(p+9);
        if(lat < -900000000 || lat > 900000000 || lon < -1800000000 || lon > 1800000000) return false;
        if((p[1]>>4)>4) return false;
        out->rid.fields&=~(WardRidPosition|WardRidAltitude);
        out->rid.fields|=WardRidLocation;
        out->rid.latitude=lat; out->rid.longitude=lon; out->rid.altitude=le16(p+15);
        if(position(lat,lon)) out->rid.fields|=WardRidPosition;
        if(out->rid.altitude) out->rid.fields|=WardRidAltitude;
    } else if(type==4) {
        int32_t lat=le32(p+2),lon=le32(p+6);
        if(lat < -900000000 || lat > 900000000 || lon < -1800000000 || lon > 1800000000 || (p[1]&3)>2) return false;
        out->rid.fields&=~WardRidOperator; out->rid.fields|=WardRidSystem;
        out->rid.operator_type=p[1]&3;
        out->rid.operator_latitude=lat; out->rid.operator_longitude=lon;
        if(position(lat,lon)) out->rid.fields|=WardRidOperator;
    } else return false;
    wardriver_match(out,WardriverHintRemoteId,WardConfidenceHigh,WardEvidencePayload);
    return true;
}
bool wardriver_detect_remote_id(const uint8_t* p,size_t length,WardriverObservation* out) {
    if(!p || !out || length<25) return false;
    WardriverObservation copy=*out;
    bool found=false;
    if((p[0]>>4)!=15) found=length==25 && rid_message(p,&copy);
    else {
        if((p[0]&15)>2 || p[1]!=25 || p[2]<1 || p[2]>9 || length!=3U+25U*p[2]) return false;
        for(unsigned i=0;i<p[2];++i) found=rid_message(p+3+25*i,&copy)||found;
    }
    if(found) *out=copy;
    return found;
}
static void names(WardriverObservation* o) {
    char lower[33];
    for(unsigned i=0;i<32;++i) { char c=o->ssid[i]; lower[i]=c>='A'&&c<='Z'?c+('a'-'A'):c; }
    lower[32]=0;
    bool penguin=!strncmp(lower,"penguin-",8)&&strlen(lower)==18;
    for(unsigned i=8;penguin&&i<18;++i) penguin=lower[i]>='0'&&lower[i]<='9';
    if(penguin || !strncmp(lower,"flock",5) || !strncmp(lower,"pigvision",9) || !strcmp(lower,"fs ext battery"))
        wardriver_match(o,WardriverHintFlock,WardConfidenceMedium,WardEvidenceName);
    if(!strncmp(lower,"axon",4) || !strncmp(lower,"ab2-",4) || !strncmp(lower,"ab3-",4) || !strncmp(lower,"ab4-",4))
        wardriver_match(o,WardriverHintAxon,WardConfidenceMedium,WardEvidenceName);
    if(o->mode==WardriverWifi) {
        if(!strncmp(lower,"pineapple_",10)) wardriver_match(o,WardriverHintPineapple,WardConfidenceMedium,WardEvidenceName);
        if(!strncmp(lower,"pwned",5)) wardriver_match(o,WardriverHintDeauther,WardConfidenceMedium,WardEvidenceName);
        return;
    }
    if(strstr(lower,"airtag")) wardriver_match(o,WardriverHintAirTag|WardriverHintTracker,WardConfidenceMedium,WardEvidenceName);
    if(strstr(lower,"smarttag")) wardriver_match(o,WardriverHintSmartTag|WardriverHintTracker,WardConfidenceMedium,WardEvidenceName);
    if(!strncmp(lower,"flipper",7)) wardriver_match(o,WardriverHintFlipper,WardConfidenceMedium,WardEvidenceName);
    if(!strcmp(lower,"hc-03") || !strcmp(lower,"hc-05") || !strcmp(lower,"hc-06") || !strcmp(lower,"rn42") || !strcmp(lower,"bt04-a")) {
        o->hints|=WardriverHintSerial;
        /* These are general-purpose serial modules, not proof of a skimmer. */
        wardriver_match(o,WardriverHintSkimmer,WardConfidenceLow,WardEvidenceName);
    }
}
static void vendor(WardriverObservation* o) {
    static const struct { uint8_t mac[3]; uint32_t hint; } signatures[]={
#include "wardriver_oui_signatures.inc"
    };
    if(o->random_address || (o->mac[0]&3)) return;
    for(size_t i=0;i<sizeof(signatures)/sizeof(*signatures);++i) {
        if(!memcmp(o->mac,signatures[i].mac,3)) {
            wardriver_match(o,signatures[i].hint,WardConfidenceMedium,WardEvidenceOui); return;
        }
    }
}
void wardriver_detect_wifi(WardriverObservation* o) { if(o) { names(o); vendor(o); } }
static void apple(const uint8_t* p,size_t n,WardriverObservation* o) {
    if(n==25 && p[2]==2 && p[3]==21) {
        wardriver_match(o,WardriverHintIBeacon,WardConfidenceHigh,WardEvidencePayload); return;
    }
    /* Nearby Find My has a short payload; disconnected has 25 bytes.
     * The status class (bits 4..5) distinguishes AirTag from AirPods/phones.
     * AirTag generations that use the generic accessory class stay Find My. */
    if(n<6 || p[2]!=0x12 || (p[3]!=2 && p[3]!=25) || n<4U+p[3]) return;
    uint32_t hint=WardriverHintFindMy;
    switch((p[4]>>4)&3) {
    case 1: hint|=WardriverHintTracker|WardriverHintAirTag; break;
    case 2: hint|=WardriverHintTracker; break;
    case 3: hint|=WardriverHintTracker|WardriverHintAirPods; break;
    default: break;
    }
    wardriver_match(o,hint,WardConfidenceHigh,WardEvidencePayload);
}
static void service(uint16_t uuid,WardriverObservation* o) {
    uint32_t hint=0; WardriverConfidence confidence=WardConfidenceHigh;
    switch(uuid) {
    case 0xFD5A: hint=WardriverHintTracker|WardriverHintSmartTag; break;
    case 0xFEED: case 0xFEEC: hint=WardriverHintTracker|WardriverHintTile; break;
    case 0xFD84: hint=WardriverHintTracker|WardriverHintChipolo; break;
    case 0xFD44: hint=WardriverHintTracker|WardriverHintFindMy; break;
    case 0xFD5F: hint=WardriverHintGlasses; break;
    case 0x3081: case 0x3082: case 0x3083: hint=WardriverHintFlipper; break;
    case 0x3100: case 0x3200: case 0x3300: case 0x3400: case 0x3500:
        hint=WardriverHintRaven; confidence=WardConfidenceMedium; break;
    case 0x1101: hint=WardriverHintSkimmer|WardriverHintSerial; confidence=WardConfidenceLow; break;
    /* FEAA alone is shared with Eddystone; FFFA alone is not decoded RID. */
    default: break;
    }
    if(hint) wardriver_match(o,hint,confidence,WardEvidenceService);
}
bool wardriver_detect_ble(const uint8_t* data,size_t length,WardriverObservation* out) {
    if(!out || (!data && length)) return false;
    WardriverObservation copy=*out;
    for(size_t pos=0;pos<length;) {
        size_t n=data[pos++];
        if(!n) break;
        if(n>length-pos) return false;
        uint8_t type=data[pos]; const uint8_t* p=data+pos+1; --n;
        if(type==0x08 || type==0x09) text_copy(copy.ssid,sizeof(copy.ssid),p,n);
        if(type==0xFF && n>=2) {
            uint16_t company=le16(p);
            if(company==0x09C8) copy.hints|=WardriverHintXuntong;
            if(company==0x004C) apple(p,n,&copy);
            if(company==0x0E29) wardriver_match(&copy,WardriverHintFlipper,WardConfidenceHigh,WardEvidenceCompany);
            if(company==0x01AB || company==0x058E || company==0x0D53 || company==0x03C2)
                wardriver_match(&copy,WardriverHintMeta,WardConfidenceLow,WardEvidenceCompany);
        }
        if(type==0x02 || type==0x03 || type==0x16) {
            if(type!=0x16 && n%2) return false;
            if(type==0x16 && n<2) return false;
            for(size_t i=0;i+1<n;i+=2) { service(le16(p+i),&copy); if(type==0x16) break; }
        }
        if(type==0x06 || type==0x07) {
            static const uint8_t base[12]={0xFB,0x34,0x9B,0x5F,0x80,0,0,0x80,0,0x10,0,0};
            if(n%16) return false;
            for(size_t i=0;i<n;i+=16) if(!memcmp(p+i,base,12) && !p[i+14] && !p[i+15]) service(le16(p+i+12),&copy);
        }
        if(type==0x16 && n>=4 && le16(p)==0xFFFA && p[2]==0x0D)
            wardriver_detect_remote_id(p+4,n-4,&copy);
        if(type==0x16 && n>=3 && le16(p)==0xFEAA) {
            if((p[2]==0x40 || p[2]==0x41) && (n==23 || n==24 || n==35 || n==36))
                wardriver_match(&copy,WardriverHintGoogle|WardriverHintTracker,WardConfidenceHigh,WardEvidencePayload);
            else if((p[2]==0 && n==22) || (p[2]==0x10 && n>=5 && n<=22) || (p[2]==0x20 && n==16) || (p[2]==0x30 && n==12))
                wardriver_match(&copy,WardriverHintEddystone,WardConfidenceHigh,WardEvidencePayload);
        }
        pos+=n+1;
    }
    names(&copy); vendor(&copy);
    *out=copy; return true;
}
static bool contains(const uint8_t* p,size_t n,const char* key) {
    size_t size=strlen(key); if(n<size) return false;
    for(size_t i=0;i<=n-size;++i) if(!memcmp(p+i,key,size)) return true;
    return false;
}
bool wardriver_detect_beacon(const uint8_t* frame,size_t length,WardriverObservation* out) {
    if(!frame || !out || length<36 || (frame[0]&0xFC)!=0x80) return false;
    WardriverObservation copy=*out;
    size_t pos;
    for(pos=36;pos+2<=length;) {
        uint8_t type=frame[pos++]; size_t n=frame[pos++];
        if(n>length-pos) return false;
        const uint8_t* p=frame+pos;
        if(type==0) { if(n>32) return false; text_copy(copy.ssid,sizeof(copy.ssid),p,n); }
        if(type==221 && n>=5 && p[0]==0xFA && p[1]==0x0B && p[2]==0xBC && p[3]==0x0D)
            wardriver_detect_remote_id(p+5,n-5,&copy);
        /* Pwnagotchi's private IE 222 carries its self-declaration JSON. */
        if(type==222 && n>2 && p[0]=='{' && contains(p,n,"\"pwnd_tot\"") && contains(p,n,"\"name\""))
            wardriver_match(&copy,WardriverHintPwnagotchi,WardConfidenceHigh,WardEvidencePayload);
        pos+=n;
    }
    if(pos!=length) return false;
    memcpy(copy.mac,frame+16,6);
    wardriver_detect_wifi(&copy);
    if(copy.hints) { *out=copy; return true; }
    return false;
}
