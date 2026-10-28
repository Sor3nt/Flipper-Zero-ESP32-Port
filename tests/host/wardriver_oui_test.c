/* Production incremental index with an in-memory, fragmented storage source. */
#include "wardriver_oui.h"
#include <storage/storage.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct File { size_t offset; };
static char source[8U*1024*1024];
static size_t length,reads,open_files,records,allocation_limit=(size_t)-1;
static bool missing,read_error;
#define CHECK(c) do { if(!(c)) { fprintf(stderr,"OUI FAIL line %d: %s\n",__LINE__,#c); exit(1); } } while(0)
void* furi_record_open(const char* s) { (void)s; ++records; return (void*)1; }
void furi_record_close(const char* s) { (void)s; --records; }
File* storage_file_alloc(Storage* s) { (void)s; ++open_files; return calloc(1,sizeof(File)); }
bool storage_file_open(File* f,const char* p,int a,int m) { (void)f; (void)p; (void)a; (void)m; return !missing; }
bool storage_file_close(File* f) { (void)f; return true; }
void storage_file_free(File* f) { --open_files; free(f); }
FS_Error storage_file_get_error(File* f) { (void)f; return read_error ? 2 : FSE_OK; }
size_t storage_file_read(File* f,void* out,size_t n) {
    ++reads; if(n>1027) n=1027; if(n>length-f->offset) n=length-f->offset;
    memcpy(out,source+f->offset,n); f->offset+=n; return n;
}
void* heap_caps_malloc(size_t n,unsigned caps) { (void)caps; return n>allocation_limit?NULL:malloc(n); }
static void fixture(void) {
    length=0;
    for(unsigned i=0;i<750;++i) length+=(size_t)sprintf(source+length,"%06X\tVendor %u\n",(749-i)*4,i);
}
int main(int argc,char** argv) {
    fixture(); WardriverOui* t=wardriver_oui_open(); CHECK(t && !reads && !wardriver_oui_ready(t));
    char vendor[29]; uint8_t mac[6]={0,0,4,0,0,0};
    wardriver_oui_lookup(t,mac,vendor); CHECK(!vendor[0]);
    CHECK(!wardriver_oui_step(t) && reads==1); /* One read, even when data remains. */
    for(unsigned i=0;!wardriver_oui_step(t);++i) CHECK(i<30);
    CHECK(wardriver_oui_ready(t) && !open_files && !records);
    wardriver_oui_lookup(t,mac,vendor); CHECK(!strcmp(vendor,"Vendor 748"));
    mac[0]=2; wardriver_oui_lookup(t,mac,vendor); CHECK(!strcmp(vendor,"Local / randomized MAC"));
    wardriver_oui_free(t);
    t=wardriver_oui_open(); CHECK(t); wardriver_oui_step(t); wardriver_oui_free(t);
    CHECK(!records && !open_files); /* Exit midway through loading closes everything. */
    t=wardriver_oui_open(); CHECK(t); wardriver_oui_step(t); read_error=true;
    CHECK(wardriver_oui_step(t) && !wardriver_oui_ready(t));
    mac[0]=0; wardriver_oui_lookup(t,mac,vendor); CHECK(!vendor[0]);
    wardriver_oui_free(t); read_error=false;
    missing=true; CHECK(!wardriver_oui_open() && !records && !open_files); missing=false;
    allocation_limit=0; CHECK(!wardriver_oui_open() && !records && !open_files);
    allocation_limit=(size_t)-1;
    memset(source,'Z',300); strcpy(source+300,"\n001122\tValid after oversized line\n"); length=strlen(source);
    t=wardriver_oui_open(); while(!wardriver_oui_step(t)) {};
    memcpy(mac,"\x00\x11\x22\x00\x00\x00",6); wardriver_oui_lookup(t,mac,vendor);
    CHECK(!strcmp(vendor,"Valid after oversized line")); wardriver_oui_free(t);
    CHECK(!records && !open_files);
    puts("PASS: incremental production OUI index, bounded reads, lookup, cancellation and allocation/missing-file failures");
    if(argc==2) {
        FILE* input=fopen(argv[1],"rb"); CHECK(input);
        length=fread(source,1,sizeof(source),input); CHECK(length && length<sizeof(source) && !ferror(input));
        fclose(input); reads=0;
        t=wardriver_oui_open(); CHECK(t);
        while(!wardriver_oui_step(t)) CHECK(reads<10000);
        CHECK(wardriver_oui_ready(t));
        memcpy(mac,"\x00\x00\x00\x12\x34\x56",6); wardriver_oui_lookup(t,mac,vendor); CHECK(!strcmp(vendor,"XEROX CORPORATION"));
        memcpy(mac,"\x3c\x5a\x37\x12\x34\x56",6); wardriver_oui_lookup(t,mac,vendor); CHECK(!strcmp(vendor,"Samsung Electronics Co. Ltd"));
        memcpy(mac,"\xf4\xf5\xd8\x12\x34\x56",6); wardriver_oui_lookup(t,mac,vendor); CHECK(!strcmp(vendor,"Google Inc."));
        wardriver_oui_free(t); CHECK(!records && !open_files);
        puts("PASS: packaged full OUI database loaded by production C index; known vendor lookups verified");
    }
    return 0;
}
