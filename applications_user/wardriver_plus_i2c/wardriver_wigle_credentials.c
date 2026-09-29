#include "wardriver_wigle.h"
#include <furi.h>
#include <storage/storage.h>
#include <stdio.h>
#include <string.h>
static bool read_config(Storage* s,const char* path,WardWigleCredentials* c,bool wdg) {
    memset(c,0,sizeof(*c));
    File* f=storage_file_alloc(s); if(!f) return false;
    char data[512]={0}; bool ok=false;
    if(storage_file_open(f,path,FSAM_READ,FSOM_OPEN_EXISTING)) {
        size_t n=storage_file_read(f,data,sizeof(data)-1);
        if(n && n<sizeof(data)-1 && !memchr(data,0,n) && storage_file_get_error(f)==FSE_OK) {
            unsigned seen=0; ok=true;
            for(char* line=data;line && ok;) {
                char* next=strchr(line,'\n'); if(next) *next++=0;
                size_t len=strlen(line); if(len && line[len-1]=='\r') line[len-1]=0;
                if(*line && *line!='#') {
                    char* value=strchr(line,':'); if(!value) { ok=false; break; }
                    *value++=0; if(*value==' ') ++value;
                    unsigned bit=wdg?(!strcmp(line,"API Key")?2:0):!strcmp(line,"API Name")?1:!strcmp(line,"API Token")?2:0;
                    if(!bit || (seen&bit) || !(wdg?wardriver_wdg_key_valid(value):wardriver_wigle_value_valid(value,bit==1))) { ok=false; break; }
                    strcpy(bit==1?c->name:c->token,value); seen|=bit;
                }
                line=next;
            }
            ok=ok && seen==(wdg?2U:3U);
        }
        bool closed=storage_file_close(f); ok=closed && ok;
    }
    storage_file_free(f); wardriver_wigle_erase(data,sizeof(data)); return ok;
}
static bool credentials_load(WardWigleCredentials* c,bool wdg) {
    memset(c,0,sizeof(*c)); WardWigleCredentials loaded={0};
    Storage* s=furi_record_open(RECORD_STORAGE);
    bool ok=read_config(s,wdg?WARD_WDG_CONFIG:WARD_WIGLE_CONFIG,&loaded,wdg) ||
        read_config(s,wdg?WARD_WDG_CONFIG ".bak":WARD_WIGLE_CONFIG ".bak",&loaded,wdg);
    if(ok) *c=loaded;
    wardriver_wigle_erase(&loaded,sizeof(loaded)); furi_record_close(RECORD_STORAGE); return ok;
}
static bool credentials_save(const WardWigleCredentials* c,bool wdg) {
    if(wdg?!wardriver_wdg_key_valid(c->token):(!wardriver_wigle_value_valid(c->name,true) || !wardriver_wigle_value_valid(c->token,false))) return false;
    const char* path=wdg?WARD_WDG_CONFIG:WARD_WIGLE_CONFIG;
    const char* temp=wdg?WARD_WDG_CONFIG ".tmp":WARD_WIGLE_CONFIG ".tmp";
    const char* backup=wdg?WARD_WDG_CONFIG ".bak":WARD_WIGLE_CONFIG ".bak";
    Storage* s=furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(s,"/ext/apps_data"); storage_simply_mkdir(s,"/ext/apps_data/wardriver");
    char data[320]; int n=wdg?snprintf(data,sizeof(data),"API Key: %s\n",c->token):
        snprintf(data,sizeof(data),"API Name: %s\nAPI Token: %s\n",c->name,c->token);
    File* f=storage_file_alloc(s);
    bool opened=f && storage_file_open(f,temp,FSAM_WRITE,FSOM_CREATE_ALWAYS);
    bool ok=opened && n>0 && n<(int)sizeof(data) && storage_file_write(f,data,n)==(size_t)n && storage_file_sync(f);
    if(opened) { bool closed=storage_file_close(f); ok=closed && ok; }
    if(f) storage_file_free(f);
    wardriver_wigle_erase(data,sizeof(data));
    if(ok) {
        FileInfo info; bool exists=storage_common_stat(s,path,&info)==FSE_OK;
        if(exists) {
            storage_common_remove(s,backup);
            ok=storage_common_rename(s,path,backup)==FSE_OK;
        }
        if(ok) {
            ok=storage_common_rename(s,temp,path)==FSE_OK;
            if(!ok && exists) storage_common_rename(s,backup,path);
            if(ok) storage_common_remove(s,backup);
        }
    }
    if(!ok) storage_common_remove(s,temp);
    furi_record_close(RECORD_STORAGE); return ok;
}
static bool credentials_clear(bool wdg) {
    Storage* s=furi_record_open(RECORD_STORAGE); bool ok=true;
    const char* paths[]={wdg?WARD_WDG_CONFIG:WARD_WIGLE_CONFIG,
        wdg?WARD_WDG_CONFIG ".bak":WARD_WIGLE_CONFIG ".bak",
        wdg?WARD_WDG_CONFIG ".tmp":WARD_WIGLE_CONFIG ".tmp"};
    for(unsigned i=0;i<3;++i) {
        FS_Error error=storage_common_remove(s,paths[i]);
        if(error!=FSE_OK && error!=FSE_NOT_EXIST) ok=false;
    }
    furi_record_close(RECORD_STORAGE); return ok;
}
bool wardriver_wigle_credentials_load(WardWigleCredentials* c) { return credentials_load(c,false); }
bool wardriver_wigle_credentials_save(const WardWigleCredentials* c) { return credentials_save(c,false); }
bool wardriver_wigle_credentials_clear(void) { return credentials_clear(false); }
bool wardriver_wdg_credentials_load(WardWigleCredentials* c) { return credentials_load(c,true); }
bool wardriver_wdg_credentials_save(const WardWigleCredentials* c) { return credentials_save(c,true); }
bool wardriver_wdg_credentials_clear(void) { return credentials_clear(true); }
