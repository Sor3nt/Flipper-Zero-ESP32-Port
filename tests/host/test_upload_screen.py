"""Execute the real unified-screen worker, mocking only credential/network I/O."""
from pathlib import Path
from host_build import run
root=Path(__file__).resolve().parents[2]
app=root/'applications_user/wardriver'
source=(app/'wardriver.c').read_text(encoding='utf-8')
source=source[source.index('static void wigle_process('):source.index('static int32_t wardriver_worker(')]
prefix=r'''
#include "wardriver.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned saves[2],loads[2],clears[2],batches;
static bool load_ok[2]={true,true},clear_ok[2]={true,true};
static WardWigleCredentials fixtures[2];
FuriStatus furi_mutex_acquire(FuriMutex* m,uint32_t n) { (void)m;(void)n;return FuriStatusOk; }
FuriStatus furi_mutex_release(FuriMutex* m) { (void)m;return FuriStatusOk; }
void view_port_update(ViewPort* v) { (void)v; }
static bool wigle_cancelled(void* p) { (void)p;return false; }
static void wigle_progress(void* p,const char* s,unsigned n) { (void)p;(void)s;(void)n; }
void wardriver_wigle_erase(void* p,size_t n) { memset(p,0,n); }
bool wardriver_wdg_key_valid(const char* s) { return strlen(s)==64; }
bool wardriver_wigle_credentials_save(const WardWigleCredentials* c) { ++saves[0];assert(!memcmp(c,&fixtures[0],sizeof(*c)));return true; }
bool wardriver_wdg_credentials_save(const WardWigleCredentials* c) { ++saves[1];assert(!memcmp(c,&fixtures[1],sizeof(*c)));return true; }
static bool load(WardWigleCredentials* c,unsigned i) { ++loads[i];memset(c,0,sizeof(*c));if(load_ok[i])*c=fixtures[i];return load_ok[i]; }
bool wardriver_wigle_credentials_load(WardWigleCredentials* c) { return load(c,0); }
bool wardriver_wdg_credentials_load(WardWigleCredentials* c) { return load(c,1); }
bool wardriver_wigle_credentials_clear(void) { ++clears[0];return clear_ok[0]; }
bool wardriver_wdg_credentials_clear(void) { ++clears[1];return clear_ok[1]; }
void wardriver_upload_both(WardWigleUpload* a,WardWigleUpload* b) {
    ++batches;assert(!memcmp(&a->credentials,&fixtures[0],sizeof(fixtures[0])));
    assert(!memcmp(&b->credentials,&fixtures[1],sizeof(fixtures[1])));
    assert(!strcmp(a->path,b->path) && a->path[0]);
    assert(a->context==b->context && a->cancelled==b->cancelled && a->progress==b->progress);
    strcpy(a->result,"Accepted by WiGLE");strcpy(b->result,"Accepted by WDGWars");
}
'''
suffix=r'''
int main(void) {
    strcpy(fixtures[0].name,"test-name");strcpy(fixtures[0].token,"test-token");
    memset(fixtures[1].token,'a',64);
    WardriverApp a={0};
    wigle_process(&a,WardWigleReload);
    assert(loads[0]==1 && loads[1]==1 && a.model.wigle_name_set && a.model.wigle_token_set && a.model.wdg_key_set);
    wigle_process(&a,WardWigleSave);assert(saves[0]==1 && saves[1]==0);
    wigle_process(&a,WardWdgSave);assert(saves[0]==1 && saves[1]==1);
    strcpy(a.model.wigle_path,WARD_WIGLE_DIR "wardrive_1_00000001_wigle.csv");
    a.model.wigle_busy=a.model.wigle_uploading=true;
    wigle_process(&a,WardWigleSend);
    assert(batches==1 && a.model.batch_result && !a.model.wigle_busy && !a.model.wigle_uploading);
    assert(strstr(a.model.upload_results[0],"WiGLE") && strstr(a.model.upload_results[1],"WDGWars"));
    load_ok[1]=false;wigle_process(&a,WardWigleReload);
    assert(a.model.wigle_token_set && !a.model.wdg_key_set && strstr(a.model.wigle_status,"set WDGWars"));
    load_ok[0]=false;load_ok[1]=true;wigle_process(&a,WardWigleReload);
    assert(!a.model.wigle_token_set && a.model.wdg_key_set && strstr(a.model.wigle_status,"set WiGLE"));
    load_ok[0]=true;wigle_process(&a,WardWigleReload);clear_ok[0]=false;
    wigle_process(&a,WardWigleClear);
    assert(clears[0]==1 && clears[1]==1 && !a.model.wigle_token_set && !a.model.wdg_key_set);
    assert(strstr(a.model.wigle_status,"SD delete failed"));
    puts("PASS: actual unified worker reloads both keys, saves only edited service, clears both, sends both and preserves separate results");
    return 0;
}
'''
out=root/'build_t_embed/host_tests';out.mkdir(parents=True,exist_ok=True)
test=out/'upload_screen_test.c';test.write_text(prefix+source+suffix,encoding='utf-8')
binary=out/'upload_screen_test'
run(['gcc','-std=c17','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined',
     '-I'+str(root/'tests/host/wardriver_host'),'-I'+str(app),'-I'+str(root/'components/u8g2'),str(test),'-o',str(binary)],check=True)
run([str(binary)],check=True)
