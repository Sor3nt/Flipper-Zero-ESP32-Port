#include "wardriver_wigle.h"
#include <furi.h>
#include <storage/storage.h>
#include <wifi/wlan_hal.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <esp_timer.h>
#include <esp_random.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIGLE_URL "https://api.wigle.net/api/v2/file/upload"
typedef struct {
    WardWigleUpload* job;
    File* file;
    esp_http_client_handle_t http;
    uint32_t size,sent;
    int64_t deadline;
    bool requested;
    char buffer[2048],response[4097],prefix[384],suffix[96],boundary[48];
} Transfer;
static void report(Transfer* t,const char* stage) {
    if(t->job->progress) t->job->progress(t->job->context,stage,t->size?(unsigned)((uint64_t)t->sent*100/t->size):0);
}
static bool cancelled(Transfer* t) {
    if(t->job->cancelled && t->job->cancelled(t->job->context)) {
        strcpy(t->job->result,t->requested?"Cancelled; check WiGLE before retry":"Upload cancelled");
        t->job->uncertain=t->requested; return true;
    }
    if(esp_timer_get_time()>t->deadline) {
        strcpy(t->job->result,"Timed out; check WiGLE before retry");
        t->job->uncertain=t->requested; return true;
    }
    return false;
}
static bool write_all(Transfer* t,const char* data,size_t length) {
    while(length) {
        if(cancelled(t)) return false;
        int n=esp_http_client_write(t->http,data,(int)length);
        if(n<=0 || n>(int)length) { strcpy(t->job->result,"Send failed; check WiGLE before retry"); t->job->uncertain=true; return false; }
        data+=n; length-=n;
    }
    return true;
}
/* HTTP/TLS runs on Sor3nt's real FreeRTOS WLAN worker, not a Furi task whose
 * TLS slot 0 is occupied. No new radio initialization or connection happens here. */
