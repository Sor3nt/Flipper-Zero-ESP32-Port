/* Production config loader, with storage reads that can fail without EOF. */
#include "wardriver_config.h"
#include <storage/storage.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct File { const char* data; };
static const char* primary;
static const char* backup;
static bool fail_read,fail_alloc;
static unsigned reads,allocs,frees;
#define CHECK(c) do { if(!(c)) { fprintf(stderr,"config FAIL line %d: %s\n",__LINE__,#c); exit(1); } } while(0)
void* furi_record_open(const char* name) { (void)name; return (void*)1; }
void furi_record_close(const char* name) { (void)name; }
File* storage_file_alloc(Storage* s) { (void)s; if(fail_alloc) return NULL; ++allocs; return calloc(1,sizeof(File)); }
bool storage_file_open(File* f,const char* path,int access,int mode) {
    CHECK(access==FSAM_READ && mode==FSOM_OPEN_EXISTING);
    f->data=strstr(path,".bak")?backup:primary; return f->data!=NULL;
}
size_t storage_file_read(File* f,void* buffer,size_t n) {
    ++reads; CHECK(reads<=2 && n<=512); if(fail_read) return 0;
    size_t length=strlen(f->data); if(length>n) length=n;
    memcpy(buffer,f->data,length); return length;
}
bool storage_file_close(File* f) { f->data=NULL; return true; }
void storage_file_free(File* f) { ++frees; free(f); }
/* Loading must never write. COFF linkers retain references from the uncalled
 * save function, so provide explicit fail-fast boundaries on Windows too. */
bool storage_simply_mkdir(Storage* s,const char* p) { (void)s; (void)p; abort(); }
size_t storage_file_write(File* f,const void* b,size_t n) { (void)f; (void)b; (void)n; abort(); }
bool storage_file_sync(File* f) { (void)f; abort(); }
int storage_common_stat(Storage* s,const char* p,FileInfo* i) { (void)s; (void)p; (void)i; abort(); }
int storage_common_rename(Storage* s,const char* a,const char* b) { (void)s; (void)a; (void)b; abort(); }
int storage_common_remove(Storage* s,const char* p) { (void)s; (void)p; abort(); }
static bool load(WardriverConfig* c) {
    reads=0; bool ok=wardriver_config_load(c); CHECK(allocs==frees); return ok;
}
int main(void) {
    const char* valid="Filetype: Wardriver settings\nVersion: 1\nGPS Mode: 2\nGPS UART: 2\n"
        "GPS Baud: 9600\nGPS RX: 43\nGPS TX: 4294967295\nIsolated UART Pins: true\n";
    WardriverConfig c;
    primary=valid; CHECK(load(&c)); CHECK(c.gps_mode==2 && c.gps_rx==43 && c.gps_tx==-1 && c.gps_baud==9600);
    fail_read=true; CHECK(!load(&c)); CHECK(!c.gps_mode && c.gps_rx==-1); fail_read=false;
    primary=NULL; backup=valid; CHECK(load(&c)); CHECK(c.gps_mode==2); backup=NULL;
    CHECK(!load(&c) && !c.gps_mode);
    primary="Version: 1\n"; CHECK(!load(&c) && !c.gps_mode);
    primary="GPS Mode: 42949672960\n"; CHECK(!load(&c));
    char data[700]; snprintf(data,sizeof(data),"%sGPS Mode: 0\n",valid); primary=data; CHECK(!load(&c));
    memset(data,'X',sizeof(data)-1); data[sizeof(data)-1]=0; CHECK(!load(&c));
    primary="Filetype: wrong\n"; backup=valid; CHECK(load(&c)); backup=NULL;
    primary=valid; fail_alloc=true; CHECK(!load(&c)); fail_alloc=false;
    /* External SD input cannot turn corruption into unbounded retries. */
    uint32_t random=7;
    for(unsigned i=0;i<5000;++i) {
        strcpy(data,valid);
        for(unsigned j=0;j<8;++j) {
            random=random*1664525U+1013904223U;
            data[random%strlen(valid)]=(char)(random>>24);
        }
        primary=data; load(&c);
    }
    puts("PASS: bounded production config reads, missing/failed SD, backup, invalid/oversized/duplicate input, 5,000 mutations");
    return 0;
}
