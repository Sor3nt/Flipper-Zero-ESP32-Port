#include "wdgwars_map_cache.h"
#include <furi.h>
#include <storage/storage.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define DATA_CAP (sizeof(WdgMapSavedArea)+sizeof(WdgCacheHeader)+16)
typedef struct { char path[128]; unsigned char* data; size_t length; bool used; } Stored;
struct Storage { int unused; } store;
struct File { Stored* file; size_t position; bool attempted,closed,write; FS_Error error; };
static Stored files[16];
typedef enum { NoFault,OpenWriteFault,HeaderWriteFault,BodyWriteFault,SyncFault,CloseWriteFault,RotateFault,InstallFault,BackupRemoveFault,MainReadFault,MainReadAndInstallFault } Fault;
static Fault fault;
static unsigned write_calls,rename_calls,refs,allocated;
static Stored* find(const char* path) { for(unsigned i=0;i<16;++i) if(files[i].used&&!strcmp(files[i].path,path)) return &files[i]; return NULL; }
static Stored* create(const char* path) { Stored* f=find(path); if(f) return f; for(unsigned i=0;i<16;++i) if(!files[i].used) { f=&files[i]; f->used=true; strcpy(f->path,path); if(!f->data) f->data=malloc(DATA_CAP); assert(f->data); f->length=0; return f; } assert(0); return NULL; }
void* furi_record_open(const char* name) { assert(!strcmp(name,RECORD_STORAGE)); ++refs; return &store; }
void furi_record_close(const char* name) { assert(!strcmp(name,RECORD_STORAGE)&&refs); --refs; }
File* storage_file_alloc(Storage* s) { assert(s==&store); ++allocated; return calloc(1,sizeof(File)); }
bool storage_file_open(File* f,const char* path,int access,int mode) { f->attempted=true; f->write=access==FSAM_WRITE; if(f->write&&fault==OpenWriteFault) return false; f->file=f->write?create(path):find(path); if(!f->file) return false; if(f->write&&mode==FSOM_CREATE_ALWAYS) f->file->length=0; return true; }
size_t storage_file_read(File* f,void* out,size_t count) { assert(f->file&&!f->write&&!f->closed); size_t n=f->file->length-f->position; if(n>count) n=count; if((fault==MainReadFault||fault==MainReadAndInstallFault) && strstr(f->file->path,".wdg") && !strstr(f->file->path,".bak") && count==sizeof(WdgMapSavedArea)) n/=2; memcpy(out,f->file->data+f->position,n); f->position+=n; return n; }
size_t storage_file_write(File* f,const void* data,size_t count) { assert(f->file&&f->write&&!f->closed); ++write_calls; size_t n=count; if((fault==HeaderWriteFault&&write_calls==1)||(fault==BodyWriteFault&&write_calls==2)) n/=2; assert(f->position+n<=DATA_CAP); memcpy(f->file->data+f->position,data,n); f->position+=n; f->file->length=f->position; return n; }
bool storage_file_sync(File* f) { assert(f->write); return fault!=SyncFault; }
bool storage_file_close(File* f) { assert(f->attempted&&!f->closed); f->closed=true; return !(f->write&&fault==CloseWriteFault); }
void storage_file_free(File* f) { assert(f); assert(!f->attempted||f->closed); --allocated; free(f); }
FS_Error storage_file_get_error(File* f) { return f->error; }
int storage_common_stat(Storage* s,const char* path,FileInfo* info) { (void)info; assert(s==&store); return find(path)?FSE_OK:FSE_NOT_EXIST; }
int storage_common_remove(Storage* s,const char* path) { assert(s==&store); if(fault==BackupRemoveFault&&strstr(path,".bak")) return FSE_INTERNAL; Stored* f=find(path); if(!f) return FSE_NOT_EXIST; f->used=false; return FSE_OK; }
int storage_common_rename(Storage* s,const char* old,const char* dest) { assert(s==&store); ++rename_calls; if((fault==RotateFault&&strstr(dest,".bak")) || ((fault==InstallFault||fault==MainReadAndInstallFault)&&strstr(old,".tmp"))) return FSE_INTERNAL; Stored* f=find(old); if(!f) return FSE_NOT_EXIST; if(find(dest)) return FSE_INTERNAL; strcpy(f->path,dest); return FSE_OK; }
bool storage_simply_mkdir(Storage* s,const char* path) { assert(s==&store&&path); return true; }
bool wdg_map_http_get(WdgMapHttpRequest* r) { (void)r; assert(0); return false; }
static void reset(void) { for(unsigned i=0;i<16;++i) files[i].used=false; fault=NoFault; write_calls=rename_calls=0; assert(!refs&&!allocated); }
static WdgMapSavedArea* area(unsigned value) { WdgMapSavedArea* a=calloc(1,sizeof(*a)); assert(a); a->center_lat=41; a->center_lon=-81; a->zoom=15; a->flags=WDG_CACHE_TERRITORIES|WDG_CACHE_ACCOUNT; a->account.wifi=value; strcpy(a->account.username,"Test user"); a->territories.bounds=(WdgMapBounds){40,-82,42,-80}; a->territories.zoom=15; a->territories.grid_lat_e6=a->territories.grid_lon_e6=20000; a->territories.cell_count=1; a->territories.cells[0]=(WdgMapCell){41000000,-81000000,1,value,0x123456}; return a; }
static void codec_checks(WdgMapSavedArea* a) {
    WdgCacheHeader h; assert(wdg_cache_crc("123456789",9)==0xcbf43926); assert(wdg_cache_area_valid(a)); wdg_cache_header(&h,a); assert(wdg_cache_verify(&h,a));
    a->account.wifi++; assert(!wdg_cache_verify(&h,a)); a->account.wifi--; h.version++; assert(!wdg_cache_verify(&h,a)); h.version--; h.magic=0; assert(!wdg_cache_verify(&h,a));
    double value=a->zoom; a->zoom=NAN; assert(!wdg_cache_area_valid(a)); a->zoom=value;
    a->territories.cell_count=WDG_MAP_MAX_CELLS+1; assert(!wdg_cache_area_valid(a)); a->territories.cell_count=1;
    a->territories.cells[0].lat_e6=91000000; assert(!wdg_cache_area_valid(a)); a->territories.cells[0].lat_e6=41000000;
    a->territories.hull_count=1; a->territories.point_count=3; a->territories.hulls[0]=(WdgMapHull){.first_point=2,.point_count=3}; assert(!wdg_cache_area_valid(a)); a->territories.hull_count=a->territories.point_count=0;
    a->territories.hull_count=1; a->territories.point_count=3; a->territories.hulls[0]=(WdgMapHull){.first_point=0,.point_count=3}; memset(a->territories.hulls[0].name,'A',sizeof(a->territories.hulls[0].name)); assert(!wdg_cache_area_valid(a)); a->territories.hulls[0].name[0]=0; assert(wdg_cache_area_valid(a)); a->territories.hull_count=a->territories.point_count=0;
    a->territories.cell_anchor=WdgMapCellAnchorSouthWest; a->territories.grid_lat_e6=0; assert(!wdg_cache_area_valid(a)); a->territories.grid_lat_e6=20000; a->territories.cell_anchor=WdgMapCellAnchorUnknown;
    memset(a->account.username,'A',sizeof(a->account.username)); assert(!wdg_cache_area_valid(a)); strcpy(a->account.username,"Test user");
    a->flags|=0x80000000; assert(!wdg_cache_area_valid(a)); a->flags&=~0x80000000;
}
static WdgMapSavedAreaV1* legacy(const WdgMapSavedArea* source) {
    WdgMapSavedAreaV1* old=calloc(1,sizeof(*old)); assert(old);
    old->center_lat=source->center_lat; old->center_lon=source->center_lon; old->zoom=source->zoom;
    old->flags=source->flags; old->account_at=source->account_at;
    memcpy(&old->territories,&source->territories,offsetof(WdgMapSnapshotV1,cells));
    memcpy(old->territories.cells,source->territories.cells,sizeof(old->territories.cells));
    memcpy(old->territories.gangs,source->territories.gangs,sizeof(old->territories.gangs));
    memcpy(old->territories.hulls,source->territories.hulls,sizeof(old->territories.hulls));
    memcpy(old->territories.points,source->territories.points,sizeof(old->territories.points));
    old->basemap=source->basemap; old->account=source->account;
    return old;
}
static WdgCacheHeader legacy_header(const WdgMapSavedAreaV1* old) {
    return (WdgCacheHeader){0x5744474d,1,sizeof(*old),wdg_cache_crc(old,sizeof(*old))};
}
static void install_legacy(const char* path,const WdgMapSavedAreaV1* old) {
    Stored* f=create(path); WdgCacheHeader h=legacy_header(old);
    memcpy(f->data,&h,sizeof(h)); memcpy(f->data+sizeof(h),old,sizeof(*old)); f->length=sizeof(h)+sizeof(*old);
}
static void migration_checks(WdgMapSavedArea* source,WdgMapSavedArea* out) {
    assert(sizeof(WdgMapSavedAreaV1)==114736 && sizeof(WdgMapSnapshotV1)==68416 && sizeof(WdgMapHull)==80);
    assert(sizeof(WdgMapSnapshot)==524352 && sizeof(WdgMapSavedArea)==570672);
    assert(sizeof(WdgMapSavedArea)>sizeof(WdgMapSavedAreaV1));
    WdgMapSavedAreaV1* old=legacy(source);
    old->territories.gang_count=1; old->territories.gangs[0]=(WdgMapGang){.id=99,.color_rgb=0x123456};
    strcpy(old->territories.gangs[0].name,"Legacy gang");
    old->territories.hull_count=1; old->territories.point_count=3;
    old->territories.hulls[0]=(WdgMapHull){.gang_id=99,.color_rgb=0x123456,.first_point=0,.point_count=3};
    strcpy(old->territories.hulls[0].name,"Legacy named hull");
    old->territories.points[0]=(WdgMapPoint){41000000,-81000000};
    old->territories.points[1]=(WdgMapPoint){41000100,-81000000};
    old->territories.points[2]=(WdgMapPoint){41000000,-81000100};
    WdgCacheHeader h=legacy_header(old);
    assert(wdg_cache_payload_size(&h)==114736 && wdg_cache_decode(&h,old,sizeof(*old),out));
    assert(!strcmp(out->territories.hulls[0].name,"Legacy named hull"));
    assert(out->territories.gangs[0].id==99 && out->territories.points[2].lon_e6==-81000100);
    assert(out->account.wifi==source->account.wifi && out->center_lat==source->center_lat);
    assert(!out->territories.gangs[100].id && !out->territories.hulls[200].point_count && !out->territories.points[5000].lat_e6);
    old->account.wifi^=1; assert(!wdg_cache_decode(&h,old,sizeof(*old),out)); old->account.wifi^=1;
    assert(!wdg_cache_decode(&h,old,sizeof(*old)-1,out));
    h.version=99; assert(!wdg_cache_payload_size(&h) && !wdg_cache_decode(&h,old,sizeof(*old),out)); h=legacy_header(old);
    h.length++; assert(!wdg_cache_payload_size(&h)); h=legacy_header(old);
    old->territories.gang_count=65; h=legacy_header(old); assert(!wdg_cache_decode(&h,old,sizeof(*old),out)); old->territories.gang_count=1;
    old->territories.hull_count=129; h=legacy_header(old); assert(!wdg_cache_decode(&h,old,sizeof(*old),out)); old->territories.hull_count=1;
    old->territories.point_count=4097; h=legacy_header(old); assert(!wdg_cache_decode(&h,old,sizeof(*old),out)); old->territories.point_count=3;
    old->territories.hulls[0].first_point=3; h=legacy_header(old); assert(!wdg_cache_decode(&h,old,sizeof(*old),out)); old->territories.hulls[0].first_point=0;
    old->territories.points[0].lat_e6=90000001; h=legacy_header(old); assert(!wdg_cache_decode(&h,old,sizeof(*old),out)); old->territories.points[0].lat_e6=41000000;
    reset(); install_legacy(WDG_CACHE_PATH "/area1.wdg",old);
    assert(wdg_cache_load(0,out) && !strcmp(out->territories.hulls[0].name,"Legacy named hull"));
    assert(wdg_cache_save(0,out));
    Stored* f=find(WDG_CACHE_PATH "/area1.wdg"); memcpy(&h,f->data,sizeof(h)); assert(h.version==2 && h.length==sizeof(*out));
    f->data[sizeof(h)+100]^=1; assert(wdg_cache_load(0,out) && out->territories.gang_count==1); /* v1 backup recovery */
    assert(wdg_cache_save(0,out)); assert(wdg_cache_load(0,out));
    reset(); install_legacy(WDG_CACHE_PATH "/area1.wdg",old); f=find(WDG_CACHE_PATH "/area1.wdg"); f->length-=4; assert(!wdg_cache_load(0,out));
    reset(); install_legacy(WDG_CACHE_PATH "/area1.wdg",old); f=find(WDG_CACHE_PATH "/area1.wdg"); f->data[f->length++]=0; assert(!wdg_cache_load(0,out));
    reset(); install_legacy(WDG_CACHE_PATH "/area1.wdg",old); install_legacy(WDG_CACHE_PATH "/area1.wdg.bak",old);
    f=find(WDG_CACHE_PATH "/area1.wdg"); f->data[sizeof(h)+500]^=1; assert(wdg_cache_load(0,out));
    free(old); reset();
}
static void expanded_roundtrip(WdgMapSavedArea* source,WdgMapSavedArea* out) {
    WdgMapSavedArea* large=calloc(1,sizeof(*large)); assert(large); *large=*source;
    large->territories.gang_count=583; large->territories.hull_count=1478; large->territories.point_count=12586;
    for(unsigned i=0;i<large->territories.gang_count;++i) { large->territories.gangs[i].id=i+1; large->territories.gangs[i].color_rgb=0xabcdef; }
    for(unsigned i=0;i<large->territories.hull_count;++i) large->territories.hulls[i]=(WdgMapHull){.gang_id=i%583+1,.color_rgb=0xabcdef,.first_point=(uint16_t)(i*3),.point_count=3};
    strcpy(large->territories.gangs[582].name,"Expanded final gang");
    strcpy(large->territories.hulls[1477].name,"Expanded final hull");
    reset(); assert(wdg_cache_save(0,large) && wdg_cache_load(0,out)); assert(!memcmp(large,out,sizeof(*large)));
    WdgCacheHeader h; wdg_cache_header(&h,large); assert(wdg_cache_payload_size(&h)==sizeof(*large));
    assert(wdg_cache_decode(&h,large,sizeof(*large),out));
    large->territories.gang_count=1025; assert(!wdg_cache_area_valid(large)); large->territories.gang_count=583;
    large->territories.hull_count=2049; assert(!wdg_cache_area_valid(large)); large->territories.hull_count=1478;
    large->territories.point_count=32769; assert(!wdg_cache_area_valid(large));
    free(large); reset();
}
int main(int argc,char** argv) {
    WdgMapSavedArea* old=area(101),*current=area(202),*newer=area(303),*out=area(0); codec_checks(old);
    reset(); assert(!wdg_cache_load(0,out)); assert(!wdg_cache_save(4,old)&&!wdg_cache_load(4,out));
    assert(wdg_cache_save(0,old)); assert(wdg_cache_load(0,out)&&out->account.wifi==101);
    assert(wdg_cache_save(0,current)); assert(wdg_cache_load(0,out)&&out->account.wifi==202);
    Stored* main=find(WDG_CACHE_PATH "/area1.wdg"); assert(main); main->data[sizeof(WdgCacheHeader)+100]^=1;
    assert(wdg_cache_load(0,out)&&out->account.wifi==101); assert(wdg_cache_save(0,newer)); assert(wdg_cache_load(0,out)&&out->account.wifi==303);
    main=find(WDG_CACHE_PATH "/area1.wdg"); main->data[sizeof(WdgCacheHeader)+100]^=1; assert(wdg_cache_load(0,out)&&out->account.wifi==101);
    Fault faults[]={OpenWriteFault,HeaderWriteFault,BodyWriteFault,SyncFault,CloseWriteFault,RotateFault,InstallFault,BackupRemoveFault};
    for(unsigned i=0;i<sizeof(faults)/sizeof(faults[0]);++i) { reset(); assert(wdg_cache_save(0,old)&&wdg_cache_save(0,current)); fault=faults[i]; write_calls=rename_calls=0; assert(!wdg_cache_save(0,newer)); fault=NoFault; assert(wdg_cache_load(0,out)&&out->account.wifi==202); assert(!refs&&!allocated); }
    reset(); assert(wdg_cache_save(0,current)); fault=MainReadAndInstallFault; assert(!wdg_cache_save(0,newer)); fault=NoFault; assert(wdg_cache_load(0,out)&&out->account.wifi==202);
    reset(); assert(wdg_cache_save(0,current)); main=find(WDG_CACHE_PATH "/area1.wdg"); main->length-=4; assert(!wdg_cache_load(0,out));
    reset(); assert(wdg_cache_save(0,current)); main=find(WDG_CACHE_PATH "/area1.wdg"); main->data[main->length++]=0; assert(!wdg_cache_load(0,out));
    migration_checks(old,out); expanded_roundtrip(current,out);
    if(argc==3 && !strcmp(argv[1],"--check-private")) {
        /* Read-only local verification. Never write/copy private cache content. */
        FILE* file=fopen(argv[2],"rb"); assert(file); WdgCacheHeader h;
        assert(fread(&h,1,sizeof(h),file)==sizeof(h)); size_t length=wdg_cache_payload_size(&h); assert(length);
        void* payload=malloc(length); assert(payload); assert(fread(payload,1,length,file)==length && fgetc(file)==EOF); fclose(file);
        assert(wdg_cache_decode(&h,payload,length,out)); free(payload);
        puts("Private SD cache: CRC/ranges valid and migration succeeded (contents not emitted)");
    }
    reset(); free(old); free(current); free(newer); free(out); for(unsigned i=0;i<16;++i) free(files[i].data); puts("WDG map cache: codec and storage fault checks passed"); return 0;
}
