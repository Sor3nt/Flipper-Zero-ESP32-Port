#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ENABLE_MODELS_DATABASE
#define ENABLE_EMULATE_FEATURE
#define APP_ASSETS_PATH(x) x
#define FURI_LOG_E(...)
#define FURI_LOG_D(...)
#define FURI_LOG_I(...)
#define RECORD_STORAGE "storage"
typedef int Storage;
typedef struct { int live; } FlipperApplication;
typedef struct { const char* appid; uint32_t ep_api_version; const void* entry_point; } FlipperAppPluginDescriptor;
typedef struct { void *on_enter,*on_exit,*on_event,*set_host_api,*car_model_get_count,*car_model_get_by_index,*registry,*needs_bruteforce; } Plugin;
typedef Plugin ProtoPirateConfigPlugin;
typedef Plugin ProtoPirateSavedInfoPlugin;
typedef Plugin ProtoPirateAboutPlugin;
typedef Plugin ProtoPirateEmulatePlugin;
typedef Plugin ProtoPirateToolScenePlugin;
typedef Plugin ProtoPiratePsaBfPlugin;
typedef Plugin ProtoPirateProtocolPlugin;
typedef int FlipperApplicationPreloadStatus;
typedef int FlipperApplicationLoadStatus;
#define FlipperApplicationPreloadStatusSuccess 0
#define FlipperApplicationLoadStatusSuccess 0
static int mode, allocations, frees, records;
static const void* firmware_api_interface;
static FlipperAppPluginDescriptor descriptor;
static Storage storage;
static Storage* furi_record_open(const char* name) { records++; return &storage; }
static void furi_record_close(const char* name) { assert(records-- == 1); }
static FlipperApplication* flipper_application_alloc(Storage* s, const void* api) {
    if(mode == 1) return NULL;
    FlipperApplication* p = malloc(sizeof(*p)); p->live=1; allocations++; return p;
}
static void flipper_application_free(FlipperApplication* p) { assert(p && p->live); p->live=0; frees++; free(p); }
static int flipper_application_preload(FlipperApplication* p,const char* path) { assert(p); return mode == 2; }
static bool flipper_application_is_plugin(FlipperApplication* p) { return mode != 3; }
static int flipper_application_map_to_memory(FlipperApplication* p) { return mode == 4; }
static const FlipperAppPluginDescriptor* flipper_application_plugin_get_descriptor(FlipperApplication* p) { return mode == 5 ? NULL : &descriptor; }
#include "loader.inc"
int main(void) {
    Plugin valid = {.on_enter=&storage,.on_exit=&storage,.on_event=&storage,.set_host_api=&storage,
        .car_model_get_count=&storage,.car_model_get_by_index=&storage};
    for(mode=1;mode<=9;mode++) {
        void* handle=NULL; const void* plugin=NULL;
        descriptor=(FlipperAppPluginDescriptor){PROTOPIRATE_CONFIG_PLUGIN_APP_ID,PROTOPIRATE_CONFIG_PLUGIN_API_VERSION,&valid};
        Plugin bad=valid;
        if(mode==6) descriptor.appid="wrong";
        if(mode==7) descriptor.ep_api_version=2; // old Flipper plugin must be rejected
        if(mode==8) { bad.on_exit=NULL; descriptor.entry_point=&bad; }
        if(mode==9) descriptor.entry_point=NULL;
        assert(!shared_plugin_load(&handle,&plugin,ProtoPirateSharedPluginsConfig,NULL));
        assert(!handle && !plugin && !records && allocations==frees);
    }
    mode=0;
    descriptor=(FlipperAppPluginDescriptor){PROTOPIRATE_CONFIG_PLUGIN_APP_ID,PROTOPIRATE_CONFIG_PLUGIN_API_VERSION,&valid};
    void* handle=NULL; const void* plugin=NULL;
    assert(shared_plugin_load(&handle,&plugin,ProtoPirateSharedPluginsConfig,NULL));
    int before=allocations;
    assert(shared_plugin_load(&handle,&plugin,ProtoPirateSharedPluginsConfig,NULL));
    assert(allocations==before);
    shared_plugin_unload(&handle,&plugin);
    shared_plugin_unload(&handle,&plugin);
    assert(!handle && !plugin && allocations==frees && !records);
    puts("PASS loader: allocation/preload/map failures, null/wrong/old descriptors, invalid callbacks, reload/unload and no leaked mappings");
}
