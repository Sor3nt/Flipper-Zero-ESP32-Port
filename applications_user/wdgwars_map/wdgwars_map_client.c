#include "wdgwars_map_client.h"
#include "wdgwars_map_json.h"
#include "../wardriver/wardriver_wigle.h"
#include <wifi/wlan_hal.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MAP_HTTP_DEADLINE_US (60LL*1000000)
/* Authoritative configured origins: credentials can never accompany a basemap
 * request and redirects are disabled. Public endpoint references are documented
 * in tests/wdgwars_map/README.md. */
typedef struct {
    WdgMapHttpRequest* request;
    int64_t deadline;
    bool content_type_seen, content_type_json;
    char url[2048];
} Transfer;
static void result(WdgMapHttpRequest* r,WdgMapClientStatus status,const char* text) { r->status=status; snprintf(r->message,sizeof(r->message),"%s",text); }
static bool interrupted(Transfer* t) {
    WdgMapHttpRequest* r=t->request;
    if(r->cancelled&&r->cancelled(r->context)) { result(r,WdgMapClientCancelled,"Download cancelled; saved map kept"); return true; }
    if(esp_timer_get_time()>t->deadline) { result(r,WdgMapClientTimeout,"Download timed out; saved map kept"); return true; }
    return false;
}
static bool equal_case(const char* a,const char* b) { while(*a&&*b) { char x=*a++,y=*b++; if(x>='A'&&x<='Z') x+=32; if(y>='A'&&y<='Z') y+=32; if(x!=y) return false; } return !*a&&!*b; }
static bool json_mime(const char* value) {
    static const char expected[]="application/json";
    for(size_t i=0;i<sizeof(expected)-1;++i) { char c=value[i]; if(!c) return false; if(c>='A'&&c<='Z') c+=32; if(c!=expected[i]) return false; }
    char tail=value[sizeof(expected)-1]; return !tail||tail==';'||tail==' '||tail=='\t';
}
static esp_err_t event(esp_http_client_event_t* e) {
    Transfer* t=e->user_data;
    if(e->event_id==HTTP_EVENT_ON_HEADER && e->header_key && e->header_value) {
        if(equal_case(e->header_key,"Retry-After")) {
            const char* p=e->header_value; uint32_t seconds=0; bool valid=*p!=0;
            for(;*p;++p) { if(*p<'0'||*p>'9'||seconds>8640) { valid=false; break; } seconds=seconds*10+(unsigned)(*p-'0'); }
            t->request->retry_after_s=valid?(seconds>86400?86400:seconds):86400;
        } else if(equal_case(e->header_key,"Content-Type")) {
            t->content_type_seen=true;
            const char* v=e->header_value;
            t->content_type_json=json_mime(v);
        }
    }
    return ESP_OK;
}
static bool valid_path(WdgMapService service,const char* p) {
    if(!p || *p!='/' || strlen(p)>1800) return false;
    for(const char* s=p;*s;++s) if((unsigned char)*s<33||(unsigned char)*s>126||*s=='#'||*s=='\\') return false;
    if(service==WdgMapServiceWdgWars) return !strcmp(p,"/api/me")||!strcmp(p,"/api/territories")||!strncmp(p,"/api/member-territories?",24);
    if(service==WdgMapServiceOverpass) return !strncmp(p,"/api/interpreter?data=",22);
    return false;
}
static void transfer(void* context) {
    Transfer* t=context; WdgMapHttpRequest* r=t->request;
    if(interrupted(t)) return;
    if(!wlan_hal_is_connected()) { result(r,WdgMapClientOffline,"Connect Sor3nt Wi-Fi; offline map ready"); return; }
    esp_http_client_config_t cfg={
        .url=t->url,.method=HTTP_METHOD_GET,.transport_type=HTTP_TRANSPORT_OVER_SSL,
        /* Overpass allows a 20-second server query; do not abandon it at 5s. */
        .timeout_ms=r->service==WdgMapServiceOverpass?25000:10000,.buffer_size=2048,.buffer_size_tx=2048,
        .crt_bundle_attach=esp_crt_bundle_attach,.skip_cert_common_name_check=false,
        .disable_auto_redirect=true,.keep_alive_enable=false,
        .auth_type=HTTP_AUTH_TYPE_NONE,.user_agent="Sor3nt-WDG-Map/1.2 (ESP32; personal viewport)",
        .event_handler=event,.user_data=t,
    };
    esp_http_client_handle_t http=esp_http_client_init(&cfg);
    if(!http) { result(r,WdgMapClientMemory,"Not enough HTTP memory"); return; }
    if(esp_http_client_set_header(http,"Accept","application/json")!=ESP_OK ||
       esp_http_client_set_header(http,"Accept-Encoding","identity")!=ESP_OK ||
       (r->service==WdgMapServiceWdgWars && esp_http_client_set_header(http,"X-API-Key",r->api_key)!=ESP_OK)) {
        result(r,WdgMapClientNetwork,"HTTP setup failed"); goto done;
    }
    esp_err_t opened=esp_http_client_open(http,0);
    if(opened!=ESP_OK) { char text[64]; snprintf(text,sizeof(text),"%s connect 0x%X; check Wi-Fi/time",r->service==WdgMapServiceWdgWars?"WDG":"OSM",(unsigned)opened); result(r,WdgMapClientNetwork,text); goto done; }
    if(interrupted(t)) goto done;
    int64_t expected=esp_http_client_fetch_headers(http);
    if(expected<0) { result(r,WdgMapClientNetwork,r->service==WdgMapServiceWdgWars?"WDG no response; server/network unavailable":"OSM no response; server busy or network"); goto done; }
    r->http_status=esp_http_client_get_status_code(http);
    if(r->http_status==401||r->http_status==403) { result(r,r->service==WdgMapServiceWdgWars?WdgMapClientAuth:WdgMapClientHttp,r->service==WdgMapServiceWdgWars?"WDGWars API key rejected":"Basemap service denied access"); goto done; }
    if(r->http_status==429) { if(!r->retry_after_s) r->retry_after_s=60; result(r,WdgMapClientRateLimit,"Rate limited; wait before refreshing"); goto done; }
    if(r->http_status!=200) { char text[64]; snprintf(text,sizeof(text),"Map service HTTP %d; saved map kept",r->http_status); result(r,WdgMapClientHttp,text); goto done; }
    if(t->content_type_seen&&!t->content_type_json) { result(r,WdgMapClientSchema,"Expected JSON; saved map kept"); goto done; }
    if(expected>=(int64_t)r->capacity) { result(r,WdgMapClientTooLarge,"Area too large; zoom in and retry"); goto done; }
    for(;;) {
        if(interrupted(t)) goto done;
        if(r->length==r->capacity-1) {
            char extra; int n=esp_http_client_read(http,&extra,1);
            if(n==0) break;
            result(r,n<0?WdgMapClientNetwork:WdgMapClientTooLarge,n<0?"Download interrupted; saved map kept":"Area too large; zoom in and retry"); goto done;
        }
        size_t want=r->capacity-1-r->length; if(want>2048) want=2048;
        int n=esp_http_client_read(http,r->body+r->length,(int)want);
        if(n<0||(size_t)n>want) { char text[64]; snprintf(text,sizeof(text),"%s read error %d; saved map kept",r->service==WdgMapServiceWdgWars?"WDG":"OSM",n); result(r,WdgMapClientNetwork,text); goto done; }
        if(!n) break;
        r->length+=(size_t)n;
    }
    r->body[r->length]=0;
    if(expected>0&&(uint64_t)expected!=r->length) { result(r,WdgMapClientNetwork,"Incomplete download; saved map kept"); goto done; }
    if(!esp_http_client_is_complete_data_received(http)) { result(r,WdgMapClientNetwork,"Incomplete HTTP body; saved map kept"); goto done; }
    if(!r->length) { result(r,WdgMapClientSchema,"Empty response; saved map kept"); goto done; }
    result(r,WdgMapClientOk,"Download complete");
done:
    r->body[r->length]=0;
    esp_http_client_close(http); esp_http_client_cleanup(http);
}
bool wdg_map_http_get(WdgMapHttpRequest* r) {
    if(!r) return false;
    r->length=0; r->http_status=0; r->retry_after_s=0;
    result(r,WdgMapClientInvalid,"Invalid map request");
    if(!r->body||r->capacity<2||r->capacity>WDG_MAP_RESPONSE_MAX+1||!valid_path(r->service,r->path)) return false;
    r->body[0]=0;
    if(r->service==WdgMapServiceWdgWars && !wardriver_wdg_key_valid(r->api_key)) { result(r,WdgMapClientAuth,"Set WDGWars API key on the SD card"); return false; }
    if(r->service==WdgMapServiceOverpass && r->api_key && *r->api_key) return false;
    Transfer* t=calloc(1,sizeof(*t)); if(!t) { result(r,WdgMapClientMemory,"Not enough HTTP memory"); return false; }
    t->request=r; t->deadline=esp_timer_get_time()+MAP_HTTP_DEADLINE_US;
    snprintf(t->url,sizeof(t->url),"https://%s%s",r->service==WdgMapServiceWdgWars?"wdgwars.pl":"overpass-api.de",r->path);
    result(r,WdgMapClientBusy,"Radio busy; saved map kept");
    if(!interrupted(t) && !wlan_hal_run_in_worker(transfer,t)) result(r,WdgMapClientBusy,"Radio busy; saved map kept");
    free(t); return r->status==WdgMapClientOk;
}
static bool fetch(WdgMapFetch* f,bool account) {
    if(!f) return false;
    f->status=WdgMapClientInvalid; f->http_status=0; f->retry_after_s=0; f->bytes_received=0; snprintf(f->message,sizeof(f->message),"Invalid map request");
    if(account?!f->account:(!f->map||!wdg_map_bounds_valid(&f->bounds)||f->zoom>20)) return false;
    size_t capacity=account?32769:WDG_MAP_RESPONSE_MAX+1;
    char* body=heap_caps_malloc(capacity,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!body) { f->status=WdgMapClientMemory; snprintf(f->message,sizeof(f->message),"Not enough download memory"); return false; }
    char path[192];
    const bool overview=!account && f->zoom<6;
    if(account) strcpy(path,"/api/me");
    else if(overview) strcpy(path,"/api/territories");
    else snprintf(path,sizeof(path),"/api/member-territories?compact=1&bbox=%.6f,%.6f,%.6f,%.6f&zoom=%u",f->bounds.west,f->bounds.south,f->bounds.east,f->bounds.north,f->zoom);
    WdgMapHttpRequest r={.service=WdgMapServiceWdgWars,.path=path,.api_key=f->api_key,.body=body,.capacity=capacity,.cancelled=f->cancelled,.context=f->context};
    bool ok=wdg_map_http_get(&r);
    f->status=r.status; f->http_status=r.http_status; f->retry_after_s=r.retry_after_s; f->bytes_received=r.length; strcpy(f->message,r.message);
    if(ok && f->cancelled && f->cancelled(f->context)) { f->status=WdgMapClientCancelled; strcpy(f->message,"Download cancelled; saved map kept"); ok=false; }
    if(ok) {
        if(account) ok=wdg_map_parse_account(body,r.length,f->account,f->message,sizeof(f->message));
        else if(overview) ok=wdg_map_parse_overview(body,r.length,f->map,f->message,sizeof(f->message));
        else ok=wdg_map_parse_territories(body,r.length,f->map,f->message,sizeof(f->message));
        if(ok) {
            f->status=WdgMapClientOk; strcpy(f->message,"Map updated");
            if(!account) {
                f->map->bounds=overview?(WdgMapBounds){-85.051128,-180,85.051128,180}:f->bounds;
                if(overview) f->map->cells_skipped=true;
                f->map->zoom=f->zoom; f->map->downloaded_at=f->downloaded_at;
            }
        } else f->status=WdgMapClientSchema;
    }
    free(body); return ok;
}
bool wdg_map_fetch_territories(WdgMapFetch* f) { return fetch(f,false); }
bool wdg_map_fetch_account(WdgMapFetch* f) { return fetch(f,true); }
