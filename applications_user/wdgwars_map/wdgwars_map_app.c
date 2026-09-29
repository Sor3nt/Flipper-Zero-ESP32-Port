#include "wdgwars_map_render.h"
#include "wdgwars_map_cache.h"
#include "wdgwars_map_client.h"
#include "wdgwars_map_json.h"
#include "wdgwars_map_profile.h"
#if WDG_MAP_PLUS_PROFILE
#include "../wardriver_plus_i2c/wardriver_gps.h"
#else
#include "../wardriver/wardriver_gps.h"
#endif
#include "../wardriver/wardriver_wigle.h"
#include <furi.h>
#include <furi_hal_display.h>
#include <furi_hal_external_uart.h>
#include <furi_hal_rtc.h>
#include <gui/gui.h>
#include <input/input.h>
#include <wifi/wlan_hal.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { JobLoad,JobDownload,JobRoads,JobAccount,JobSave,JobTerritories } MapJob;
typedef enum { PageMap,PageActions,PageAreas,PageAccount,PageDetails,PageCoordinates,PageAbout,PageResult } MapPage;
typedef struct {
    WdgMapSavedArea *active,*scratch;
    WdgMapRenderState view;
    WardriverGps gps;
    WardriverConfig gps_config;
    WardriverGpsData fix;
    FuriMessageQueue* input;
    FuriMutex* mutex;
    FuriThread* worker;
    uint16_t *pixels,*stripe;
    bool busy,done,cancel,commit,saving,exit_requested,pan,center_set,key_present;
    bool request_center_set;
    bool auto_maps,auto_attempted,automatic_job,territories_pending,pending_automatic,startup_load;
    bool streets_failed,territories_failed,job_map_error,road_updated,territory_updated;
    double attempt_lat,attempt_lon,attempt_zoom;
    uint64_t last_input_ms;
    char street_result[96],territory_result[96];
    MapJob job;
    MapPage page;
    unsigned selected,slot,job_slot,layers,coordinate_digit;
    double target_zoom,request_lat,request_lon,request_zoom;
    char coordinate_text[24];
    uint32_t request_epoch;
    uint64_t next_download_ms,next_account_ms;
    char notice[80],worker_message[80];
} WdgMapApp;
static uint64_t uptime_ms(void) { return (uint64_t)(esp_timer_get_time()/1000); }
static uint32_t now_ms(void) { return (uint32_t)uptime_ms(); }
static void* map_alloc(size_t size) {
    void* p=heap_caps_calloc(1,size,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    return p?p:calloc(1,size);
}
static void message(WdgMapApp* a,const char* text) {
    furi_mutex_acquire(a->mutex,FuriWaitForever);
    snprintf(a->worker_message,sizeof(a->worker_message),"%s",text);
    furi_mutex_release(a->mutex);
}
static bool cancelled(void* context) {
    WdgMapApp* a=context; furi_mutex_acquire(a->mutex,FuriWaitForever);
    bool stop=a->cancel; furi_mutex_release(a->mutex); return stop;
}
static uint32_t epoch(WdgMapApp* a) {
    if(a->fix.timestamp_valid && a->fix.timestamp>=1704067200ULL && a->fix.timestamp<=UINT32_MAX) return (uint32_t)a->fix.timestamp;
    uint32_t t=furi_hal_rtc_get_timestamp(); return t>=1704067200U?t:0;
}
static void input_callback(const void* value,void* context) {
    WdgMapApp* a=context; furi_message_queue_put(a->input,value,0);
}
static int32_t worker_main(void* context);
static bool inside(const WdgMapBounds* b,double lat,double lon);
static bool has_geometry(const WdgMapSavedArea* area) {
    return ((area->flags&WDG_CACHE_BASEMAP) && area->basemap.feature_count) ||
        ((area->flags&WDG_CACHE_TERRITORIES) && (area->territories.cell_count || area->territories.hull_count));
}
static bool view_bounds(const WdgMapRenderState* view,WdgMapBounds* bounds) {
    if(view->zoom<0.5) { *bounds=(WdgMapBounds){-85.051128,-180,85.051128,180}; return true; }
    if(!wdgwars_map_viewport_bounds(view,bounds)) return false;
    if(!wdg_map_bounds_valid(bounds) && view->zoom<6) {
        *bounds=(WdgMapBounds){-85.051128,-180,85.051128,180}; return true;
    }
    return wdg_map_bounds_valid(bounds);
}
static bool bounds_cover(const WdgMapBounds* cached,const WdgMapBounds* wanted) {
    const double margin=0.000002;
    return wdg_map_bounds_valid(cached) && wanted->south>=cached->south-margin && wanted->north<=cached->north+margin &&
        wanted->west>=cached->west-margin && wanted->east<=cached->east+margin;
}
static bool streets_cover(WdgMapApp* a) {
    if(!(a->active->flags&WDG_CACHE_BASEMAP)) return false;
    WdgMapRenderState request=a->view; request.zoom=a->target_zoom;
    WdgMapBounds visible,actual;
    return view_bounds(&request,&visible) && wdg_basemap_bounds(&visible,(uint8_t)a->target_zoom,&actual) &&
        bounds_cover(&a->active->basemap.bounds,&actual);
}
static bool territories_cover(WdgMapApp* a) {
    if(!(a->active->flags&WDG_CACHE_TERRITORIES)) return false;
    WdgMapRenderState request=a->view; request.zoom=a->target_zoom; WdgMapBounds bounds;
    return view_bounds(&request,&bounds) && bounds_cover(&a->active->territories.bounds,&bounds) &&
        (a->target_zoom<6 || !a->active->territories.cells_skipped);
}

static bool start_job(WdgMapApp* a,MapJob job,unsigned slot) {
    if(a->busy) { snprintf(a->notice,sizeof(a->notice),"Download busy; Back cancels"); return false; }
    uint64_t now=uptime_ms();
    if((job==JobDownload || job==JobRoads) && a->next_download_ms>now) {
        snprintf(a->notice,sizeof(a->notice),"Please wait %lus before refresh",(unsigned long)((a->next_download_ms-now+999)/1000)); return false;
    }
    if((job==JobAccount || job==JobTerritories) && a->next_account_ms>now) {
        snprintf(a->notice,sizeof(a->notice),"Account refresh cooling down"); return false;
    }
    if((job==JobDownload || job==JobRoads || job==JobTerritories) && !a->center_set && a->target_zoom>=0.5) {
        snprintf(a->notice,sizeof(a->notice),"Set coordinates or enable GPS first"); return false;
    }
    if((job==JobDownload || job==JobRoads || job==JobAccount || job==JobTerritories) && !wlan_hal_is_connected()) {
        snprintf(a->notice,sizeof(a->notice),"Connect using firmware Wi-Fi first"); return false;
    }
    a->job=job; a->job_slot=slot; a->request_lat=a->view.center_lat; a->request_lon=a->view.center_lon;
    a->request_zoom=a->target_zoom; a->request_epoch=epoch(a); a->request_center_set=a->center_set;
    if(job==JobLoad) memset(a->scratch,0,sizeof(*a->scratch));
    else { *a->scratch=*a->active; a->scratch->center_lat=a->request_lat; a->scratch->center_lon=a->request_lon; a->scratch->zoom=a->request_zoom; if(a->center_set) a->scratch->flags|=WDG_CACHE_CENTER; }
    a->automatic_job=false; a->job_map_error=false; a->road_updated=false; a->territory_updated=false;
    a->cancel=false; a->saving=false; a->done=false; a->commit=false; a->busy=true;
    snprintf(a->worker_message,sizeof(a->worker_message),"%s",job==JobLoad?"Opening saved area...":job==JobSave?"Saving area...":"Connecting...");
    a->worker=furi_thread_alloc_ex("WDGMapIO",16384,worker_main,a);
    if(!a->worker) { a->busy=false; snprintf(a->notice,sizeof(a->notice),"Not enough worker memory"); return false; }
    if(job==JobDownload || job==JobRoads || job==JobTerritories) {
        if(job!=JobTerritories) a->next_download_ms=now+60000;
        a->auto_attempted=true; a->attempt_lat=a->view.center_lat; a->attempt_lon=a->view.center_lon; a->attempt_zoom=a->target_zoom;
    }
    if(job==JobAccount || job==JobTerritories) a->next_account_ms=now+60000;
    furi_thread_start(a->worker); return true;
}
static void finish_job(WdgMapApp* a) {
    if(!a->busy) return;
    furi_mutex_acquire(a->mutex,FuriWaitForever); bool done=a->done; furi_mutex_release(a->mutex);
    if(!done) return;
    furi_thread_join(a->worker); furi_thread_free(a->worker); a->worker=NULL;
    if(a->commit && !a->cancel) {
        WdgMapSavedArea* old=a->active; a->active=a->scratch; a->scratch=old;
        if(a->job==JobLoad && has_geometry(a->active) && !(a->startup_load && a->fix.has_fix)) {
            a->view.center_lat=a->active->center_lat; a->view.center_lon=a->active->center_lon;
            a->target_zoom=a->view.zoom=a->active->zoom;
            a->center_set=(a->active->flags&WDG_CACHE_CENTER)!=0; a->view.follow=a->startup_load && a->gps.installed;
        }
        a->slot=a->job_slot;
    }
    snprintf(a->notice,sizeof(a->notice),"%s",a->worker_message); a->busy=false;
    if(a->job==JobLoad) { a->auto_attempted=false; a->startup_load=false; }
    if(a->job==JobDownload && a->key_present && !a->cancel) { a->territories_pending=true; a->pending_automatic=a->automatic_job; }
    if(a->job==JobDownload || a->job==JobRoads) a->streets_failed=a->job_map_error;
    if(a->job==JobTerritories) a->territories_failed=a->job_map_error;
    if(a->cancel) { a->auto_maps=false; a->territories_pending=false; }
    if(a->job==JobSave || (!a->automatic_job && a->job_map_error)) a->page=PageResult;
}
static bool view_changed(WdgMapApp* a) {
    if(!a->auto_attempted) return true;
    if(fabs(a->target_zoom-a->attempt_zoom)>=0.5) return true;
    if(a->target_zoom<0.5) return false;
    WdgMapBounds b; WdgMapRenderState view=a->view; view.zoom=a->target_zoom;
    if(!view_bounds(&view,&b)) return false;
    return fabs(a->view.center_lat-a->attempt_lat)>(b.north-b.south)*0.2 ||
        fabs(a->view.center_lon-a->attempt_lon)>(b.east-b.west)*0.2;
}
static void automatic_update(WdgMapApp* a) {
    if(a->busy || a->exit_requested || a->page!=PageMap || !wlan_hal_is_connected() ||
        (!a->center_set && a->target_zoom>=0.5) || uptime_ms()-a->last_input_ms<1800) return;
    if(a->territories_pending) {
        if(a->pending_automatic && territories_cover(a)) a->territories_pending=false;
        else if(!a->pending_automatic || a->auto_maps) {
            if(uptime_ms()<a->next_account_ms) return;
            a->territories_pending=false;
            /* A follow-up for a moved viewport must not mark its streets as attempted. */
            bool attempted=a->auto_attempted;
            double lat=a->attempt_lat,lon=a->attempt_lon,zoom=a->attempt_zoom;
            if(start_job(a,JobTerritories,a->slot)) a->automatic_job=true;
            a->auto_attempted=attempted; a->attempt_lat=lat; a->attempt_lon=lon; a->attempt_zoom=zoom;
            return;
        }
    }
    if(!a->auto_maps) return;
    if(!view_changed(a)) return;
    bool roads=a->target_zoom>=WDG_BASE_MIN_ZOOM && !streets_cover(a);
    bool territories=!territories_cover(a);
    if(roads || territories) {
        if(uptime_ms()<(roads?a->next_download_ms:a->next_account_ms)) return;
        if(start_job(a,roads?JobDownload:JobTerritories,a->slot)) a->automatic_job=true;
    }
}
static void gps_startup(WdgMapApp* a) {
    wardriver_config_load(&a->gps_config);
#if !WDG_MAP_PLUS_PROFILE
    if(!furi_hal_external_uart_available()) a->gps_config.gps_mode=0;
#endif
    /* Plus reuses the guarded Wardriver Plus driver. Saved non-Plus RX44/TX43
     * and I2C pins are rejected there before any UART or GPIO operation. */
    wardriver_gps_start(&a->gps,&a->gps_config);
#if WDG_MAP_PLUS_PROFILE
    if(a->gps_config.gps_mode && !a->gps.installed)
        snprintf(a->notice,sizeof(a->notice),"GPS %s - configure Wardriver Plus pins",a->gps.status);
#endif
}
static void gps_toggle(WdgMapApp* a) {
    if(a->gps.installed) { wardriver_gps_stop(&a->gps); wardriver_nmea_init(&a->gps.parser); a->gps_config.gps_mode=0; memset(&a->fix,0,sizeof(a->fix)); strcpy(a->notice,"GPS off"); return; }
#if WDG_MAP_PLUS_PROFILE
    if(a->gps_config.gps_rx<0 || !a->gps_config.isolated_uart_pins) {
        strcpy(a->notice,"Set isolated GPS pins in Wardriver Plus first"); return;
    }
    a->gps_config.gps_mode=2;
#else
    if(!furi_hal_external_uart_available()) { strcpy(a->notice,"GPS requires the non-NRF UART build"); return; }
    if(a->gps_config.gps_rx<0 || !a->gps_config.isolated_uart_pins) {
        a->gps_config=(WardriverConfig){.gps_mode=2,.gps_uart=2,.gps_baud=9600,.gps_rx=44,.gps_tx=-1,.isolated_uart_pins=true};
    } else a->gps_config.gps_mode=2;
#endif
    wardriver_gps_start(&a->gps,&a->gps_config);
    snprintf(a->notice,sizeof(a->notice),"GPS %s / RX%d / %lu baud",a->gps.status,(int)a->gps_config.gps_rx,(unsigned long)a->gps_config.gps_baud);
}
static const WdgMapCell* nearest_cell(WdgMapApp* a) {
    if(!(a->active->flags&WDG_CACHE_TERRITORIES)) return NULL;
    const WdgMapCell* best=NULL; float distance=70*70;
    for(unsigned i=0;i<a->active->territories.cell_count;++i) {
        const WdgMapCell* c=&a->active->territories.cells[i]; float x,y;
        if(wdgwars_map_geo_to_screen(&a->view,c->lat_e6/1e6,c->lon_e6/1e6,&x,&y)) {
            float d=(x-160)*(x-160)+(y-85)*(y-85); if(d<distance) { distance=d; best=c; }
        }
    }
    return best;
}
static const WdgMapHull* hull_at_center(WdgMapApp* a) {
    if(!(a->active->flags&WDG_CACHE_TERRITORIES)) return NULL;
    WdgMapSnapshot* map=&a->active->territories;
    for(unsigned k=0;k<map->hull_count;++k) {
        const WdgMapHull* h=&map->hulls[k]; bool hit=false;
        if(h->point_count<3 || (unsigned)h->first_point+h->point_count>map->point_count) continue;
        for(unsigned i=0,j=h->point_count-1;i<h->point_count;j=i++) {
            const WdgMapPoint *p=&map->points[h->first_point+i],*q=&map->points[h->first_point+j];
            float x,y,xx,yy;
            if(!wdgwars_map_geo_to_screen(&a->view,p->lat_e6/1e6,p->lon_e6/1e6,&x,&y) ||
               !wdgwars_map_geo_to_screen(&a->view,q->lat_e6/1e6,q->lon_e6/1e6,&xx,&yy)) continue;
            if((y>85)!=(yy>85) && 160<(xx-x)*(85-y)/(yy-y)+x) hit=!hit;
        }
        if(hit) return h;
    }
    return NULL;
}
static const char* gang_name(WdgMapApp* a,uint32_t id) {
    for(unsigned i=0;i<a->active->territories.gang_count;++i) if(a->active->territories.gangs[i].id==id) return a->active->territories.gangs[i].name;
    return id?"Gang name unavailable":"No gang reported";
}
static void coordinates_begin(WdgMapApp* a) {
    snprintf(a->coordinate_text,sizeof(a->coordinate_text),"%+010.5f %+010.5f",a->view.center_lat,a->view.center_lon);
    a->coordinate_digit=0; a->page=PageCoordinates;
}
static void coordinate_input(WdgMapApp* a,int step,bool click) {
    /* Two fixed-width signed coordinates. Turn edits; click advances; Back cancels. */
    static const unsigned positions[]={0,1,2,3,5,6,7,8,9,11,12,13,14,16,17,18,19,20};
    if(click) {
        if(++a->coordinate_digit>=sizeof(positions)/sizeof(positions[0])) {
            double lat=strtod(a->coordinate_text,NULL),lon=strtod(a->coordinate_text+11,NULL);
            if(lat < -85.05112878 || lat > 85.05112878 || lon < -180 || lon > 180) strcpy(a->notice,"Coordinates outside supported range");
            else { a->view.center_lat=lat; a->view.center_lon=lon; a->center_set=true; a->view.follow=false; a->target_zoom=a->view.zoom=15; strcpy(a->notice,"Center set; choose Refresh area"); }
            a->page=PageMap;
        }
    } else {
        char* c=&a->coordinate_text[positions[a->coordinate_digit]];
        if(*c=='+' || *c=='-') *c=*c=='+'?'-':'+'; else *c=(char)('0'+(*c-'0'+step+10)%10);
    }
}
static void action(WdgMapApp* a,unsigned i) {
    a->page=PageMap;
    switch(i) {
    case 0: start_job(a,a->target_zoom>=WDG_BASE_MIN_ZOOM?JobDownload:JobTerritories,a->slot); break;
    case 1:
        a->target_zoom=a->view.zoom=0; a->view.follow=false; a->center_set=true; a->auto_attempted=false;
        strcpy(a->notice,"Whole world / offline coastlines"); break;
    case 2: a->page=PageAccount; break;
    case 3: a->page=PageDetails; break;
    case 4: a->pan=!a->pan; a->view.follow=false; strcpy(a->notice,a->pan?"Pan: turn N/S; hold+turn E/W":"Turn to zoom"); break;
    case 5:
        if(a->fix.has_fix) {
            a->view.center_lat=a->fix.latitude; a->view.center_lon=a->fix.longitude;
            a->view.follow=true; a->center_set=true; a->target_zoom=a->view.zoom=15; a->auto_attempted=false;
            strcpy(a->notice,"Following GPS");
        } else strcpy(a->notice,"Waiting for GPS fix");
        break;
    case 6: gps_toggle(a); break;
    case 7: a->page=PageAreas; a->selected=0; break;
    case 8: coordinates_begin(a); break;
    case 9: a->layers=(a->layers+1)%3; strcpy(a->notice,a->layers==0?"All map layers":a->layers==1?"Territories only":"Streets only"); break;
    case 10: a->auto_maps=!a->auto_maps; a->auto_attempted=false; strcpy(a->notice,a->auto_maps?"Online maps enabled":"Offline mode; downloads paused"); break;
    case 11: a->page=PageResult; break;
    case 12: a->page=PageAbout; break;
    case 13: a->exit_requested=true; break;
    }
}
static void handle_input(WdgMapApp* a,const InputEvent* e) {
    a->last_input_ms=uptime_ms();
    if(e->key==InputKeyBack && (e->type==InputTypeShort || e->type==InputTypeLong)) {
        if(a->page!=PageMap) { a->page=PageMap; return; }
        if(a->busy) {
            furi_mutex_acquire(a->mutex,FuriWaitForever);
            bool finishing=a->saving || a->done;
            if(!finishing) a->cancel=true;
            furi_mutex_release(a->mutex);
            strcpy(a->notice,finishing?"Finishing SD save...":"Cancelling download..."); return;
        }
        if(a->pan) { a->pan=false; return; }
        a->exit_requested=true; return;
    }
    bool click=e->key==InputKeyOk && e->type==InputTypeShort;
    bool turn=(e->type==InputTypePress || e->type==InputTypeRepeat) && e->key<=InputKeyLeft;
    if(!click && !turn) return;
    int step=(e->key==InputKeyUp || e->key==InputKeyRight)?1:-1;
    if(a->page==PageCoordinates) { coordinate_input(a,step,click); return; }
    if(a->page==PageActions || a->page==PageAreas) {
        unsigned count=a->page==PageActions?14:8;
        if(turn) a->selected=(a->selected+count+(step>0?1:-1))%count;
        else if(a->page==PageActions) action(a,a->selected);
        else { unsigned slot=a->selected%4; MapJob job=a->selected<4?JobLoad:JobSave; a->page=PageMap; start_job(a,job,slot); }
        return;
    }
    if(a->page!=PageMap) {
        if(click && a->page==PageAccount) start_job(a,JobAccount,a->slot);
        else if(click) a->page=PageMap;
        return;
    }
    if(click) {
        if(a->view.backdrop_retry && !a->busy) {
            start_job(a,a->target_zoom>=WDG_BASE_MIN_ZOOM?JobDownload:JobTerritories,a->slot);
        } else { a->page=PageActions; a->selected=0; }
        return;
    }
    if(a->pan) {
        float x=160,y=85;
        if(e->key==InputKeyLeft || e->key==InputKeyRight) x+=step*24; else y-=step*24;
        wdgwars_map_screen_to_geo(&a->view,x,y,&a->view.center_lat,&a->view.center_lon);
        a->center_set=true; a->view.follow=false;
    } else {
        a->target_zoom+=step*(a->target_zoom<=2?1.0:0.5);
        if(a->target_zoom<0) a->target_zoom=0;
        if(a->target_zoom<1) a->target_zoom=a->target_zoom<0.5?0:1;
        if(a->target_zoom==0) { a->view.zoom=0; a->view.follow=false; a->center_set=true; }
        else if(a->view.zoom<1) a->view.zoom=a->target_zoom;
        if(a->target_zoom>20) a->target_zoom=20;
    }
}
static bool inside(const WdgMapBounds* b,double lat,double lon) {
    return wdg_map_bounds_valid(b) && lat>=b->south && lat<=b->north && lon>=b->west && lon<=b->east;
}
static void render(WdgMapApp* a) {
    char area[24],base_status[48],busy_text[80];
    snprintf(area,sizeof(area),"AREA %u",a->slot+1);
    a->view.now_ms=now_ms(); a->view.online=wlan_hal_is_connected(); a->view.loading=a->busy;
    a->view.has_fix=a->fix.has_fix; a->view.gps_connected=a->fix.connected;
    a->view.gps_lat=a->fix.latitude; a->view.gps_lon=a->fix.longitude; a->view.satellites=a->fix.satellites;
    /* HDOP is dimensionless, not an accuracy radius in metres. */
    a->view.accuracy_known=false; a->view.area_name=area;
    uint32_t time=epoch(a),downloaded=0;
    if(a->layers!=2 && (a->active->flags&WDG_CACHE_TERRITORIES) && inside(&a->active->territories.bounds,a->view.center_lat,a->view.center_lon)) downloaded=a->active->territories.downloaded_at;
    else if(a->layers!=1 && (a->active->flags&WDG_CACHE_BASEMAP) && inside(&a->active->basemap.bounds,a->view.center_lat,a->view.center_lon)) downloaded=a->active->basemap.downloaded_at;
    a->view.cache_age_known=time && downloaded && time>=downloaded;
    a->view.cache_age_seconds=a->view.cache_age_known?time-downloaded:0;
    if(a->busy) { furi_mutex_acquire(a->mutex,FuriWaitForever); snprintf(busy_text,sizeof(busy_text),"%s",a->worker_message); furi_mutex_release(a->mutex); }
    a->view.status=a->busy?busy_text:a->notice;
    const WdgMapBasemap* base=(a->active->flags&WDG_CACHE_BASEMAP) && a->layers!=1?&a->active->basemap:NULL;
    const WdgMapSnapshot* map=(a->active->flags&WDG_CACHE_TERRITORIES) && a->layers!=2?&a->active->territories:NULL;
    const char* coverage="Streets not downloaded";
    if(base) {
        WdgMapBounds viewport;
        if(!inside(&base->bounds,a->view.center_lat,a->view.center_lon)) coverage="Outside saved streets";
        else if(wdgwars_map_viewport_bounds(&a->view,&viewport) &&
            (!inside(&base->bounds,viewport.south,viewport.west) || !inside(&base->bounds,viewport.north,viewport.east))) coverage="Central street area cached";
        else coverage=base->partial?"Partial street detail":"Saved street detail";
    }
    snprintf(base_status,sizeof(base_status),"%s",coverage);
    a->view.basemap_status=base_status;
    a->view.backdrop_status=WdgMapBackdropAuto;
    a->view.backdrop_retry=false; a->view.map_message=NULL; a->view.map_detail=NULL;
    bool overview=a->target_zoom<WDG_BASE_MIN_ZOOM;
    bool road_covered=streets_cover(a);
    if(a->busy && (a->job==JobDownload || a->job==JobRoads || a->job==JobTerritories)) {
        a->view.backdrop_status=WdgMapBackdropLoading;
        a->view.map_message=a->job==JobTerritories?"Loading gang territories":"Loading street map";
        a->view.map_detail=busy_text;
    } else if(!overview && !road_covered) {
        a->view.backdrop_status=!a->view.online || !a->auto_maps?WdgMapBackdropOffline:
            a->streets_failed && !view_changed(a)?WdgMapBackdropError:WdgMapBackdropMissing;
        a->view.map_message=a->view.backdrop_status==WdgMapBackdropError?"Street map download failed":"This area is not downloaded";
        a->view.map_detail=!a->center_set?"Waiting for GPS or map coordinates":
            a->view.backdrop_status==WdgMapBackdropError?a->street_result:"Online loading will fetch this area";
        a->view.backdrop_retry=a->view.online && a->center_set && !a->busy;
    } else if(!overview && base && !base->feature_count) {
        a->view.backdrop_status=WdgMapBackdropEmpty;
        a->view.map_message="No mapped streets returned";
        a->view.map_detail="Try a nearby area or a wider view";
    } else if(overview && a->territories_failed && !view_changed(a)) {
        a->view.backdrop_status=WdgMapBackdropError;
        a->view.map_message="Gang territories unavailable";
        a->view.map_detail=a->territory_result;
        a->view.backdrop_retry=a->view.online && !a->busy;
    }
    const WdgMapCell* nearest=nearest_cell(a);
    const WdgMapHull* hull=nearest?NULL:hull_at_center(a);
    a->view.selected_gang_id=nearest?nearest->gang_id:hull?hull->gang_id:0;
    wdgwars_map_render(a->pixels,map,&a->view,base);
    char rows[14][96]; const char* lines[14]; unsigned n=0; const char* title="",*footer="Back: map"; size_t selected=99;
    if(a->page==PageActions) {
        snprintf(rows[0],70,"Update map + territories / Area %u",a->slot+1);
        snprintf(rows[1],70,"Whole world overview"); strcpy(rows[2],"My account & statistics"); strcpy(rows[3],"Territory near map center");
        snprintf(rows[4],70,"Pan map: %s",a->pan?"ON":"OFF"); strcpy(rows[5],"Center on GPS");
        snprintf(rows[6],70,"GPS: %s",a->gps.installed?"ON (click to stop)":"OFF (click to enable)"); strcpy(rows[7],"Offline areas"); strcpy(rows[8],"Set map coordinates");
        snprintf(rows[9],70,"Layers: %s",a->layers==0?"All":a->layers==1?"Territories":"Streets"); snprintf(rows[10],70,"Online map loading: %s",a->auto_maps?"ON":"OFF");
        strcpy(rows[11],"Last download details"); strcpy(rows[12],"Help & map sources"); strcpy(rows[13],"Close WDGWars Map");
        n=14; selected=a->selected; title="EXPLORE"; footer="Turn: choose   Click: open   Back: map";
    } else if(a->page==PageAreas) {
        for(unsigned i=0;i<4;++i) { snprintf(rows[i],70,"Open saved Area %u",i+1); snprintf(rows[i+4],70,"Copy downloaded map to Area %u",i+1); }
        n=8; selected=a->selected; title="OFFLINE AREAS"; footer="Downloads replace the selected area";
    } else if(a->page==PageAccount) {
        title="MY WDGWARS"; footer="Click: refresh account   Back: map";
        if(a->active->flags&WDG_CACHE_ACCOUNT) {
            WdgMapAccount* u=&a->active->account;
            snprintf(rows[n++],70,"Player: %.57s",u->username); snprintf(rows[n++],70,"Gang: %.59s",u->gang);
            snprintf(rows[n++],70,"Wi-Fi %lu  /  BLE %lu",(unsigned long)u->wifi,(unsigned long)u->ble);
            snprintf(rows[n++],70,"Aircraft %lu  /  Mesh %lu",(unsigned long)u->aircraft,(unsigned long)u->mesh);
            snprintf(rows[n++],70,"Total discoveries: %lu",(unsigned long)u->total);
            if(time && a->active->account_at && time>=a->active->account_at) snprintf(rows[n++],70,"Cached %lu min ago",(unsigned long)((time-a->active->account_at)/60));
            else strcpy(rows[n++],"Saved account; age unknown");
        } else { strcpy(rows[n++],"Click to download your account"); strcpy(rows[n++],"Uses the Wardriver API key on SD"); }
    } else if(a->page==PageDetails) {
        title="TERRITORY DETAILS";
        if(nearest) {
            snprintf(rows[n++],70,"Gang: %.58s",gang_name(a,nearest->gang_id));
            snprintf(rows[n++],70,"Network count: %lu",(unsigned long)nearest->count);
            snprintf(rows[n++],70,"Location: %.5f, %.5f",nearest->lat_e6/1e6,nearest->lon_e6/1e6);
            strcpy(rows[n++],"Ownership from saved WDGWars data");
            if(a->active->territories.cell_anchor==WdgMapCellAnchorUnknown) strcpy(rows[n++],"Cell boundary convention unverified");
        } else if(hull) {
            snprintf(rows[n++],70,"Gang: %.58s",hull->name[0]?hull->name:gang_name(a,hull->gang_id));
            strcpy(rows[n++],"Returned gang outline at map center");
            snprintf(rows[n++],70,"Outline vertices: %u",hull->point_count);
            strcpy(rows[n++],"Gang hulls may overlap other cells");
        } else { strcpy(rows[n++],"No downloaded territory near center"); strcpy(rows[n++],"Pan to a colored marker or refresh"); }
    } else if(a->page==PageCoordinates) {
        static const unsigned positions[]={0,1,2,3,5,6,7,8,9,11,12,13,14,16,17,18,19,20};
        title="SET COORDINATES"; footer="Turn: edit   Click: next   Back: cancel";
        strcpy(rows[n++],"Latitude     Longitude"); strcpy(rows[n++],a->coordinate_text);
        memset(rows[n],' ',21); rows[n][21]=0; rows[n][positions[a->coordinate_digit]]='^'; ++n;
        strcpy(rows[n++],"Use + north/east, - south/west"); strcpy(rows[n++],"Click through all digits to apply");
    } else if(a->page==PageResult) {
        title="MAP UPDATE"; footer="Click / Back: return to map";
        if(a->busy) { strcpy(rows[n++],"Download in progress..."); snprintf(rows[n++],96,"%.43s",busy_text); }
        else {
        snprintf(rows[n++],96,"%.43s",a->notice);
        snprintf(rows[n++],96,"Streets: %.34s",a->street_result[0]?a->street_result:"not downloaded");
        if(strlen(a->street_result)>34) snprintf(rows[n++],96,"%.43s",a->street_result+34);
        snprintf(rows[n++],96,"Territories: %.30s",a->territory_result[0]?a->territory_result:"not downloaded");
        if(strlen(a->territory_result)>30) snprintf(rows[n++],96,"%.43s",a->territory_result+30);
        snprintf(rows[n++],96,"Area %u: %u paths / %u cells / %u hulls",a->slot+1,a->active->basemap.feature_count,a->active->territories.cell_count,a->active->territories.hull_count);
        }
    } else if(a->page==PageAbout) {
        title="WDGWARS MAP 1.3"; footer="Territories: WDGWars / OSM: ODbL 1.0";
        strcpy(rows[n++],"Turn: zoom / click: actions"); strcpy(rows[n++],"Pan: turn N/S; hold + turn E/W");
        strcpy(rows[n++],"Connect with firmware Wi-Fi first");
#if WDG_MAP_PLUS_PROFILE
        strcpy(rows[n++],"GPS: Wardriver Plus isolated pins");
#else
        strcpy(rows[n++],"GPS reuses saved Wardriver settings");
#endif
        strcpy(rows[n++],"(c) OpenStreetMap contributors"); strcpy(rows[n++],"openstreetmap.org/copyright");
    }
    if(a->page!=PageMap) { for(unsigned i=0;i<n;++i) lines[i]=rows[i]; wdgwars_map_render_menu(a->pixels,title,lines,n,selected,footer); }
    for(unsigned y=0;y<170;y+=8) {
        unsigned height=y+8>170?170-y:8;
        memcpy(a->stripe,a->pixels+y*320,height*320*sizeof(uint16_t));
        if(!furi_hal_display_draw_rgb565(0,y,320,height,a->stripe)) break;
    }
    a->view.status=NULL; a->view.area_name=NULL; a->view.basemap_status=NULL; a->view.map_message=NULL; a->view.map_detail=NULL;
}
int32_t wdgwars_map_app(void* context) {
    UNUSED(context);
    WdgMapApp* a=calloc(1,sizeof(*a)); if(!a) return -1;
    a->active=map_alloc(sizeof(*a->active)); a->scratch=map_alloc(sizeof(*a->scratch));
    a->pixels=map_alloc(320*170*sizeof(uint16_t));
    a->stripe=heap_caps_malloc(320*8*sizeof(uint16_t),MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA);
    a->input=furi_message_queue_alloc(32,sizeof(InputEvent)); a->mutex=furi_mutex_alloc(FuriMutexTypeNormal);
    if(!a->active || !a->scratch || !a->pixels || !a->stripe || !a->input || !a->mutex) goto cleanup;
    a->target_zoom=a->view.zoom=15; a->active->zoom=15; a->auto_maps=true; a->last_input_ms=uptime_ms();
    strcpy(a->notice,"Choose coordinates or enable GPS");
    gps_startup(a);
    Gui* gui=furi_record_open(RECORD_GUI);
    FuriPubSub* events=furi_record_open(RECORD_INPUT_EVENTS);
    gui_direct_draw_acquire(gui);
    FuriPubSubSubscription* subscription=furi_pubsub_subscribe(events,input_callback,a);
    a->startup_load=true;
    if(!start_job(a,JobLoad,0)) a->startup_load=false;
    uint32_t last_frame=0;
    while(!a->exit_requested || a->busy) {
        InputEvent event;
        if(furi_message_queue_get(a->input,&event,furi_ms_to_ticks(20))==FuriStatusOk) handle_input(a,&event);
        wardriver_gps_poll(&a->gps,now_ms()); a->fix=wardriver_nmea_snapshot(&a->gps.parser,now_ms());
        if(a->fix.has_fix && (!a->center_set || a->view.follow)) {
            a->view.center_lat=a->fix.latitude; a->view.center_lon=a->fix.longitude;
            a->view.follow=true; a->center_set=true;
        }
        if(a->exit_requested && a->busy) { furi_mutex_acquire(a->mutex,FuriWaitForever); if(!a->saving && !a->done) a->cancel=true; furi_mutex_release(a->mutex); }
        finish_job(a);
        automatic_update(a);
        double diff=a->target_zoom-a->view.zoom;
        if(a->target_zoom<1 || a->view.zoom<1) a->view.zoom=a->target_zoom;
        else { a->view.zoom+=diff*0.3; if(diff<0.01 && diff>-0.01) a->view.zoom=a->target_zoom; }
        if(now_ms()-last_frame>=80) { render(a); last_frame=now_ms(); }
    }
    furi_pubsub_unsubscribe(events,subscription);
    gui_direct_draw_release(gui); furi_record_close(RECORD_INPUT_EVENTS); furi_record_close(RECORD_GUI);
    wardriver_gps_stop(&a->gps);
cleanup:
    if(a->input) furi_message_queue_free(a->input);
    if(a->mutex) furi_mutex_free(a->mutex);
    free(a->active); free(a->scratch); free(a->pixels); free(a->stripe); free(a); return 0;
}

/* Only this worker performs network/storage work. Displayed data is immutable
 * until the UI joins this worker and swaps the completed scratch snapshot. */
static void backoff(WdgMapApp* a,uint32_t seconds,bool wdg) {
    if(seconds>86400) seconds=86400;
    if(seconds<60) seconds=60;
    uint64_t until=uptime_ms()+(uint64_t)seconds*1000;
    if(wdg) { if(until>a->next_account_ms) a->next_account_ms=until; }
    else if(until>a->next_download_ms) a->next_download_ms=until;
}
static int32_t worker_main(void* context) {
    WdgMapApp* a=context; WdgMapSavedArea* out=a->scratch;
    bool updated=false,save=false,error=false,road_updated=false,territory_updated=false;
    char result[80]="",street[96]="",territory[96]="";
    if(a->job==JobLoad) {
        updated=wdg_cache_load(a->job_slot,out);
        if(updated && has_geometry(out)) snprintf(result,sizeof(result),"Opened offline Area %u",a->job_slot+1);
        else if(updated) strcpy(result,"Saved account loaded; map not downloaded");
        else snprintf(result,sizeof(result),"Area %u not downloaded; online loading ready",a->job_slot+1);
    } else if(a->job==JobSave) {
        save=a->request_center_set && has_geometry(out);
        if(!save) { strcpy(result,"No downloaded map to save. Use Update map first."); error=true; }
    } else {
        WardWigleCredentials credentials={0}; bool have_key=false;
        if(a->job!=JobRoads) have_key=wardriver_wdg_credentials_load(&credentials);
        a->key_present=have_key;
        if(a->job==JobAccount) {
            if(!have_key) { strcpy(result,"No WDGWars API key on SD. Street maps need no key."); error=true; }
            else {
                message(a,"Downloading WDGWars account...");
                WdgMapFetch f={.api_key=credentials.token,.account=&out->account,.cancelled=cancelled,.context=a};
                updated=wdg_map_fetch_account(&f); backoff(a,f.retry_after_s,true);
                if(updated) { out->flags|=WDG_CACHE_ACCOUNT; out->account_at=a->request_epoch; save=true; strcpy(result,"Account updated; separate from map download"); }
                else { snprintf(result,sizeof(result),"%s",f.message); error=true; }
            }
        } else {
            WdgMapRenderState request_view={.center_lat=a->request_lat,.center_lon=a->request_lon,.zoom=a->request_zoom};
            WdgMapBounds bounds;
            if(!view_bounds(&request_view,&bounds)) { strcpy(result,"View crosses date line. Use Whole world overview."); error=true; }
            else if(a->job==JobTerritories) {
                if(!have_key) { strcpy(territory,"No API key on SD; geographic map remains available"); error=true; }
                else {
                    message(a,a->request_zoom<0.5?"Loading worldwide gang territories...":"Loading WDGWars territories...");
                    WdgMapFetch f={.api_key=credentials.token,.map=&out->territories,.bounds=bounds,
                        .zoom=(uint8_t)a->request_zoom,.downloaded_at=a->request_epoch,.cancelled=cancelled,.context=a};
                    if(wdg_map_fetch_territories(&f)) {
                        updated=territory_updated=true; out->flags|=WDG_CACHE_TERRITORIES;
                        snprintf(territory,sizeof(territory),"%u cells, %u gang outlines%s",out->territories.cell_count,out->territories.hull_count,
                            out->territories.cells_skipped?" (overview)":"");
                        if(!out->territories.cell_count && !out->territories.hull_count) strcpy(territory,"Server returned no territories for this view");
                    } else { snprintf(territory,sizeof(territory),"%s",f.message); error=true; }
                    backoff(a,f.retry_after_s,true);
                }
                if(error) snprintf(result,sizeof(result),"%s",territory);
                else snprintf(result,sizeof(result),"Territories: %.66s",territory);
                save=updated;
            } else {
                message(a,"Loading street map and landmarks...");
                WdgBasemapFetch f={.bounds=bounds,.zoom=(uint8_t)a->request_zoom,.downloaded_at=a->request_epoch,
                    .cancelled=cancelled,.context=a,.map=&out->basemap};
                if(wdg_basemap_fetch(&f)) {
                    updated=road_updated=true; out->flags|=WDG_CACHE_BASEMAP;
                    if(out->basemap.partial)
                        snprintf(street,sizeof(street),"%.58s; %u paths, %u points",f.message[0]?f.message:"Partial streets",out->basemap.feature_count,out->basemap.point_count);
                    else {
                        snprintf(street,sizeof(street),"%u paths, %u points",out->basemap.feature_count,out->basemap.point_count);
                        if(!out->basemap.feature_count) strcpy(street,"Server returned no mapped streets for this view");
                    }
                    snprintf(result,sizeof(result),"Street map: %.65s",street);
                } else { snprintf(street,sizeof(street),"%s",f.message); snprintf(result,sizeof(result),"Street download failed: %.55s",f.message); error=true; }
                backoff(a,f.retry_after_s,false); save=updated;
            }
        }
        wardriver_wigle_erase(&credentials,sizeof(credentials));
    }
    if(cancelled(a)) { updated=false; save=false; strcpy(result,"Cancelled; cached map kept. Auto loading paused."); }
    if(save) {
        furi_mutex_acquire(a->mutex,FuriWaitForever);
        if(a->cancel) { save=false; updated=false; strcpy(result,"Cancelled; previous map kept"); }
        else a->saving=true;
        furi_mutex_release(a->mutex);
    }
    if(save) {
        message(a,"Finishing offline save to SD...");
        bool saved=wdg_cache_save(a->job_slot,out);
        if(a->job==JobSave) {
            updated=saved; error=!saved;
            snprintf(result,sizeof(result),saved?"Copied downloaded map to Area %u":"SD save failed; previous area kept",a->job_slot+1);
        } else if(!saved) { snprintf(result,sizeof(result),"Map updated in memory; SD save failed"); error=true; }
        /* Successful metadata writes never turn a failed/empty map into 'Map saved'. */
    }
    furi_mutex_acquire(a->mutex,FuriWaitForever);
    a->commit=updated; a->job_map_error=error; a->road_updated=road_updated; a->territory_updated=territory_updated;
    if(street[0]) snprintf(a->street_result,sizeof(a->street_result),"%s",street);
    if(territory[0]) snprintf(a->territory_result,sizeof(a->territory_result),"%s",territory);
    snprintf(a->worker_message,sizeof(a->worker_message),"%s",result); a->done=true;
    furi_mutex_release(a->mutex); return 0;
}
