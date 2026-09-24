#include "wardriver_storage.h"
#include <furi.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define LOG_DIR "/ext/apps_data/wardriver"
#define BUFFER_SIZE 4096
typedef struct { File* file; size_t used; bool unsynced; char buffer[BUFFER_SIZE]; } LogFile;
struct WardriverLog {
    Storage* storage;
    LogFile local,wigle;
    bool ok;
    char wigle_path[144];
};
static bool drain(LogFile* f) {
    if(!f->file) return true;
    bool ok=!f->used || storage_file_write(f->file,f->buffer,f->used)==f->used;
    f->used=0; return ok;
}
static bool append(LogFile* f,const char* text) {
    size_t n=strlen(text);
    if(!f->file || n>BUFFER_SIZE) return false;
    if(f->used+n>BUFFER_SIZE && !drain(f)) return false;
    memcpy(f->buffer+f->used,text,n); f->used+=n; f->unsynced=true; return true;
}
static bool open_file(Storage* s,LogFile* f,const char* path) {
    f->file=storage_file_alloc(s);
    return f->file && storage_file_open(f->file,path,FSAM_WRITE,FSOM_CREATE_NEW);
}
WardriverLog* wardriver_log_open(uint32_t timestamp,uint32_t session) {
    WardriverLog* l=calloc(1,sizeof(*l));
    if(!l) return NULL;
    l->storage=furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(l->storage,"/ext/apps_data"); storage_simply_mkdir(l->storage,LOG_DIR);
    char path[144];
    /* Include boot/session entropy and CREATE_NEW: never truncate old sessions,
     * including when the RTC is unset or a device is restarted in one second. */
    snprintf(path,sizeof(path),LOG_DIR "/wardrive_%lu_%08lx_local.csv",
        (unsigned long)timestamp,(unsigned long)session);
    snprintf(l->wigle_path,sizeof(l->wigle_path),LOG_DIR "/wardrive_%lu_%08lx_wigle.csv",
        (unsigned long)timestamp,(unsigned long)session);
    l->ok=open_file(l->storage,&l->local,path);
    if(l->ok) l->ok=append(&l->local,
        "Type,BSSID,SSID,AuthMode,FirstSeenUTC,LastSeenUTC,Detections,Channel,RSSI,"
        "GPSFix,Latitude,Longitude,AltitudeMeters,HDOP,Satellites,GPSUTC,Vendor,Hint,RemoteID,FirstUptimeMs,LastUptimeMs\n");
    return l;
}
bool wardriver_log_record(WardriverLog* l,const WardriverNetwork* n) {
    if(!l || !l->ok) return false;
    char ssid[68],vendor[60],hint[96],rid[44],mac[18],first[20],last[20],gps_time[20];
    char lat[24]="",lon[24]="",alt[24]="",hdop[16]="",sat[8]="",row[768];
    wardriver_csv_quote(ssid,sizeof(ssid),n->observation.ssid);
    wardriver_csv_quote(vendor,sizeof(vendor),n->vendor);
    wardriver_csv_quote(hint,sizeof(hint),wardriver_hint_text(n->observation.hints));
    wardriver_csv_quote(rid,sizeof(rid),n->observation.remote_id);
    wardriver_mac_text(n->observation.mac,mac);
    wardriver_format_time(n->first_seen,first); wardriver_format_time(n->last_seen,last);
    wardriver_format_time(n->gps.timestamp_valid?n->gps.timestamp:0,gps_time);
    if(n->gps.has_fix) {
        snprintf(lat,sizeof(lat),"%.7f",n->gps.latitude); snprintf(lon,sizeof(lon),"%.7f",n->gps.longitude);
        if(n->gps.altitude_valid) snprintf(alt,sizeof(alt),"%.2f",n->gps.altitude);
        if(n->gps.hdop_valid) snprintf(hdop,sizeof(hdop),"%.2f",(double)n->gps.hdop);
    }
    if(n->gps.satellites_valid) snprintf(sat,sizeof(sat),"%u",n->gps.satellites);
    const char* auth=n->observation.mode==WardriverWifi?wardriver_security(n->observation.auth):"[BLE]";
    int length=snprintf(row,sizeof(row),"%s,%s,%s,%s,%s,%s,%lu,%u,%d,%u,%s,%s,%s,%s,%s,%s,%s,%s,%s,%lu,%lu\n",
        n->observation.mode==WardriverWifi?"WIFI":"BLE",mac,ssid,auth,first,last,
        (unsigned long)n->detections,n->observation.channel,n->observation.rssi,n->gps.has_fix?1:0,
        lat,lon,alt,hdop,sat,gps_time,vendor,hint,rid,(unsigned long)n->first_seen_ms,(unsigned long)n->last_seen_ms);
    l->ok=length>0 && (size_t)length<sizeof(row) && append(&l->local,row);
    if(l->ok && n->observation.mode==WardriverWifi && n->gps.has_fix && n->gps.timestamp_valid) {
        if(!l->wigle.file) {
            l->ok=open_file(l->storage,&l->wigle,l->wigle_path) && append(&l->wigle,
                "WigleWifi-1.6,appRelease=1.3,model=T-Embed-CC1101-Plus,release=Sor3nt,device=ESP32-S3,display=Wardriver,board=T-Embed,brand=LILYGO,star=Sol,body=3,subBody=0\n"
                "MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type\n");
        }
        unsigned channel=n->observation.channel;
        unsigned frequency=channel==14?2484:channel>=1&&channel<=13?2407+5*channel:0;
        /* Follow Kismet's WiGLE convention: 0 is an unknown accuracy, not a
         * measured zero-metre error. HDOP remains in the detailed local CSV.
         * Unknown altitude also has a zero placeholder only in WiGLE export;
         * the local export preserves its absence. Coordinates are never fake. */
        char wigle_alt[24]; snprintf(wigle_alt,sizeof(wigle_alt),"%.0f",n->gps.altitude_valid?n->gps.altitude:0.0);
        length=snprintf(row,sizeof(row),"%s,%s,%s,%s,%u,%u,%d,%s,%s,%s,0,,,WIFI\n",
            mac,ssid,auth,*first?first:gps_time,channel,frequency,n->observation.rssi,lat,lon,wigle_alt);
        if(l->ok) l->ok=length>0 && (size_t)length<sizeof(row) && append(&l->wigle,row);
    }
    return l->ok;
}
static bool sync_file(LogFile* file) {
    if(!file->file || !file->unsynced) return true;
    if(!storage_file_sync(file->file)) return false;
    file->unsynced=false; return true;
}
bool wardriver_log_flush(WardriverLog* l) {
    if(!l || !l->ok) return false;
    l->ok=drain(&l->local) && drain(&l->wigle);
    if(l->ok) l->ok=sync_file(&l->local);
    if(l->ok) l->ok=sync_file(&l->wigle);
    return l->ok;
}
bool wardriver_log_close(WardriverLog* l,WardriverLogProgress progress,void* context) {
    if(!l) return false;
    /* Flush/sync each pending buffer once. Do not repeat already successful
     * periodic syncs or hide all close operations behind one generic message. */
#define PHASE(s) do { if(progress) progress(context,s); } while(0)
    if(l->ok) { PHASE("WRITING LOCAL CSV"); l->ok=drain(&l->local); }
    if(l->ok) { PHASE("SYNCING LOCAL CSV"); l->ok=sync_file(&l->local); }
    if(l->ok) { PHASE("WRITING WIGLE CSV"); l->ok=drain(&l->wigle); }
    if(l->ok) { PHASE("SYNCING WIGLE CSV"); l->ok=sync_file(&l->wigle); }
    if(l->local.file) {
        PHASE("CLOSING LOCAL CSV");
        bool closed=storage_file_close(l->local.file); l->ok=closed && l->ok;
        storage_file_free(l->local.file);
    }
    if(l->wigle.file) {
        PHASE("CLOSING WIGLE CSV");
        bool closed=storage_file_close(l->wigle.file); l->ok=closed && l->ok;
        storage_file_free(l->wigle.file);
    }
    PHASE("RELEASING STORAGE");
    bool ok=l->ok; furi_record_close(RECORD_STORAGE); free(l); return ok;
#undef PHASE
}
bool wardriver_log_ok(const WardriverLog* l) { return l && l->ok; }
