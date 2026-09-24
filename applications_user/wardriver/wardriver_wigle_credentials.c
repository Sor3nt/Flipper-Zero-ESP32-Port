#include "wardriver_wigle.h"
#include <furi.h>
#include <storage/storage.h>
#include <stdio.h>
#include <string.h>
static bool read_config(Storage* s,const char* path,WardWigleCredentials* c) {
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
                    unsigned bit=!strcmp(line,"API Name")?1:!strcmp(line,"API Token")?2:0;
                    if(!bit || (seen&bit) || !wardriver_wigle_value_valid(value,bit==1)) { ok=false; break; }
                    strcpy(bit==1?c->name:c->token,value); seen|=bit;
                }
                line=next;
            }
            ok=ok && seen==3;
        }
        bool closed=storage_file_close(f); ok=closed && ok;
    }
    storage_file_free(f); wardriver_wigle_erase(data,sizeof(data)); return ok;
}
bool wardriver_wigle_credentials_load(WardWigleCredentials* c) {
    memset(c,0,sizeof(*c)); WardWigleCredentials loaded={0};
    Storage* s=furi_record_open(RECORD_STORAGE);
    bool ok=read_config(s,WARD_WIGLE_CONFIG,&loaded) || read_config(s,WARD_WIGLE_CONFIG ".bak",&loaded);
    if(ok) *c=loaded;
    wardriver_wigle_erase(&loaded,sizeof(loaded)); furi_record_close(RECORD_STORAGE); return ok;
}
bool wardriver_wigle_credentials_save(const WardWigleCredentials* c) {
    if(!wardriver_wigle_value_valid(c->name,true) || !wardriver_wigle_value_valid(c->token,false)) return false;
    Storage* s=furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(s,"/ext/apps_data"); storage_simply_mkdir(s,"/ext/apps_data/wardriver");
    char data[320]; int n=snprintf(data,sizeof(data),"API Name: %s\nAPI Token: %s\n",c->name,c->token);
    File* f=storage_file_alloc(s);
    bool opened=f && storage_file_open(f,WARD_WIGLE_CONFIG ".tmp",FSAM_WRITE,FSOM_CREATE_ALWAYS);
    bool ok=opened && n>0 && n<(int)sizeof(data) && storage_file_write(f,data,n)==(size_t)n && storage_file_sync(f);
    if(opened) { bool closed=storage_file_close(f); ok=closed && ok; }
    if(f) storage_file_free(f);
    wardriver_wigle_erase(data,sizeof(data));
    if(ok) {
        FileInfo info; bool exists=storage_common_stat(s,WARD_WIGLE_CONFIG,&info)==FSE_OK;
        if(exists) {
            storage_common_remove(s,WARD_WIGLE_CONFIG ".bak");
            ok=storage_common_rename(s,WARD_WIGLE_CONFIG,WARD_WIGLE_CONFIG ".bak")==FSE_OK;
        }
        if(ok) {
            ok=storage_common_rename(s,WARD_WIGLE_CONFIG ".tmp",WARD_WIGLE_CONFIG)==FSE_OK;
            if(!ok && exists) storage_common_rename(s,WARD_WIGLE_CONFIG ".bak",WARD_WIGLE_CONFIG);
            if(ok) storage_common_remove(s,WARD_WIGLE_CONFIG ".bak");
        }
    }
    if(!ok) storage_common_remove(s,WARD_WIGLE_CONFIG ".tmp");
    furi_record_close(RECORD_STORAGE); return ok;
}
bool wardriver_wigle_credentials_clear(void) {
    Storage* s=furi_record_open(RECORD_STORAGE); bool ok=true;
    const char* paths[]={WARD_WIGLE_CONFIG,WARD_WIGLE_CONFIG ".bak",WARD_WIGLE_CONFIG ".tmp"};
    for(unsigned i=0;i<3;++i) {
        FS_Error error=storage_common_remove(s,paths[i]);
        if(error!=FSE_OK && error!=FSE_NOT_EXIST) ok=false;
    }
    furi_record_close(RECORD_STORAGE); return ok;
}
