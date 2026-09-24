#include "wardriver_detect.h"
#include <string.h>

/* BLE name/service matching adapted from ElicoftZ's ble_detector_parse.c.
 * Company IDs identify suppliers, not unique products. All those matches are hints.
 * Remote ID wire layout: opendroneid-core-c (ASTM F3411); decoder below is app-local.
 */
static uint16_t le16(const uint8_t* p) { return p[0] | ((uint16_t)p[1]<<8); }
static void text_copy(char* to,size_t size,const uint8_t* from,size_t length) {
    size_t n=length<size-1?length:size-1;
    for(size_t i=0;i<n;++i) to[i]=(from[i]>=32 && from[i]<127)?(char)from[i]:'.';
    to[n]=0;
}
static bool basic_id(const uint8_t* p,WardriverObservation* out) {
    if((p[0]&15)>2 || (p[0]>>4)!=0) return false;
    unsigned type=p[1]>>4;
    if(type!=1 && type!=2) return false; /* Only textual serial/registration IDs. */
    if(p[2]<33 || p[2]>126) return false;
    for(unsigned i=2;i<22 && p[i];++i) if(p[i]<32 || p[i]>126) return false;
    size_t length=0;
    while(length<20 && p[2+length]) ++length;
    text_copy(out->remote_id,sizeof(out->remote_id),p+2,length);
    out->hints|=WardriverHintRemoteId;
    return true;
}
bool wardriver_detect_remote_id(const uint8_t* p,size_t length,WardriverObservation* out) {
    if(!p || !out || length<25) return false;
    if((p[0]>>4)!=15) return length==25 && basic_id(p,out);
    if((p[0]&15)>2 || p[1]!=25 || p[2]<1 || p[2]>9 || length!=3U+25U*p[2]) return false;
    bool found=false;
    for(unsigned i=0;i<p[2];++i) found=basic_id(p+3+25*i,out)||found;
    return found;
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
            if(company==0x004C && n>=4 && p[2]==0x12 && p[3]==0x19 && n>=29)
                copy.hints|=WardriverHintTracker; /* Find My accessories, not necessarily AirTag. */
        }
        if(type==0x02 || type==0x03 || type==0x16) {
            if(type!=0x16 && n%2) return false;
            for(size_t i=0;i+1<n;i+=2) {
                uint16_t uuid=le16(p+i);
                if(uuid==0xFD5A || uuid==0xFEED || uuid==0xFD84) copy.hints|=WardriverHintTracker;
                if(type==0x16) break;
            }
        }
        if(type==0x16 && n>=4 && le16(p)==0xFFFA && p[2]==0x0D)
            wardriver_detect_remote_id(p+4,n-4,&copy);
        pos+=n+1;
    }
    char lower[33];
    for(unsigned i=0;i<sizeof(lower);++i) {
        char c=copy.ssid[i]; lower[i]=c>='A'&&c<='Z'?c+('a'-'A'):c;
    }
    lower[32]=0;
    bool penguin=!strncmp(lower,"penguin-",8)&&strlen(lower)==18;
    for(unsigned i=8;penguin&&i<18;++i) penguin=lower[i]>='0'&&lower[i]<='9';
    if(penguin || !strncmp(lower,"flock",5) || !strcmp(lower,"fs ext battery")) copy.hints|=WardriverHintFlock;
    if(!strncmp(lower,"axon body",9)) copy.hints|=WardriverHintAxon;
    if(strstr(lower,"airtag") || strstr(lower,"smarttag")) copy.hints|=WardriverHintTracker;
    if(!strcmp(lower,"hc-03") || !strcmp(lower,"hc-05") || !strcmp(lower,"hc-06")) copy.hints|=WardriverHintSerial;
    *out=copy;
    return true;
}
bool wardriver_detect_beacon(const uint8_t* frame,size_t length,WardriverObservation* out) {
    if(!frame || !out || length<36 || (frame[0]&0xFC)!=0x80) return false;
    WardriverObservation copy=*out; bool found=false;
    size_t pos;
    for(pos=36;pos+2<=length;) {
        uint8_t type=frame[pos++]; size_t n=frame[pos++];
        if(n>length-pos) return false;
        const uint8_t* p=frame+pos;
        if(type==0 && n<=32) text_copy(copy.ssid,sizeof(copy.ssid),p,n);
        if(type==221 && n>=5 && p[0]==0xFA && p[1]==0x0B && p[2]==0xBC && p[3]==0x0D)
            found=wardriver_detect_remote_id(p+5,n-5,&copy)||found;
        pos+=n;
    }
    if(pos!=length) return false;
    if(found) { memcpy(copy.mac,frame+16,6); *out=copy; }
    return found;
}
