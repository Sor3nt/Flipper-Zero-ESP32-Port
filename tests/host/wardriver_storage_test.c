/* Production logger: no SD activity without a valid fix, one shared upload CSV. */
#include "wardriver_storage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
struct File { FILE* stream; };
static const char* directory;
static unsigned writes,syncs,closes,phases,opens,files,records;
static bool fail_open,fail_write,fail_sync,partial_write;
#define CHECK(c) do { if(!(c)) { fprintf(stderr,"storage FAIL line %d: %s\n",__LINE__,#c); exit(1); } } while(0)
void* furi_record_open(const char* name) { (void)name; ++records; return (void*)1; }
void furi_record_close(const char* name) { (void)name; CHECK(records); --records; }
File* storage_file_alloc(Storage* s) { (void)s; ++files; return calloc(1,sizeof(File)); }
bool storage_file_open(File* f,const char* path,int a,int m) {
    CHECK(a==FSAM_WRITE && m==FSOM_CREATE_NEW); ++opens; char dest[1024];
    CHECK(strstr(path,"_wigle.csv") && !strstr(path,"_local.csv"));
    snprintf(dest,sizeof(dest),"%s/%s",directory,strrchr(path,'/')+1);
    if(fail_open) return false;
    f->stream=fopen(dest,"wbx"); return f->stream!=NULL;
}
size_t storage_file_write(File* f,const void* data,size_t n) {
    CHECK(f->stream); ++writes; if(fail_write) return 0;
    return fwrite(data,1,partial_write?n/2:n,f->stream);
}
FS_Error storage_file_get_error(File* f) { return ferror(f->stream)?FSE_INTERNAL:FSE_OK; }
bool storage_file_sync(File* f) { ++syncs; return !fail_sync && fflush(f->stream)==0; }
bool storage_file_close(File* f) { CHECK(f->stream); ++closes; bool ok=fclose(f->stream)==0; f->stream=NULL; return ok; }
static void closing(void* context,const char* stage) { CHECK(context && stage[0] && !strstr(stage,"LOCAL")); ++phases; }
void storage_file_free(File* f) { CHECK(!f->stream && files); --files; free(f); }
bool storage_simply_mkdir(Storage* s,const char* path) { CHECK(s && path); return true; }
int main(int argc,char** argv) {
    CHECK(argc==2); directory=argv[1];
    WardriverNetwork n={.observation={.mac={0,1,2,3,4,5},.channel=6,.auth=3,.rssi=-67},
        .first_seen=1,.last_seen=2,.detections=1};
    strcpy(n.observation.ssid,"Cafe, \"hello\"\nWiFi");
    WardriverLog* log=wardriver_log_open(1790118000,10);
    CHECK(wardriver_log_ok(log) && !wardriver_log_has_records(log));
    for(unsigned i=0;i<100;++i) { CHECK(wardriver_log_record(log,&n)); CHECK(wardriver_log_flush(log)); }
    CHECK(wardriver_log_close(log,closing,&n));
    CHECK(!opens && !writes && !syncs && !closes && !phases && !records && !files);

    log=wardriver_log_open(1790118000,1);
    n.gps=(WardriverGpsData){.has_fix=true,.timestamp_valid=true,.timestamp=1790118001,
        .latitude=48.1173,.longitude=11.5166667,.altitude_valid=true,.altitude=545.4};
    WardriverGpsData valid=n.gps; n.last_fix=valid;
    CHECK(wardriver_log_record(log,&n)); CHECK(opens==1 && !writes && wardriver_log_has_records(log));
    CHECK(wardriver_log_flush(log) && writes==1);
    n.gps.has_fix=false; CHECK(wardriver_log_record(log,&n));
    n.gps=valid; n.gps.timestamp_valid=false; CHECK(wardriver_log_record(log,&n));
    n.gps=valid; n.gps.timestamp=0; CHECK(wardriver_log_record(log,&n));
    n.gps=valid; n.gps.latitude=NAN; CHECK(wardriver_log_record(log,&n));
    n.gps=valid; n.gps.longitude=INFINITY; CHECK(wardriver_log_record(log,&n));
    n.gps=valid; n.gps.latitude=91; CHECK(wardriver_log_record(log,&n));
    n.gps=valid; n.gps.longitude=-181; CHECK(wardriver_log_record(log,&n));
    n.gps=valid; n.observation.mode=WardriverBle; CHECK(wardriver_log_record(log,&n));
    n.observation.mode=WardriverWifi; n.observation.channel=0; CHECK(wardriver_log_record(log,&n));
    CHECK(wardriver_log_flush(log) && writes==1);
    n.observation.channel=6; n.gps=valid; n.gps.timestamp+=30; n.gps.latitude=48.2;
    CHECK(wardriver_log_record(log,&n)); CHECK(opens==1);
    CHECK(wardriver_log_flush(log)); unsigned synced=syncs;
    CHECK(wardriver_log_close(log,closing,&n));
    CHECK(syncs==synced && closes==1 && !records && !files);
    /* A session with only a detected receiver/time, but no fix, creates nothing. */
    log=wardriver_log_open(1790118000,11); n.gps=valid; n.gps.has_fix=false;
    n.gps.enabled=n.gps.connected=true;
    CHECK(wardriver_log_record(log,&n) && wardriver_log_close(log,NULL,NULL)); CHECK(opens==1);
    n.gps=valid;
    fail_open=true; log=wardriver_log_open(1790118000,2);
    CHECK(wardriver_log_ok(log)); CHECK(!wardriver_log_record(log,&n));
    CHECK(!wardriver_log_ok(log) && !wardriver_log_close(log,NULL,NULL)); fail_open=false;
    log=wardriver_log_open(1790118000,3); CHECK(wardriver_log_record(log,&n)); fail_write=true;
    CHECK(!wardriver_log_flush(log)); CHECK(!wardriver_log_record(log,&n));
    CHECK(!wardriver_log_close(log,NULL,NULL)); fail_write=false;
    log=wardriver_log_open(1790118000,4); CHECK(wardriver_log_record(log,&n)); fail_sync=true;
    CHECK(!wardriver_log_flush(log) && !wardriver_log_ok(log));
    CHECK(!wardriver_log_close(log,NULL,NULL)); fail_sync=false;
    log=wardriver_log_open(1790118000,6); CHECK(wardriver_log_record(log,&n)); partial_write=true;
    CHECK(!wardriver_log_flush(log) && !wardriver_log_close(log,NULL,NULL)); partial_write=false;
    log=wardriver_log_open(1790118000,1); CHECK(!wardriver_log_record(log,&n));
    CHECK(!wardriver_log_close(log,NULL,NULL)); /* CREATE_NEW cannot overwrite session 1. */
    log=wardriver_log_open(1790118000,5); unsigned before=writes;
    for(unsigned i=0;i<500;++i) CHECK(wardriver_log_record(log,&n));
    CHECK(wardriver_log_close(log,NULL,NULL)); CHECK(writes-before<100);
    CHECK(!files && !records);
    puts("PASS: GPS-only single upload CSV; no-fix creates no file, fix loss/recovery, no stale last_fix, buffering, collision and I/O faults");
    return 0;
}
