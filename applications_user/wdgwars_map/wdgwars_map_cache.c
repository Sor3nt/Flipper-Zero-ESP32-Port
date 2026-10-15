#include "wdgwars_map_cache.h"
#include <furi.h>
#include <storage/storage.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif
static void* cache_alloc(size_t size) {
#ifdef ESP_PLATFORM
    void* p=heap_caps_malloc(size,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(p) return p;
#endif
    return malloc(size);
}
static bool read_file(Storage* s,const char* path,WdgMapSavedArea* a) {
    File* f=storage_file_alloc(s); if(!f) return false;
    bool ok=false,opened=storage_file_open(f,path,FSAM_READ,FSOM_OPEN_EXISTING);
    void* legacy=NULL;
    if(opened) {
        WdgCacheHeader h={0}; unsigned char extra;
        if(storage_file_read(f,&h,sizeof(h))==sizeof(h)) {
            size_t length=wdg_cache_payload_size(&h);
            void* payload=a;
            if(h.version==1 && length) { legacy=cache_alloc(length); payload=legacy; }
            ok=length && payload && storage_file_read(f,payload,length)==length &&
                storage_file_read(f,&extra,1)==0 && storage_file_get_error(f)==FSE_OK &&
                wdg_cache_decode(&h,payload,length,a);
        }
    }
    free(legacy);
    bool closed=storage_file_close(f); ok=ok && closed;
    storage_file_free(f); return ok;
}
bool wdg_cache_load(unsigned slot,WdgMapSavedArea* scratch) {
    if(slot>=WDG_CACHE_SLOTS || !scratch) return false;
    char path[100],backup[104];
    snprintf(path,sizeof(path),WDG_CACHE_PATH "/area%u.wdg",slot+1);
    snprintf(backup,sizeof(backup),"%s.bak",path);
    Storage* s=furi_record_open(RECORD_STORAGE);
    bool ok=read_file(s,path,scratch) || read_file(s,backup,scratch);
    furi_record_close(RECORD_STORAGE); return ok;
}
bool wdg_cache_save(unsigned slot,const WdgMapSavedArea* a) {
    if(slot>=WDG_CACHE_SLOTS || !wdg_cache_area_valid(a)) return false;
    char path[100],temp[104],backup[104];
    snprintf(path,sizeof(path),WDG_CACHE_PATH "/area%u.wdg",slot+1);
    snprintf(temp,sizeof(temp),"%s.tmp",path); snprintf(backup,sizeof(backup),"%s.bak",path);
    Storage* s=furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(s,"/ext/apps_data"); storage_simply_mkdir(s,WDG_CACHE_PATH);
    File* f=storage_file_alloc(s); WdgCacheHeader h; wdg_cache_header(&h,a);
    bool opened=f && storage_file_open(f,temp,FSAM_WRITE,FSOM_CREATE_ALWAYS);
    bool ok=opened && storage_file_write(f,&h,sizeof(h))==sizeof(h) && storage_file_write(f,a,sizeof(*a))==sizeof(*a) && storage_file_sync(f);
    if(f) { bool closed=storage_file_close(f); ok=ok && closed; }
    if(f) storage_file_free(f);
    if(ok) {
        FileInfo info; bool exists=storage_common_stat(s,path,&info)==FSE_OK;
        bool rotated=false;
        if(exists) {
            WdgMapSavedArea* check=cache_alloc(sizeof(*check));
            if(!check) ok=false;
            else {
                bool valid=read_file(s,path,check);
                bool recovery_valid=valid || read_file(s,backup,check); free(check);
                if(valid) {
                    FS_Error removed=storage_common_remove(s,backup);
                    ok=(removed==FSE_OK || removed==FSE_NOT_EXIST) && storage_common_rename(s,path,backup)==FSE_OK;
                    rotated=ok;
                } else ok=recovery_valid && storage_common_remove(s,path)==FSE_OK; /* Never destroy an unreadable sole copy. */
            }
        }
        if(ok) {
            ok=storage_common_rename(s,temp,path)==FSE_OK;
            if(!ok && rotated) storage_common_rename(s,backup,path);
            /* Keep the previous complete snapshot as a recovery copy. */
        }
    }
    if(!ok) storage_common_remove(s,temp);
    furi_record_close(RECORD_STORAGE); return ok;
}
