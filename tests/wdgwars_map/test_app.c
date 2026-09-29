#include "wdgwars_map_render.h"
#include "wdgwars_map_profile.h"
#include "wdgwars_map_cache.h"
#include "wdgwars_map_client.h"
#include "../wardriver/wardriver_gps.h"
#include "../wardriver/wardriver_wigle.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
typedef struct FuriMessageQueue { int unused; } FuriMessageQueue;
typedef struct FuriMutex { int locked; } FuriMutex;
typedef struct FuriThread { int unused; } FuriThread;
#define FuriWaitForever UINT32_MAX
typedef enum { InputKeyUp,InputKeyDown,InputKeyRight,InputKeyLeft,InputKeyOk,InputKeyBack } InputKey;
typedef enum { InputTypePress,InputTypeRelease,InputTypeShort,InputTypeLong,InputTypeRepeat } InputType;
typedef struct { InputKey key; InputType type; } InputEvent;
static uint64_t clock_ms=10000;
static uint32_t rtc=1704067200;
static bool connected=true,uart_available=true,alloc_fail,have_key=true,load_ok,save_ok=true;
static bool roads_ok=true,territories_ok=true,account_ok=true,roads_partial;
static WardriverConfig saved_gps;
static unsigned thread_started,thread_joined,thread_freed,gps_started,gps_stopped,nmea_cleared,save_calls,roads_calls,territory_calls,account_calls;
static unsigned roads_count=3,territory_count=2,hull_count;
static WdgMapSavedArea* loaded;
static FuriThread thread;
static void (*during_save)(void*);
static void* save_context;
static int64_t esp_timer_get_time(void) { return (int64_t)clock_ms*1000; }
static uint32_t furi_hal_rtc_get_timestamp(void) { return rtc; }
static bool wlan_hal_is_connected(void) { return connected; }
#if !WDG_MAP_PLUS_PROFILE
static bool furi_hal_external_uart_available(void) { return uart_available; }
#endif
static void furi_mutex_acquire(FuriMutex* m,uint32_t timeout) { assert(timeout==FuriWaitForever&&!m->locked); m->locked=1; }
static void furi_mutex_release(FuriMutex* m) { assert(m->locked); m->locked=0; }
static FuriThread* furi_thread_alloc_ex(const char* name,uint32_t stack,int32_t (*fn)(void*),void* arg) { assert(name&&stack==16384&&fn&&arg); return alloc_fail?NULL:&thread; }
static void furi_thread_start(FuriThread* t) { assert(t==&thread); ++thread_started; }
static void furi_thread_join(FuriThread* t) { assert(t==&thread); ++thread_joined; }
static void furi_thread_free(FuriThread* t) { assert(t==&thread); ++thread_freed; }
bool wardriver_config_load(WardriverConfig* config) { *config=saved_gps; return true; }
void wardriver_gps_start(WardriverGps* gps,const WardriverConfig* config) { ++gps_started; gps->installed=config->gps_mode!=0; strcpy(gps->status,gps->installed?"listening":"OFF"); }
void wardriver_gps_stop(WardriverGps* gps) { ++gps_stopped; gps->installed=false; }
void wardriver_nmea_init(WardriverNmea* nmea) { ++nmea_cleared; memset(nmea,0,sizeof(*nmea)); }
bool wardriver_wdg_credentials_load(WardWigleCredentials* c) { if(have_key) { memset(c->token,'a',64); c->token[64]=0; } return have_key; }
void wardriver_wigle_erase(void* data,size_t length) { memset(data,0,length); }
bool wdg_cache_load(unsigned slot,WdgMapSavedArea* out) { assert(slot<4); if(load_ok) *out=*loaded; return load_ok; }
bool wdg_cache_save(unsigned slot,const WdgMapSavedArea* out) { assert(slot<4&&out); ++save_calls; if(during_save) during_save(save_context); return save_ok; }
bool wdg_basemap_bounds(const WdgMapBounds* view,uint8_t zoom,WdgMapBounds* out) { if(zoom<WDG_BASE_MIN_ZOOM||!wdg_map_bounds_valid(view)) return false; *out=*view; return true; }
bool wdg_basemap_fetch(WdgBasemapFetch* f) {
    ++roads_calls; f->status=roads_ok?WdgMapClientOk:WdgMapClientNetwork; f->http_status=roads_ok?200:503; f->retry_after_s=0;
    strcpy(f->message,roads_ok?(roads_partial?"Partial streets: server timeout":"Basemap downloaded"):"OSM connect error");
    if(roads_ok) { memset(f->map,0,sizeof(*f->map)); f->map->bounds=f->bounds; f->map->downloaded_at=f->downloaded_at; f->map->feature_count=roads_count; f->map->partial=roads_partial; }
    return roads_ok;
}
bool wdg_map_fetch_territories(WdgMapFetch* f) {
    ++territory_calls; f->status=territories_ok?WdgMapClientOk:WdgMapClientAuth; f->http_status=territories_ok?200:401; f->retry_after_s=0;
    strcpy(f->message,territories_ok?"Map updated":"WDGWars API key rejected");
    if(territories_ok) { memset(f->map,0,sizeof(*f->map)); f->map->bounds=f->bounds; f->map->downloaded_at=f->downloaded_at; f->map->zoom=f->zoom; f->map->cell_count=territory_count; f->map->hull_count=hull_count; f->map->cells_skipped=f->zoom<6; }
    return territories_ok;
}
bool wdg_map_fetch_account(WdgMapFetch* f) { ++account_calls; f->status=account_ok?WdgMapClientOk:WdgMapClientAuth; strcpy(f->message,account_ok?"Account updated":"API key rejected"); if(account_ok) strcpy(f->account->username,"Example"); return account_ok; }
#include "app_helpers.inc"
static WdgMapApp* setup(void) {
    WdgMapApp* a=calloc(1,sizeof(*a)); assert(a); a->active=calloc(1,sizeof(*a->active)); a->scratch=calloc(1,sizeof(*a->scratch)); a->mutex=calloc(1,sizeof(*a->mutex)); assert(a->active&&a->scratch&&a->mutex);
    a->target_zoom=a->view.zoom=a->active->zoom=15; a->view.center_lat=41.12345; a->view.center_lon=-81.54321; a->auto_maps=true; return a;
}
static void run_job(WdgMapApp* a) { assert(a->busy&&a->worker); assert(worker_main(a)==0&&a->done); finish_job(a); assert(!a->busy&&!a->worker&&!a->mutex->locked); }
static void drop(WdgMapApp* a) { assert(!a->busy); free(a->active); free(a->scratch); free(a->mutex); free(a); }
static void reset_mocks(void) { clock_ms=10000; connected=uart_available=have_key=save_ok=roads_ok=territories_ok=account_ok=true; alloc_fail=load_ok=roads_partial=false; gps_started=gps_stopped=nmea_cleared=0; saved_gps=(WardriverConfig){.gps_rx=-1,.gps_tx=-1,.gps_uart=2,.gps_baud=9600}; during_save=NULL; save_context=NULL; roads_count=3; territory_count=2; hull_count=0; roads_calls=territory_calls=account_calls=save_calls=thread_started=thread_joined=thread_freed=0; memset(loaded,0,sizeof(*loaded)); loaded->zoom=15; }
static void input_tests(void) {
    WdgMapApp* a=setup(); assert(now_ms()==10000); coordinates_begin(a); assert(strlen(a->coordinate_text)==21&&!strcmp(a->coordinate_text,"+041.12345 -081.54321"));
    for(unsigned i=0;i<18;++i) coordinate_input(a,0,true); assert(a->center_set&&!a->view.follow&&a->page==PageMap&&fabs(a->view.center_lat-41.12345)<1e-8);
    coordinates_begin(a); coordinate_input(a,1,false); assert(a->coordinate_text[0]=='-');
    strcpy(a->coordinate_text,"+999.00000 +000.00000"); a->coordinate_digit=17; coordinate_input(a,0,true); assert(fabs(a->view.center_lat-41.12345)<1e-8&&strstr(a->notice,"outside"));
    a->gps.installed=true; memset(&a->gps.parser,0x6a,sizeof(a->gps.parser)); a->fix.has_fix=true; gps_toggle(a); assert(!a->gps.installed&&!a->fix.has_fix&&gps_stopped==1&&nmea_cleared==1&&!a->gps_config.gps_mode);
#if WDG_MAP_PLUS_PROFILE
    uart_available=false; a->gps_config.gps_rx=-1; gps_toggle(a);
    assert(!gps_started&&a->gps_config.gps_rx==-1&&strstr(a->notice,"Wardriver Plus"));
    a->gps_config=(WardriverConfig){.gps_rx=16,.gps_tx=-1,.gps_baud=38400,.gps_uart=1,.isolated_uart_pins=false};
    gps_toggle(a); assert(!gps_started&&!a->gps.installed);
    a->gps_config.isolated_uart_pins=true; gps_toggle(a);
    assert(gps_started==1&&a->gps.installed&&a->gps_config.gps_rx==16&&a->gps_config.gps_tx==-1&&a->gps_config.gps_baud==38400);
    gps_toggle(a); assert(!a->gps.installed); uart_available=true;
#else
    uart_available=false; gps_toggle(a); assert(!gps_started); uart_available=true; a->gps_config.gps_rx=-1; gps_toggle(a); assert(gps_started==1&&a->gps_config.gps_rx==44&&a->gps_config.gps_tx==-1&&a->gps_config.isolated_uart_pins);
    int custom_rx=WDG_MAP_UART_I2C_PROFILE?16:18;
    gps_toggle(a); a->gps_config=(WardriverConfig){.gps_rx=custom_rx,.gps_tx=-1,.gps_baud=38400,.gps_uart=1,.isolated_uart_pins=true}; gps_toggle(a); assert(a->gps_config.gps_baud==38400&&a->gps_config.gps_rx==custom_rx);
#endif
    InputEvent back={InputKeyBack,InputTypeShort},turn={InputKeyDown,InputTypePress},click={InputKeyOk,InputTypeShort};
    a->page=PageMap; a->pan=true; handle_input(a,&back); assert(!a->pan&&!a->exit_requested);
    a->page=PageActions; a->selected=0; handle_input(a,&turn); assert(a->selected==13); handle_input(a,&click); assert(a->exit_requested); a->exit_requested=false;
    a->page=PageMap; a->target_zoom=20; turn.key=InputKeyUp; handle_input(a,&turn); assert(a->target_zoom==20); a->target_zoom=0; turn.key=InputKeyDown; handle_input(a,&turn); assert(a->target_zoom==0);
    action(a,1); assert(a->target_zoom==0&&a->view.zoom==0&&!a->view.follow); WdgMapBounds world; assert(view_bounds(&a->view,&world)&&world.west==-180&&world.east==180&&world.south< -85&&world.north>85);
    a->fix.has_fix=true; a->fix.latitude=52.5; a->fix.longitude=13.4; action(a,5); assert(a->target_zoom==15&&a->view.follow&&a->center_set&&a->view.center_lat==52.5);
    a->pan=true; a->view.follow=false; double before=a->view.center_lat; turn.key=InputKeyUp; handle_input(a,&turn); assert(a->view.center_lat>before&&!a->view.follow);
    a->center_set=false; assert(!start_job(a,JobDownload,0)&&!thread_started); a->center_set=true; connected=false; assert(!start_job(a,JobDownload,0)); connected=true;
    alloc_fail=true; assert(!start_job(a,JobDownload,0)&&!a->busy&&!a->worker&&!thread_started); alloc_fail=false;
    assert(start_job(a,JobDownload,1)); a->page=PageMap; handle_input(a,&back); assert(a->cancel); run_job(a); assert(!a->auto_maps&&!a->territories_pending);
    drop(a);
}
static void gps_profile_startup(void) {
    WdgMapApp* a=setup(); gps_startup(a); assert(!a->gps.installed&&a->gps_config.gps_rx==-1&&!a->gps_config.gps_mode);
    saved_gps=(WardriverConfig){.gps_mode=1,.gps_rx=16,.gps_tx=-1,.gps_uart=2,.gps_baud=9600,.isolated_uart_pins=true};
    uart_available=false; gps_startup(a);
#if WDG_MAP_PLUS_PROFILE
    assert(a->gps.installed&&a->gps_config.gps_mode==1); /* Plus allows explicitly isolated spare pins. */
#else
    assert(!a->gps.installed&&!a->gps_config.gps_mode); /* Existing profile keeps the UART availability gate. */
#endif
    assert(a->gps_config.gps_rx==16&&saved_gps.gps_mode==1);
#if !WDG_MAP_PLUS_PROFILE
    uart_available=true;
    saved_gps=(WardriverConfig){.gps_mode=2,.gps_rx=44,.gps_tx=43,.gps_uart=2,.gps_baud=9600,.isolated_uart_pins=true};
    gps_startup(a); assert(a->gps.installed && a->gps_config.gps_rx==44 && a->gps_config.gps_tx==43);
    assert(saved_gps.gps_rx==44 && saved_gps.gps_tx==43); /* No stored setting rewrite. */
#endif
    drop(a);
}
static void partial_street_result(void) {
    WdgMapApp* a=setup(); a->center_set=true; roads_partial=true;
    assert(start_job(a,JobDownload,0)); run_job(a);
    assert(a->active->basemap.partial&&strstr(a->street_result,"server timeout")&&strstr(a->street_result,"3 paths"));
    drop(a);
}
static void cold_start_and_cache(void) {
    WdgMapApp* a=setup(); load_ok=true; loaded->flags=WDG_CACHE_ACCOUNT|WDG_CACHE_CENTER; loaded->center_lat=10; loaded->center_lon=20;
    assert(start_job(a,JobLoad,0)); run_job(a); assert(!a->center_set&&!has_geometry(a->active)); assert(!strstr(a->notice,"Opened offline"));
    unsigned before=thread_started; automatic_update(a); assert(thread_started==before); /* GPS required for local street data. */
    a->fix.has_fix=true; a->view.center_lat=51.5; a->view.center_lon=0; a->view.follow=a->center_set=true;
    automatic_update(a); assert(a->busy&&a->job==JobDownload&&a->automatic_job); run_job(a);
    assert(roads_calls==1&&!territory_calls&&a->active->basemap.feature_count==3&&a->page==PageMap);
    automatic_update(a); assert(a->busy&&a->job==JobTerritories); run_job(a);
    assert(territory_calls==1&&a->active->territories.cell_count==2);
    before=thread_started; for(unsigned i=0;i<10;++i) { clock_ms+=61000; automatic_update(a); } assert(thread_started==before); /* Same view never hammers API. */
    a->view.center_lon+=1; automatic_update(a); assert(a->busy&&a->job==JobDownload); run_job(a); automatic_update(a); if(a->busy) run_job(a);
    load_ok=false; a->view.center_lat=60; a->view.center_lon=30; a->view.follow=true;
    assert(start_job(a,JobLoad,2)); run_job(a); assert(a->center_set&&a->view.center_lat==60&&a->view.center_lon==30&&a->view.follow);
    drop(a);
}
static void startup_cache_priority(void) {
    WdgMapApp* a=setup(); load_ok=true; loaded->flags=WDG_CACHE_BASEMAP|WDG_CACHE_CENTER; loaded->basemap.feature_count=2; loaded->center_lat=10; loaded->center_lon=20;
    a->startup_load=true; a->gps.installed=true; a->fix.has_fix=true; a->center_set=true; a->view.center_lat=51.5; a->view.center_lon=0; a->view.follow=true;
    assert(start_job(a,JobLoad,0)); run_job(a); assert(a->view.center_lat==51.5&&a->view.center_lon==0&&a->view.follow&&!a->startup_load);
    a->startup_load=true; a->fix.has_fix=false; assert(start_job(a,JobLoad,0)); run_job(a); assert(a->view.center_lat==10&&a->view.follow); /* Ready for a later first fix. */
    a->fix.has_fix=true; a->view.center_lat=60; a->view.center_lon=30; assert(start_job(a,JobLoad,0)); run_job(a); assert(a->view.center_lat==10&&a->view.center_lon==20&&!a->view.follow); /* Manual area choice wins. */
    loaded->zoom=0; loaded->flags=WDG_CACHE_TERRITORIES|WDG_CACHE_CENTER; loaded->territories.hull_count=1; loaded->basemap.feature_count=0;
    assert(start_job(a,JobLoad,0)); run_job(a); assert(a->target_zoom==0&&a->view.zoom==0&&a->center_set);
    drop(a);
}
static void move_during_fetch(void) {
    WdgMapApp* a=setup(); a->center_set=true; automatic_update(a); assert(a->busy&&a->job==JobDownload);
    a->view.center_lon+=1; run_job(a); assert(a->territories_pending); automatic_update(a); assert(a->busy&&a->job==JobTerritories); run_job(a);
    clock_ms+=61000; automatic_update(a); assert(a->busy&&a->job==JobDownload); /* Newly moved view must still acquire roads. */
    run_job(a); automatic_update(a); if(a->busy) run_job(a); drop(a);
}
static void explicit_update_and_cached_followup(void) {
    WdgMapApp* a=setup(); a->center_set=true; a->auto_maps=false;
    WdgMapBounds b; assert(view_bounds(&a->view,&b));
    a->active->flags=WDG_CACHE_TERRITORIES; a->active->territories.bounds=b; a->active->territories.cell_count=2;
    assert(territories_cover(a)); action(a,0); assert(a->busy&&a->job==JobDownload&&!a->automatic_job);
    run_job(a); assert(a->territories_pending&&!a->pending_automatic); automatic_update(a);
    assert(a->busy&&a->job==JobTerritories); /* Explicit refresh must update existing covered WDG data, even when auto loading is OFF. */
    run_job(a); assert(roads_calls==1&&territory_calls==1&&!a->auto_maps);
    drop(a);
    reset_mocks(); a=setup(); a->center_set=true;
    assert(view_bounds(&a->view,&b)); a->active->flags=WDG_CACHE_TERRITORIES; a->active->territories.bounds=b; a->active->territories.cell_count=2;
    automatic_update(a); assert(a->busy&&a->job==JobDownload); run_job(a); assert(a->territories_pending&&a->pending_automatic);
    automatic_update(a); assert(!a->busy&&!a->territories_pending&&!territory_calls); /* Automatic road refresh reuses covered WDG data. */
    drop(a);
}
static void dateline_overview(void) {
    WdgMapApp* a=setup(); a->center_set=true; a->view.center_lat=22; a->view.center_lon=79; a->view.zoom=a->target_zoom=1;
    WdgMapBounds b; assert(view_bounds(&a->view,&b)); assert(b.west==-180&&b.east==180&&b.south< -85&&b.north>85);
    territory_count=0; hull_count=1; automatic_update(a); assert(a->busy&&a->job==JobTerritories); run_job(a);
    assert(!roads_calls&&territory_calls==1&&!a->territories_failed&&a->active->territories.cells_skipped);
    drop(a);
}
static void back_during_save(void* context) { WdgMapApp* a=context; InputEvent e={InputKeyBack,InputTypeShort}; assert(a->saving); handle_input(a,&e); assert(!a->cancel); }
static void save_and_empty(void) {
    WdgMapApp* a=setup(); a->center_set=true; a->active->flags=WDG_CACHE_ACCOUNT|WDG_CACHE_CENTER;
    assert(start_job(a,JobSave,0)); run_job(a); assert(!save_calls&&!has_geometry(a->active)&&strstr(a->notice,"No downloaded map"));
    a->page=PageMap; roads_count=0; have_key=false; assert(start_job(a,JobDownload,0)); run_job(a); assert(!a->active->basemap.feature_count&&!has_geometry(a->active));
    assert(!strstr(a->notice,"saved Area")&&!strstr(a->street_result,"0 paths saved"));
    unsigned before=thread_started; clock_ms+=61000; automatic_update(a); assert(thread_started==before);
    a->page=PageMap; a->auto_attempted=false; roads_ok=false; a->view.center_lon+=1; automatic_update(a); assert(a->busy); run_job(a); assert(a->streets_failed&&strstr(a->street_result,"error"));
    before=thread_started; clock_ms+=61000; automatic_update(a); assert(thread_started==before);
    a->page=PageMap; a->active->flags|=WDG_CACHE_BASEMAP; a->active->basemap.feature_count=1; during_save=back_during_save; save_context=a;
    assert(start_job(a,JobSave,0)); run_job(a); assert(!a->cancel&&a->page==PageResult); during_save=NULL;
    drop(a);
}
static void globe_and_throttle(void) {
    WdgMapApp* a=setup(); action(a,1); assert(!a->fix.has_fix); territory_count=0; hull_count=1;
    automatic_update(a); assert(a->busy&&a->job==JobTerritories); run_job(a); assert(!roads_calls&&territory_calls==1&&a->active->territories.hull_count==1&&a->active->territories.zoom==0);
    unsigned before=thread_started; clock_ms+=61000; a->view.center_lat=70; a->view.center_lon=170; automatic_update(a); assert(thread_started==before); /* World view doesn't change when camera center changes. */
    a->view.zoom=a->target_zoom=15; assert(!territories_cover(a));
    a->last_input_ms=clock_ms; automatic_update(a); assert(!a->busy); clock_ms+=1801; automatic_update(a); assert(a->busy&&a->job==JobDownload); run_job(a); if(a->territories_pending) { automatic_update(a); if(a->busy) run_job(a); }
    clock_ms+=61000; backoff(a,180,true); assert(!start_job(a,JobAccount,0)); assert(!start_job(a,JobTerritories,0));
    assert(start_job(a,JobDownload,0)); run_job(a); /* WDG cooldown does not block the OSM host. */
    drop(a);
    reset_mocks(); a=setup(); a->center_set=true; clock_ms=6000000000ULL; assert(start_job(a,JobAccount,0)); run_job(a); assert(account_calls==1); drop(a);
}
int main(void) {
    loaded=calloc(1,sizeof(*loaded)); assert(loaded);
    reset_mocks(); input_tests(); reset_mocks(); gps_profile_startup(); reset_mocks(); partial_street_result(); reset_mocks(); cold_start_and_cache(); reset_mocks(); startup_cache_priority(); reset_mocks(); move_during_fetch(); reset_mocks(); explicit_update_and_cached_followup(); reset_mocks(); dateline_overview(); reset_mocks(); save_and_empty(); reset_mocks(); globe_and_throttle();
    free(loaded); puts("WDG map app: first-load, GPS, offline cache, auto-download, globe, cancellation and lifecycle checks passed"); return 0;
}