static void transfer(void* context) {
    Transfer* t=context; WardWigleUpload* job=t->job;
    if(cancelled(t)) return;
    if(!wlan_hal_is_connected()) { strcpy(job->result,"Connect Sor3nt Wi-Fi first"); return; }
    esp_http_client_config_t cfg={
        .url=WIGLE_URL,.method=HTTP_METHOD_POST,.transport_type=HTTP_TRANSPORT_OVER_SSL,
        .timeout_ms=10000,.buffer_size=2048,.buffer_size_tx=1024,
        .crt_bundle_attach=esp_crt_bundle_attach,.skip_cert_common_name_check=false,
        .disable_auto_redirect=true,.keep_alive_enable=false,
        .username=job->credentials.name,.password=job->credentials.token,.auth_type=HTTP_AUTH_TYPE_BASIC,
        .user_agent="Sor3nt-Wardriver/1.3",
    };
    t->http=esp_http_client_init(&cfg);
    if(!t->http) { strcpy(job->result,"HTTP allocation failed"); return; }
    char content_type[100]; snprintf(content_type,sizeof(content_type),"multipart/form-data; boundary=%s",t->boundary);
    if(esp_http_client_set_header(t->http,"Content-Type",content_type)!=ESP_OK ||
       esp_http_client_set_header(t->http,"Accept","application/json")!=ESP_OK) {
        strcpy(job->result,"HTTP setup failed"); goto done;
    }
    report(t,"CONNECTING TO WIGLE");
    int length=(int)(strlen(t->prefix)+t->size+strlen(t->suffix));
    t->requested=true; /* open can send headers before reporting a transport failure */
    if(esp_http_client_open(t->http,length)!=ESP_OK) {
        strcpy(job->result,"TLS/network error; check Wi-Fi/time"); goto done;
    }
    if(!write_all(t,t->prefix,strlen(t->prefix))) goto done;
    while(t->sent<t->size) {
        if(cancelled(t)) goto done;
        size_t want=t->size-t->sent; if(want>sizeof(t->buffer)) want=sizeof(t->buffer);
        size_t n=storage_file_read(t->file,t->buffer,want);
        if(n!=want || storage_file_get_error(t->file)!=FSE_OK) {
            strcpy(job->result,"SD read failed; check WiGLE before retry"); job->uncertain=true; goto done;
        }
        if(!write_all(t,t->buffer,n)) goto done;
        t->sent+=(uint32_t)n; report(t,"UPLOADING CSV");
    }
    if(!write_all(t,t->suffix,strlen(t->suffix))) goto done;
    report(t,"WAITING FOR WIGLE");
    if(cancelled(t)) goto done;
    if(esp_http_client_fetch_headers(t->http)<0) {
        strcpy(job->result,"No response; check WiGLE before retry"); job->uncertain=true; goto done;
    }
    int status=esp_http_client_get_status_code(t->http);
    if(status==401 || status==403) { strcpy(job->result,"API credentials rejected"); goto done; }
    if(status==429) { strcpy(job->result,"WiGLE rate limit; retry later"); goto done; }
    if(status<200 || status>=300) {
        snprintf(job->result,sizeof(job->result),"WiGLE HTTP %d; check account",status); job->uncertain=status>=500; goto done;
    }
    size_t used=0;
    for(;;) {
        if(cancelled(t)) goto done;
        if(used==sizeof(t->response)-1) { strcpy(job->result,"Large response; check WiGLE account"); job->uncertain=true; goto done; }
        int n=esp_http_client_read(t->http,t->response+used,(int)(sizeof(t->response)-1-used));
        if(n<0) { strcpy(job->result,"Response lost; check WiGLE account"); job->uncertain=true; goto done; }
        if(!n) break;
        used+=(size_t)n;
    }
    bool success=false,warning=false;
    if(!wardriver_wigle_response(t->response,used,&success,&warning)) {
        strcpy(job->result,"Unknown response; check WiGLE account"); job->uncertain=true;
    } else if(success) {
        job->accepted=true;
        strcpy(job->result,warning?"Accepted; check WiGLE warning":"Accepted by WiGLE");
    } else strcpy(job->result,"WiGLE rejected file; check account");
done:
    esp_http_client_close(t->http); esp_http_client_cleanup(t->http); t->http=NULL;
}
void wardriver_wigle_upload(WardWigleUpload* job) {
    job->accepted=false; job->uncertain=false;
    strcpy(job->result,"Upload not started");
    if(!*job->credentials.name || !*job->credentials.token ||
       !wardriver_wigle_value_valid(job->credentials.name,true) || !wardriver_wigle_value_valid(job->credentials.token,false)) {
        strcpy(job->result,"Set API Name and API Token"); return;
    }
    if(!wardriver_wigle_path_valid(job->path)) { strcpy(job->result,"Select a wardrive WiGLE CSV"); return; }
    if(!wlan_hal_is_connected()) { strcpy(job->result,"Connect Sor3nt Wi-Fi first"); return; }
    Transfer* t=calloc(1,sizeof(*t)); if(!t) { strcpy(job->result,"Not enough upload memory"); return; }
    t->job=job; t->deadline=esp_timer_get_time()+180LL*1000000;
    Storage* s=furi_record_open(RECORD_STORAGE);
    t->file=storage_file_alloc(s); bool opened=t->file && storage_file_open(t->file,job->path,FSAM_READ,FSOM_OPEN_EXISTING);
    if(!opened) { strcpy(job->result,"Cannot open selected CSV"); goto done; }
    uint64_t size=storage_file_size(t->file);
    if(!size || size>WARD_WIGLE_MAX_FILE) { strcpy(job->result,"CSV empty or exceeds 16 MiB"); goto done; }
    t->size=(uint32_t)size; report(t,"CHECKING GPS CSV");
    WardWigleCsv csv={0}; uint32_t checked=0;
    while(checked<t->size) {
        if(cancelled(t)) goto done;
        size_t want=t->size-checked; if(want>sizeof(t->buffer)) want=sizeof(t->buffer);
        size_t n=storage_file_read(t->file,t->buffer,want);
        if(n!=want || storage_file_get_error(t->file)!=FSE_OK || !wardriver_wigle_csv_feed(&csv,t->buffer,n)) {
            strcpy(job->result,"Invalid CSV or SD read error"); goto done;
        }
        checked+=(uint32_t)n;
    }
    if(!wardriver_wigle_csv_finish(&csv)) { strcpy(job->result,"CSV needs valid GPS Wi-Fi rows"); goto done; }
    if(!storage_file_seek(t->file,0,true)) { strcpy(job->result,"Cannot rewind CSV"); goto done; }
    snprintf(t->boundary,sizeof(t->boundary),"wardriver-%08lx-%08lx",(unsigned long)esp_random(),(unsigned long)esp_random());
    snprintf(t->prefix,sizeof(t->prefix),"--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\nContent-Type: text/csv\r\n\r\n",t->boundary,strrchr(job->path,'/')+1);
    snprintf(t->suffix,sizeof(t->suffix),"\r\n--%s--\r\n",t->boundary);
    if(!wlan_hal_run_in_worker(transfer,t)) strcpy(job->result,"Radio busy; upload not started");
done:
    if(opened) storage_file_close(t->file);
    if(t->file) storage_file_free(t->file);
    furi_record_close(RECORD_STORAGE);
    wardriver_wigle_erase(t,sizeof(*t)); free(t);
}
