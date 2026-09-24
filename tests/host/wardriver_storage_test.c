/* Exercise the production buffered CSV writer with a fault-injecting file backend. */
#include "wardriver_storage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct File { FILE* stream; };
static const char* directory;
static unsigned writes,syncs,closes,phases;
static bool fail_open,fail_write,fail_sync;
#define CHECK(c) do { if(!(c)) { fprintf(stderr,"storage FAIL line %d: %s\n",__LINE__,#c); exit(1); } } while(0)
void* furi_record_open(const char* name) { (void)name; return (void*)1; }
void furi_record_close(const char* name) { (void)name; }
File* storage_file_alloc(Storage* s) { (void)s; return calloc(1,sizeof(File)); }
bool storage_file_open(File* f,const char* path,int a,int m) {
    (void)a; (void)m; char dest[1024];
    snprintf(dest,sizeof(dest),"%s/%s",directory,strrchr(path,'/')+1);
    if(fail_open) return false;
    f->stream=fopen(dest,"wbx"); return f->stream!=NULL;
}
size_t storage_file_write(File* f,const void* data,size_t n) {
    ++writes; if(fail_write) return 0; return fwrite(data,1,n,f->stream);
}
bool storage_file_sync(File* f) { ++syncs; return !fail_sync && fflush(f->stream)==0; }
bool storage_file_close(File* f) { ++closes; bool ok=!f->stream || fclose(f->stream)==0; f->stream=NULL; return ok; }
static void closing(void* context,const char* stage) { CHECK(context && stage[0]); ++phases; }
void storage_file_free(File* f) { free(f); }
bool storage_simply_mkdir(Storage* s,const char* path) { (void)s; (void)path; return true; }
int main(int argc,char** argv) {
    CHECK(argc==2); directory=argv[1];
    WardriverNetwork n={.observation={.mac={0,1,2,3,4,5},.channel=6,.auth=3,.rssi=-67},
        .first_seen=1790118000,.last_seen=1790118001,.detections=1};
    strcpy(n.observation.ssid,"Cafe, \"hello\"\nWiFi");
    WardriverLog* log=wardriver_log_open(1790118000,1);
    CHECK(wardriver_log_ok(log)); CHECK(wardriver_log_record(log,&n)); CHECK(writes==0);
    CHECK(wardriver_log_flush(log)); CHECK(writes==1);
    n.gps=(WardriverGpsData){.has_fix=true,.timestamp_valid=true,.timestamp=1790118001,
        .latitude=48.1173,.longitude=11.5166667,.hdop_valid=true,.hdop=0.9,
        .satellites_valid=true,.satellites=9,.altitude_valid=true,.altitude=545.4};
    CHECK(wardriver_log_record(log,&n)); CHECK(writes==1); CHECK(wardriver_log_flush(log));
    n.gps.has_fix=false; CHECK(wardriver_log_record(log,&n));
    n.gps.has_fix=true; n.observation.mode=WardriverBle; CHECK(wardriver_log_record(log,&n));
    CHECK(wardriver_log_flush(log)); unsigned synced=syncs;
    CHECK(wardriver_log_close(log,closing,&n));
    CHECK(syncs==synced && closes==2 && phases>=3); /* No redundant SD sync at exit. */
    fail_open=true; log=wardriver_log_open(1790118000,2);
    CHECK(!wardriver_log_ok(log)); wardriver_log_close(log,NULL,NULL); fail_open=false;
    log=wardriver_log_open(1790118000,3); fail_write=true;
    CHECK(!wardriver_log_flush(log)); CHECK(!wardriver_log_record(log,&n));
    wardriver_log_close(log,NULL,NULL); fail_write=false;
    log=wardriver_log_open(1790118000,4); fail_sync=true;
    CHECK(!wardriver_log_flush(log)); CHECK(!wardriver_log_ok(log));
    wardriver_log_close(log,NULL,NULL); fail_sync=false;
    puts("PASS: production buffered logger, no-fix/BLE separation, open/write/sync faults");
    return 0;
}
