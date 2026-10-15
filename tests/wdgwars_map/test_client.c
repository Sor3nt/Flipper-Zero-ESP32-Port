#include "wdgwars_map_client.h"
#include <esp_http_client.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct Http { esp_http_client_config_t cfg; } handle;
static const char good[]="{\"ok\":true,\"grid_lat\":0.02,\"grid_lng\":0.02,\"cells\":[{\"lat\":41,\"lng\":-81,\"gang_id\":1,\"color\":\"#aabbcc\"}]}";
static const char* response=good;
static const char* content_type="application/json";
static const char* retry_after="60";
static char url[2048];
static int status=200,opens,closes,cleanups,key_headers,reads,read_fail,worker_calls;
static size_t offset;
static bool connected=true,busy=false,cancel_flag=false,complete=true;
static int64_t now,step,expected=-2,header_delay;
static int connect_error,cancel_after_reads;
static size_t chunk_size=17;
static bool header_error;
static bool cancel(void* arg) { (void)arg; return cancel_flag; }
int64_t esp_timer_get_time(void) { now+=step; return now; }
void* heap_caps_malloc(size_t n,int caps) { assert(caps==3); return malloc(n); }
int esp_crt_bundle_attach(void* c) { (void)c; return 0; }
bool wlan_hal_is_connected(void) { return connected; }
bool wlan_hal_run_in_worker(void (*fn)(void*),void* arg) { ++worker_calls; if(busy) return false; fn(arg); return true; }
bool wardriver_wdg_key_valid(const char* key) { return key&&strlen(key)==64; }
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t* c) { handle.cfg=*c; assert(c->method==HTTP_METHOD_GET&&c->transport_type==HTTP_TRANSPORT_OVER_SSL&&c->disable_auto_redirect&&!c->skip_cert_common_name_check&&c->crt_bundle_attach&&c->timeout_ms==(strstr(c->url,"https://overpass-api.de/")==c->url?25000:10000)); strcpy(url,c->url); return &handle; }
esp_err_t esp_http_client_set_header(esp_http_client_handle_t h,const char* k,const char* v) { assert(h==&handle); if(!strcmp(k,"X-API-Key")) { ++key_headers; assert(strstr(url,"https://wdgwars.pl/")==url&&strlen(v)==64); } return ESP_OK; }
esp_err_t esp_http_client_open(esp_http_client_handle_t h,int length) { assert(h==&handle&&!length); ++opens; return connect_error; }
static void header(char* key,char* value) { esp_http_client_event_t e={.event_id=HTTP_EVENT_ON_HEADER,.user_data=handle.cfg.user_data,.header_key=key,.header_value=value}; assert(handle.cfg.event_handler(&e)==ESP_OK); }
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t h) { assert(h==&handle); now+=header_delay; if(header_error) return -1; header("Content-Type",(char*)content_type); if(status==429) header("Retry-After",(char*)retry_after); return expected==-2?(int64_t)strlen(response):expected; }
int esp_http_client_get_status_code(esp_http_client_handle_t h) { assert(h==&handle); return status; }
int esp_http_client_read(esp_http_client_handle_t h,char* out,int want) { assert(h==&handle); ++reads; if(read_fail&&reads>=read_fail) return -1; size_t n=strlen(response)-offset; if(n>(size_t)want) n=(size_t)want; if(n>chunk_size) n=chunk_size; memcpy(out,response+offset,n); offset+=n; if(cancel_after_reads&&reads>=cancel_after_reads) cancel_flag=true; return (int)n; }
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t h) { assert(h==&handle); return complete; }
esp_err_t esp_http_client_close(esp_http_client_handle_t h) { assert(h==&handle); ++closes; return ESP_OK; }
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t h) { assert(h==&handle); ++cleanups; return ESP_OK; }
static const char key[]="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static WdgMapHttpRequest request(char* b,size_t capacity) { return (WdgMapHttpRequest){.service=WdgMapServiceWdgWars,.path="/api/member-territories?compact=1&bbox=-81,40,-80,41&zoom=13",.api_key=key,.body=b,.capacity=capacity,.cancelled=cancel}; }
static void reset(void) { status=200; offset=0; opens=closes=cleanups=key_headers=reads=worker_calls=read_fail=0; connected=complete=true; busy=cancel_flag=false; now=step=header_delay=0; connect_error=0; cancel_after_reads=0; chunk_size=17; header_error=false; expected=-2; response=good; content_type="application/json"; }
int main(void) {
    char buffer[2048]; reset(); WdgMapHttpRequest r=request(buffer,sizeof(buffer)); assert(wdg_map_http_get(&r)&&r.length==strlen(good)&&!strcmp(buffer,good)&&key_headers==1&&closes==1&&cleanups==1);
    reset(); r=request(buffer,sizeof(buffer)); r.service=WdgMapServiceOverpass; r.api_key=NULL; r.path="/api/interpreter?data=%5Bout%3Ajson%5D%3B"; assert(wdg_map_http_get(&r)&&!key_headers&&strstr(url,"https://overpass-api.de/")==url);
    reset(); r=request(buffer,sizeof(buffer)); r.service=WdgMapServiceOverpass; r.path="/api/interpreter?data=x"; assert(!wdg_map_http_get(&r)&&r.status==WdgMapClientInvalid&&!opens);
    reset(); r=request(buffer,sizeof(buffer)); r.path="https://attacker.test/api/me"; assert(!wdg_map_http_get(&r)&&!worker_calls);
    reset(); r=request(buffer,sizeof(buffer)); r.path="/api/territories"; assert(wdg_map_http_get(&r)&&key_headers==1&&!strcmp(url,"https://wdgwars.pl/api/territories"));
    reset(); r=request(buffer,sizeof(buffer)); r.path="/api/territories?foreign=1"; assert(!wdg_map_http_get(&r)&&!opens);
    reset(); r=request(buffer,sizeof(buffer)); r.path="/api/territories/other"; assert(!wdg_map_http_get(&r)&&!opens);
    reset(); r=request(buffer,sizeof(buffer)); r.path="/api/territories"; r.api_key=NULL; assert(!wdg_map_http_get(&r)&&r.status==WdgMapClientAuth&&!opens);
    reset(); r=request(buffer,sizeof(buffer)); r.path="/api/upload-csv"; assert(!wdg_map_http_get(&r)&&!opens);
    reset(); r=request(buffer,sizeof(buffer)); r.path="/api/me\r\nX: y"; assert(!wdg_map_http_get(&r)&&!opens);
    reset(); r=request(buffer,sizeof(buffer)); r.api_key=NULL; assert(!wdg_map_http_get(&r)&&r.status==WdgMapClientAuth&&!opens);
    reset(); r=request(buffer,sizeof(buffer)); status=401; assert(!wdg_map_http_get(&r)&&r.status==WdgMapClientAuth&&!reads&&cleanups==1);
    reset(); r=request(buffer,sizeof(buffer)); status=429; retry_after="123"; assert(!wdg_map_http_get(&r)&&r.retry_after_s==123&&r.status==WdgMapClientRateLimit&&!reads);
    reset(); r=request(buffer,sizeof(buffer)); status=302; assert(!wdg_map_http_get(&r)&&r.status==WdgMapClientHttp&&opens==1);
    reset(); r=request(buffer,sizeof(buffer)); connected=false; assert(!wdg_map_http_get(&r)&&r.status==WdgMapClientOffline&&!opens);
    reset(); r=request(buffer,sizeof(buffer)); busy=true; assert(!wdg_map_http_get(&r)&&r.status==WdgMapClientBusy&&!opens);
    reset(); r=request(buffer,sizeof(buffer)); cancel_flag=true; assert(!wdg_map_http_get(&r)&&r.status==WdgMapClientCancelled&&!opens);
    reset(); r=request(buffer,sizeof(buffer)); step=25000000; assert(!wdg_map_http_get(&r)&&r.status==WdgMapClientTimeout&&cleanups==1);
    reset(); r=request(buffer,10); assert(!wdg_map_http_get(&r)&&r.status==WdgMapClientTooLarge&&!reads);
    reset(); r=request(buffer,10); expected=0; assert(!wdg_map_http_get(&r)&&r.status==WdgMapClientTooLarge&&reads);
    reset(); r=request(buffer,sizeof(buffer)); expected=(int64_t)strlen(good)+4; assert(!wdg_map_http_get(&r)&&r.status==WdgMapClientNetwork);
    reset(); r=request(buffer,sizeof(buffer)); content_type="text/html"; assert(!wdg_map_http_get(&r)&&r.status==WdgMapClientSchema&&!reads);
    reset(); r=request(buffer,sizeof(buffer)); read_fail=2; assert(!wdg_map_http_get(&r)&&r.status==WdgMapClientNetwork&&cleanups==1);
    reset(); r=request(buffer,sizeof(buffer)); expected=0; complete=false; assert(!wdg_map_http_get(&r)&&r.status==WdgMapClientNetwork&&cleanups==1);
    WdgMapSnapshot* map=calloc(1,sizeof(*map)); assert(map); reset();
    WdgMapFetch f={.api_key=key,.bounds={40,-82,42,-80},.zoom=14,.downloaded_at=100,.map=map};
    assert(wdg_map_fetch_territories(&f)&&map->cell_count==1&&map->downloaded_at==100&&map->zoom==14&&map->bounds.west==-82);
    reset(); response="{}"; assert(!wdg_map_fetch_territories(&f)&&f.status==WdgMapClientSchema&&map->cell_count==1&&map->downloaded_at==100);
    assert(!strstr(f.message,key)); free(map); puts("WDG map HTTP: all checks passed"); return 0;
}
