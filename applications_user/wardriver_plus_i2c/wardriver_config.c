#include "wardriver_config.h"
#include <furi.h>
#include <storage/storage.h>
#include <string.h>
#include <stdio.h>
#define CONFIG_PATH "/ext/apps_data/wardriver/config.conf"
void wardriver_config_defaults(WardriverConfig* c) {
    *c=(WardriverConfig){.gps_uart=2,.gps_baud=9600,.gps_rx=-1,.gps_tx=-1};
}
/* Keep the FlipperFormat text on disk, but bound parsing to one small read.
 * The generic stream reader can retry forever on zero-byte reads without EOF
 * after an SD error. An absent/malformed configuration must leave GPS OFF. */
static char* trim(char* s) {
    while(*s==' ' || *s=='\t' || *s=='\r') ++s;
    size_t n=strlen(s);
    while(n && (s[n-1]==' ' || s[n-1]=='\t' || s[n-1]=='\r')) s[--n]=0;
    return s;
}
static bool number(const char* s,uint32_t* value) {
    if(!*s) return false;
    uint32_t n=0;
    for(;*s;++s) {
        if(*s<'0' || *s>'9') return false;
        unsigned digit=*s-'0';
        if(n>(UINT32_MAX-digit)/10) return false;
        n=n*10+digit;
    }
    *value=n; return true;
}
static bool parse(char* data,WardriverConfig* c) {
    static const char* keys[]={"Filetype","Version","GPS Mode","GPS UART","GPS Baud",
        "GPS RX","GPS TX","Isolated UART Pins"};
    unsigned seen=0;
    for(char* line=data;line;) {
        char* next=strchr(line,'\n'); if(next) *next++=0;
        line=trim(line);
        if(*line && *line!='#') {
            char* colon=strchr(line,':'); if(!colon) return false;
            *colon=0; char* key=trim(line); char* value=trim(colon+1);
            unsigned i=0; while(i<8 && strcmp(key,keys[i])) ++i;
            if(i<8) {
                if(seen & (1U<<i)) return false;
                seen|=1U<<i;
                uint32_t v=0;
                if(i==0) { if(strcmp(value,"Wardriver settings")) return false; }
                else if(i==7) {
                    if(strcmp(value,"true") && strcmp(value,"false")) return false;
                    c->isolated_uart_pins=!strcmp(value,"true");
                } else {
                    if(!number(value,&v)) return false;
                    switch(i) {
                    case 1: if(v!=1) return false; break;
                    case 2: if(v>2) return false; c->gps_mode=v; break;
                    case 3: if(v<1 || v>2) return false; c->gps_uart=v; break;
                    case 4: if(v!=9600 && v!=38400 && v!=115200) return false; c->gps_baud=v; break;
                    case 5: case 6:
                        if(v!=UINT32_MAX && v>48) return false;
                        if(i==5) c->gps_rx=v==UINT32_MAX?-1:(int32_t)v;
                        else c->gps_tx=v==UINT32_MAX?-1:(int32_t)v;
                        break;
                    }
                }
            }
        }
        line=next;
    }
    return seen==255;
}
static bool load_file(Storage* s,const char* path,WardriverConfig* c) {
    File* f=storage_file_alloc(s); if(!f) return false;
    bool ok=storage_file_open(f,path,FSAM_READ,FSOM_OPEN_EXISTING);
    if(ok) {
        char data[513];
        size_t n=storage_file_read(f,data,sizeof(data)-1);
        ok=n>0 && n<sizeof(data)-1 && !memchr(data,0,n);
        if(ok) { data[n]=0; ok=parse(data,c); }
        bool closed=storage_file_close(f); ok=ok && closed;
    }
    storage_file_free(f); return ok;
}
bool wardriver_config_load(WardriverConfig* c) {
    wardriver_config_defaults(c);
    Storage* s=furi_record_open(RECORD_STORAGE);
    WardriverConfig loaded=*c;
    bool ok=load_file(s,CONFIG_PATH,&loaded) || load_file(s,CONFIG_PATH ".bak",&loaded);
    if(ok) *c=loaded;
    furi_record_close(RECORD_STORAGE); return ok;
}
bool wardriver_config_save(const WardriverConfig* c) {
    Storage* s=furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(s,"/ext/apps_data");
    storage_simply_mkdir(s,"/ext/apps_data/wardriver");
    char buffer[384];
    int length=snprintf(buffer,sizeof(buffer),
        "Filetype: Wardriver settings\nVersion: 1\nGPS Mode: %lu\nGPS UART: %lu\n"
        "GPS Baud: %lu\nGPS RX: %lu\nGPS TX: %lu\nIsolated UART Pins: %s\n",
        (unsigned long)c->gps_mode,(unsigned long)c->gps_uart,(unsigned long)c->gps_baud,
        (unsigned long)(uint32_t)c->gps_rx,(unsigned long)(uint32_t)c->gps_tx,c->isolated_uart_pins?"true":"false");
    File* file=storage_file_alloc(s);
    bool ok=file && length>0 && (size_t)length<sizeof(buffer) &&
        storage_file_open(file,CONFIG_PATH ".tmp",FSAM_WRITE,FSOM_CREATE_ALWAYS) &&
        storage_file_write(file,buffer,length)==(size_t)length && storage_file_sync(file);
    if(file) { storage_file_close(file); storage_file_free(file); }
    bool had_old=false;
    if(ok) {
        FileInfo info;
        had_old=storage_common_stat(s,CONFIG_PATH,&info)==FSE_OK;
        if(had_old) {
            storage_common_remove(s,CONFIG_PATH ".bak");
            ok=storage_common_rename(s,CONFIG_PATH,CONFIG_PATH ".bak")==FSE_OK;
        }
        if(ok) {
            ok=storage_common_rename(s,CONFIG_PATH ".tmp",CONFIG_PATH)==FSE_OK;
            if(!ok && had_old) storage_common_rename(s,CONFIG_PATH ".bak",CONFIG_PATH);
            if(ok) storage_common_remove(s,CONFIG_PATH ".bak");
        }
    }
    furi_record_close(RECORD_STORAGE); return ok;
}
