#include "wardriver_storage.h"
#include <furi.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <float.h>
#define LOG_DIR "/ext/apps_data/wardriver"
#define BUFFER_SIZE 4096

/* ESP32/FAP uses IEEE-754 binary64. Classify without pulling in __unorddf2,
 * which is not exported by the existing firmware's compiler-runtime ABI. */
static bool finite_value(double value) {
    _Static_assert(sizeof(double)==sizeof(uint64_t) && DBL_MANT_DIG==53 && DBL_MAX_EXP==1024,
        "GPS log requires IEEE-754 binary64");
    uint64_t bits; memcpy(&bits,&value,sizeof(bits));
    return ((bits>>52)&0x7ffU)!=0x7ffU;
}

/* One upload-compatible stream. No SD files/directories are touched until the
 * first actual Wi-Fi observation carrying a valid GPS fix and UTC timestamp. */
struct WardriverLog {
    Storage* storage;
    File* file;
    size_t used;
    bool opened,unsynced,ok,has_records;
    char path[144];
    char buffer[BUFFER_SIZE];
};
static bool drain(WardriverLog* l) {
    if(!l->used) return true;
    size_t written=storage_file_write(l->file,l->buffer,l->used);
    if(written!=l->used || storage_file_get_error(l->file)!=FSE_OK) return false;
    l->used=0; return true;
}
static bool append(WardriverLog* l,const char* text) {
    size_t n=strlen(text);
    if(!l->opened || n>BUFFER_SIZE) return false;
    if(l->used+n>BUFFER_SIZE && !drain(l)) return false;
    memcpy(l->buffer+l->used,text,n); l->used+=n; l->unsynced=true; return true;
}
WardriverLog* wardriver_log_open(uint32_t timestamp,uint32_t session) {
    WardriverLog* l=calloc(1,sizeof(*l));
    if(!l) return NULL;
    /* Session entropy + CREATE_NEW protects existing files even with an unset RTC. */
    snprintf(l->path,sizeof(l->path),LOG_DIR "/wardrive_%lu_%08lx_wigle.csv",
        (unsigned long)timestamp,(unsigned long)session);
    l->ok=true;
    return l;
}
bool wardriver_log_record(WardriverLog* l,const WardriverNetwork* n) {
    if(!l || !l->ok || !n) return false;
    const WardriverGpsData* gps=&n->gps; /* Never use the retained last_fix. */
    if(n->observation.mode!=WardriverWifi || !gps->has_fix || !gps->timestamp_valid ||
       !finite_value(gps->latitude) || !finite_value(gps->longitude) ||
       gps->latitude < -90 || gps->latitude > 90 ||
       gps->longitude < -180 || gps->longitude > 180) return true;
    char time[20]; wardriver_format_time(gps->timestamp,time);
    if(!time[0]) return true;
    unsigned channel=n->observation.channel;
    if(channel<1 || channel>14) return true;
    char ssid[68],mac[18],altitude[24],row[320];
    wardriver_csv_quote(ssid,sizeof(ssid),n->observation.ssid);
    wardriver_mac_text(n->observation.mac,mac);
    /* 0 is the format's unknown-altitude/accuracy placeholder, not fake GPS. */
    snprintf(altitude,sizeof(altitude),"%.0f",
        gps->altitude_valid && finite_value(gps->altitude)?gps->altitude:0.0);
    int length=snprintf(row,sizeof(row),"%s,%s,%s,%s,%u,%u,%d,%.7f,%.7f,%s,0,,,WIFI\n",
        mac,ssid,wardriver_security(n->observation.auth),time,channel,
        channel==14?2484:2407+5*channel,n->observation.rssi,
        gps->latitude,gps->longitude,altitude);
    if(length<=0 || (size_t)length>=sizeof(row)) { l->ok=false; return false; }
    if(!l->opened) {
        l->storage=furi_record_open(RECORD_STORAGE);
        storage_simply_mkdir(l->storage,"/ext/apps_data"); storage_simply_mkdir(l->storage,LOG_DIR);
        l->file=storage_file_alloc(l->storage);
        l->opened=l->file && storage_file_open(l->file,l->path,FSAM_WRITE,FSOM_CREATE_NEW);
        l->ok=l->opened && append(l,
            "WigleWifi-1.6,appRelease=1.5.1,model=T-Embed-CC1101-Plus,release=Sor3nt,device=ESP32-S3,display=Wardriver,board=T-Embed,brand=LILYGO,star=Sol,body=3,subBody=0\n"
            "MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type\n");
    }
    if(l->ok) l->ok=append(l,row);
    if(l->ok) l->has_records=true;
    return l->ok;
}
static bool sync_file(WardriverLog* l) {
    if(!l->opened || !l->unsynced) return true;
    if(!storage_file_sync(l->file)) return false;
    l->unsynced=false; return true;
}
bool wardriver_log_flush(WardriverLog* l) {
    if(!l || !l->ok) return false;
    l->ok=drain(l) && sync_file(l);
    return l->ok;
}
bool wardriver_log_close(WardriverLog* l,WardriverLogProgress progress,void* context) {
    if(!l) return false;
#define PHASE(s) do { if(progress) progress(context,s); } while(0)
    if(l->opened) {
        if(l->ok) { PHASE("WRITING UPLOAD CSV"); l->ok=drain(l); }
        if(l->ok) { PHASE("SYNCING UPLOAD CSV"); l->ok=sync_file(l); }
        PHASE("CLOSING UPLOAD CSV");
        bool closed=storage_file_close(l->file); l->ok=closed && l->ok;
    }
    if(l->file) storage_file_free(l->file);
    if(l->storage) { PHASE("RELEASING STORAGE"); furi_record_close(RECORD_STORAGE); }
    bool ok=l->ok; free(l); return ok;
#undef PHASE
}
bool wardriver_log_ok(const WardriverLog* l) { return l && l->ok; }
bool wardriver_log_has_records(const WardriverLog* l) { return l && l->has_records; }
