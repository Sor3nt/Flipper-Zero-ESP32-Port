/* Real WiGLE modules with fault-injecting storage/HTTP boundaries. No network. */
#include "wardriver_wigle.h"
#include <storage/storage.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#define CHECK(c) do { if(!(c)) { fprintf(stderr,"WiGLE FAIL line %d: %s\n",__LINE__,#c); exit(1); } } while(0)
struct File { FILE* f; int error; };
struct Http { size_t read; };
static const char* directory;
static unsigned files,records,clients,requests,closes;
static bool online=true,dispatch=true,open_fail,write_fail,read_fail,sync_fail,rename_fail,cancel_flag,cancel_on_progress,fetch_fail;
static int status=200;
static const char* response="{\"success\":true,\"results\":{\"transids\":[{\"transid\":\"test\"}]}}";
static char request[20000],content_type[120]; static size_t written,expected;
static void path_for(const char* path,char out[600]) { snprintf(out,600,"%s/%s",directory,strrchr(path,'/')+1); }
void* furi_record_open(const char* s) { (void)s; ++records; return (void*)1; }
void furi_record_close(const char* s) { (void)s; --records; }
File* storage_file_alloc(Storage* s) { (void)s; ++files; return calloc(1,sizeof(File)); }
bool storage_file_open(File* f,const char* path,int access,int mode) {
    (void)mode; char out[600]; path_for(path,out);
    f->f=fopen(out,access==FSAM_READ?"rb":"wb"); return f->f!=NULL;
}
size_t storage_file_read(File* f,void* data,size_t n) { if(read_fail) { f->error=2; return 0; } return fread(data,1,n,f->f); }
size_t storage_file_write(File* f,const void* data,size_t n) { if(write_fail) return 0; return fwrite(data,1,n,f->f); }
FS_Error storage_file_get_error(File* f) { return f->error; }
uint64_t storage_file_size(File* f) { long pos=ftell(f->f); fseek(f->f,0,SEEK_END); long n=ftell(f->f); fseek(f->f,pos,SEEK_SET); return n; }
bool storage_file_seek(File* f,uint32_t off,bool from_start) { return fseek(f->f,off,from_start?SEEK_SET:SEEK_CUR)==0; }
bool storage_file_sync(File* f) { return !sync_fail && !fflush(f->f); }
bool storage_file_close(File* f) { if(!f->f) return false; bool ok=!fclose(f->f); f->f=NULL; return ok; }
void storage_file_free(File* f) { CHECK(!f->f); --files; free(f); }
bool storage_simply_mkdir(Storage* s,const char* path) { (void)s; (void)path; return true; }
int storage_common_stat(Storage* s,const char* path,FileInfo* info) { (void)s; (void)info; char out[600]; struct stat st; path_for(path,out); return stat(out,&st)?FSE_NOT_EXIST:FSE_OK; }
int storage_common_remove(Storage* s,const char* path) { (void)s; char out[600]; path_for(path,out); return remove(out)?FSE_NOT_EXIST:FSE_OK; }
int storage_common_rename(Storage* s,const char* from,const char* to) { (void)s; char a[600],b[600]; path_for(from,a); path_for(to,b); return rename_fail?2:rename(a,b)?2:FSE_OK; }
int64_t esp_timer_get_time(void) { return 1000000; }
uint32_t esp_random(void) { return 0x12345678; }
bool wlan_hal_is_connected(void) { return online; }
bool wlan_hal_run_in_worker(void (*fn)(void*),void* arg) { if(!dispatch) return false; fn(arg); return true; }
int esp_crt_bundle_attach(void* conf) { (void)conf; return 0; }
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t* c) {
    CHECK(!strcmp(c->url,"https://api.wigle.net/api/v2/file/upload"));
    CHECK(c->crt_bundle_attach==esp_crt_bundle_attach && !c->skip_cert_common_name_check && c->disable_auto_redirect);
    CHECK(c->auth_type==HTTP_AUTH_TYPE_BASIC && c->method==HTTP_METHOD_POST && c->transport_type==HTTP_TRANSPORT_OVER_SSL);
    CHECK(c->timeout_ms>0 && c->timeout_ms<=10000 && !c->keep_alive_enable);
    CHECK(!strcmp(c->username,"test-name") && !strcmp(c->password,"test-not-a-secret"));
    ++clients; return calloc(1,sizeof(struct Http));
}
int esp_http_client_set_header(esp_http_client_handle_t h,const char* key,const char* value) { (void)h; if(!strcmp(key,"Content-Type")) snprintf(content_type,sizeof(content_type),"%s",value); return 0; }
int esp_http_client_open(esp_http_client_handle_t h,int n) { (void)h; ++requests; expected=n; written=0; return open_fail?-1:0; }
int esp_http_client_write(esp_http_client_handle_t h,const char* data,int n) {
    (void)h; if(write_fail) return -1;
    if(n>17) n=17; /* Exercise partial writes, including multipart boundaries. */
    CHECK(written+(size_t)n<sizeof(request)); memcpy(request+written,data,n); written+=n; request[written]=0; return n;
}
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t h) { (void)h; return fetch_fail?-1:(int64_t)strlen(response); }
int esp_http_client_get_status_code(esp_http_client_handle_t h) { (void)h; return status; }
int esp_http_client_read(esp_http_client_handle_t h,char* data,int n) {
    size_t remain=strlen(response)-h->read; if((size_t)n>remain) n=(int)remain; if(n>11) n=11;
    memcpy(data,response+h->read,n); h->read+=n; return n;
}
int esp_http_client_close(esp_http_client_handle_t h) { (void)h; ++closes; return 0; }
int esp_http_client_cleanup(esp_http_client_handle_t h) { --clients; free(h); return 0; }
static bool cancel(void* context) { CHECK(context); return cancel_flag; }
static void progress(void* context,const char* stage,unsigned percent) { CHECK(context && stage && percent<=100); if(cancel_on_progress && percent) cancel_flag=true; }
static const char* metadata="WigleWifi-1.6,appRelease=1.3\n";
static const char* header="MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type\n";
static const char* row="AA:BB:CC:DD:EE:FF,\"Cafe, \"\"hi\"\"\nsecond line\",[WPA2-PSK][ESS],2026-09-23 12:00:00,6,2437,-67,48.1173000,11.5166667,545,0,,,WIFI\n";
static void write_fixture(const char* name,const char* data) { char path[600]; snprintf(path,sizeof(path),"%s/%s",directory,name); FILE* f=fopen(path,"wb"); CHECK(f); CHECK(fwrite(data,1,strlen(data),f)==strlen(data)); CHECK(!fclose(f)); }
static void run(WardWigleUpload* j) { wardriver_wigle_upload(j); CHECK(!files && !records && !clients); CHECK(!strstr(j->result,"test-not-a-secret")); }
int main(int argc,char** argv) {
    CHECK(argc==2); directory=argv[1];
    WardWigleCredentials c={.name="test-name",.token="test-not-a-secret"},loaded;
    CHECK(wardriver_wigle_credentials_save(&c)); CHECK(wardriver_wigle_credentials_load(&loaded)); CHECK(!memcmp(&c,&loaded,sizeof(c)));
    sync_fail=true; strcpy(loaded.token,"replacement"); CHECK(!wardriver_wigle_credentials_save(&loaded)); sync_fail=false;
    CHECK(wardriver_wigle_credentials_load(&loaded) && !strcmp(loaded.token,c.token));
    rename_fail=true; CHECK(!wardriver_wigle_credentials_save(&c)); rename_fail=false;
    CHECK(wardriver_wigle_credentials_load(&loaded));
    CHECK(wardriver_wigle_credentials_clear()); CHECK(!wardriver_wigle_credentials_load(&loaded) && !loaded.name[0] && !loaded.token[0]);
    write_fixture("wigle.conf","API Name: ok\nAPI Token: bad token\n"); CHECK(!wardriver_wigle_credentials_load(&loaded));
    CHECK(wardriver_wigle_value_valid("",true)); CHECK(!wardriver_wigle_value_valid("bad:name",true));
    CHECK(!wardriver_wigle_value_valid("bad\nHeader: injected",false));
    char csv[6000]; snprintf(csv,sizeof(csv),"%s%s%s%s",metadata,header,row,row);
    WardWigleCsv parser={0}; for(size_t i=0;i<strlen(csv);++i) CHECK(wardriver_wigle_csv_feed(&parser,csv+i,1)); CHECK(wardriver_wigle_csv_finish(&parser) && parser.rows==2);
    char bad[6000]; strcpy(bad,csv); char* lat=strstr(bad,"48.1173000"); memcpy(lat,"99.1173000",10);
    parser=(WardWigleCsv){0}; CHECK(!wardriver_wigle_csv_feed(&parser,bad,strlen(bad)));
    parser=(WardWigleCsv){0}; CHECK(wardriver_wigle_csv_feed(&parser,metadata,strlen(metadata))); CHECK(wardriver_wigle_csv_feed(&parser,header,strlen(header))); CHECK(!wardriver_wigle_csv_finish(&parser));
    bool ok,warn;
    CHECK(wardriver_wigle_response(response,strlen(response),&ok,&warn) && ok && !warn);
    const char* rejected[]={"{\"message\":\"success: true\"}","{\"success\":true,}","{\"success\":true,\"success\":false}","{\"success\":\"true\"}","{\"success\":true}trailing","{\"success\":true,\"n\":01}"};
    for(unsigned i=0;i<sizeof(rejected)/sizeof(*rejected);++i) CHECK(!wardriver_wigle_response(rejected[i],strlen(rejected[i]),&ok,&warn));
    write_fixture("wardrive_1790164800_abcd1234_wigle.csv",csv);
    WardWigleUpload job={.credentials=c,.cancelled=cancel,.progress=progress,.context=&c};
    strcpy(job.path,WARD_WIGLE_DIR "wardrive_1790164800_abcd1234_wigle.csv");
    CHECK(wardriver_wigle_path_valid(job.path)); CHECK(!wardriver_wigle_path_valid(WARD_WIGLE_DIR "../wigle.conf"));
    run(&job); CHECK(job.accepted && !job.uncertain && written==expected);
    CHECK(strstr(request,csv) && !strstr(request,"test-not-a-secret") && !strstr(request,"donate"));
    write_fixture("multipart-request.txt",request); write_fixture("multipart-type.txt",content_type);
    unsigned old=requests; online=false; run(&job); CHECK(!job.accepted && requests==old); online=true;
    dispatch=false; run(&job); CHECK(!job.accepted && requests==old); dispatch=true;
    read_fail=true; run(&job); CHECK(!job.accepted && requests==old); read_fail=false;
    open_fail=true; run(&job); CHECK(!job.accepted); open_fail=false;
    write_fail=true; run(&job); CHECK(!job.accepted && job.uncertain); write_fail=false;
    fetch_fail=true; run(&job); CHECK(!job.accepted && job.uncertain); fetch_fail=false;
    status=401; run(&job); CHECK(!job.accepted && !strcmp(job.result,"API credentials rejected"));
    status=302; run(&job); CHECK(!job.accepted); status=200;
    response="{\"success\":false}"; run(&job); CHECK(!job.accepted && !job.uncertain);
    response="{\"success\":true,\"warning\":\"check account\"}"; run(&job); CHECK(job.accepted && strstr(job.result,"warning"));
    response="{\"success\":true"; run(&job); CHECK(!job.accepted && job.uncertain);
    cancel_flag=true; old=requests; run(&job); CHECK(!job.accepted && requests==old); cancel_flag=false;
    cancel_on_progress=true; run(&job); CHECK(!job.accepted && job.uncertain); cancel_on_progress=false; cancel_flag=false;
    write_fixture("wardrive_1790164800_abcd1234_wigle.csv",bad); old=requests; run(&job); CHECK(!job.accepted && requests==old);
    CHECK(!records && !files && !clients && closes==requests);
    puts("PASS: actual WiGLE HTTPS/upload/config/CSV/JSON; partial writes, certificate/redirect settings, cancellation, failed reads/auth/transport, no automatic retry, complete cleanup");
    return 0;
}
