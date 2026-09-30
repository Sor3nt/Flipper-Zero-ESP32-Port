#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdatomic.h>
#ifdef _WIN32
#include <windows.h>
typedef CRITICAL_SECTION FuriMutex;
#else
#include <pthread.h>
typedef pthread_mutex_t FuriMutex;
#endif
#include "m-array.h"
#define furi_check(x) assert(x)
#define UNUSED(x) (void)(x)
#define FURI_LOG_I(...) ((void)0)
#define FURI_LOG_E(...) ((void)0)
#define FuriMutexTypeRecursive 1
#define FuriStatusOk 0
#define FuriWaitForever UINT32_MAX
#define RECORD_STORAGE "storage"
static FuriMutex* furi_mutex_alloc(int type) {
    assert(type==FuriMutexTypeRecursive); FuriMutex* m=malloc(sizeof(*m)); assert(m);
#ifdef _WIN32
    InitializeCriticalSection(m);
#else
    pthread_mutexattr_t a; pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a,PTHREAD_MUTEX_RECURSIVE); pthread_mutex_init(m,&a); pthread_mutexattr_destroy(&a);
#endif
    return m;
}
static int furi_mutex_acquire(FuriMutex* m,uint32_t timeout) {
    assert(m && timeout==FuriWaitForever);
#ifdef _WIN32
    EnterCriticalSection(m);
#else
    assert(!pthread_mutex_lock(m));
#endif
    return 0;
}
static int furi_mutex_release(FuriMutex* m) {
#ifdef _WIN32
    LeaveCriticalSection(m);
#else
    assert(!pthread_mutex_unlock(m));
#endif
    return 0;
}
static void furi_mutex_free(FuriMutex* m) {
#ifdef _WIN32
    DeleteCriticalSection(m);
#else
    pthread_mutex_destroy(m);
#endif
    free(m);
}
static atomic_int strings_live;
typedef struct { char* s; size_t n,cap; } FuriString;
static FuriString* furi_string_alloc(void) { FuriString* s=calloc(1,sizeof(*s)); assert(s);s->s=calloc(1,1);s->cap=1;strings_live++;return s; }
static void furi_string_free(FuriString* s) { assert(s);free(s->s);free(s);strings_live--; }
static void furi_string_reserve(FuriString* s,size_t n) { if(n+1>s->cap){s->s=realloc(s->s,n+1);assert(s->s);s->cap=n+1;} }
static void furi_string_set_strn(FuriString* s,const char* t,size_t n) { furi_string_reserve(s,n);memcpy(s->s,t,n);s->s[n]=0;s->n=n; }
static void furi_string_set_str(FuriString* s,const char* t) { furi_string_set_strn(s,t,strlen(t)); }
#define furi_string_set furi_string_set_str
static void furi_string_reset(FuriString* s) { s->n=0;s->s[0]=0; }
static const char* furi_string_get_cstr(const FuriString* s) { return s->s; }
static size_t furi_string_size(const FuriString* s) { return s->n; }
static void furi_string_cat_str(FuriString* s,const char* t) { size_t n=strlen(t);furi_string_reserve(s,s->n+n);memcpy(s->s+s->n,t,n+1);s->n+=n; }
static void furi_string_move(FuriString* d,FuriString* s) { free(d->s);*d=*s;free(s);strings_live--; }
static void furi_string_printf(FuriString* s,const char* fmt,...) { assert(fmt);va_list a,b;va_start(a,fmt);va_copy(b,a);int n=vsnprintf(NULL,0,fmt,a);va_end(a);assert(n>=0);furi_string_reserve(s,(size_t)n);vsnprintf(s->s,s->cap,fmt,b);va_end(b);s->n=(size_t)n; }
typedef struct { int dummy; } Storage;
static Storage storage;
static void* furi_record_open(const char* name) { assert(!strcmp(name,RECORD_STORAGE));return &storage; }
static void furi_record_close(const char* name) { assert(!strcmp(name,RECORD_STORAGE)); }
static uint32_t furi_get_tick(void) { return 1000; }
typedef struct { uint32_t sequence; atomic_bool alive; size_t bytes; } FlipperFormat;
typedef FlipperFormat Stream;
typedef struct { uint8_t id; } SubGhzProtocolDecoderBase;
typedef struct { int unused; } SubGhzRadioPreset;
typedef enum { SubGhzProtocolStatusOk, SubGhzProtocolStatusError } SubGhzProtocolStatus;
static FlipperFormat* allocations[4096];static size_t allocated;static atomic_int files_live;
static bool fail_save,fail_serialize,fail_copy;
static unsigned save_calls;
static size_t serialized_size = 256;
static FlipperFormat* flipper_format_string_alloc(void) { FlipperFormat* f=calloc(1,sizeof(*f));assert(f);f->alive=true;assert(allocated<4096);allocations[allocated++]=f;files_live++;return f; }
static FlipperFormat* flipper_format_file_alloc(Storage* s) { assert(s==&storage);return flipper_format_string_alloc(); }
static void flipper_format_free(FlipperFormat* f) { assert(f && atomic_exchange(&f->alive,false));files_live--; /* Retain tombstone for deterministic UAF detection. */ }
static bool flipper_format_file_open_existing(FlipperFormat* f,const char* path) { assert(f->alive);unsigned seq;assert(sscanf(path,"history/%u",&seq)==1);f->sequence=seq;return true; }
static Stream* flipper_format_get_raw_stream(FlipperFormat* f) { assert(f->alive);return f; }
static size_t stream_size(Stream* s) { return s->bytes; }
#define StreamOffsetFromStart 0
static bool stream_seek(Stream* s,int32_t offset,int origin) { assert(s->alive && offset==0 && origin==0);return true; }
static size_t stream_copy(Stream* src,Stream* dst,size_t size) { assert(src->alive && dst->alive && size==src->bytes);if(fail_copy)return 0;dst->sequence=src->sequence;dst->bytes=src->bytes;return src->bytes; }
static bool flipper_format_rewind(FlipperFormat* f) { assert(f->alive);return true; }
static uint8_t subghz_protocol_decoder_base_get_hash_data(SubGhzProtocolDecoderBase* d) { return d->id; }
static void subghz_protocol_decoder_base_get_string(SubGhzProtocolDecoderBase* d,FuriString* s) { char text[240];memset(text,'a'+d->id%26,sizeof(text));text[230]=0;furi_string_printf(s,"Mock %u\n%s",d->id,text); }
static SubGhzProtocolStatus subghz_protocol_decoder_base_serialize(SubGhzProtocolDecoderBase* d,FlipperFormat* f,SubGhzRadioPreset* p) { f->sequence=d->id;f->bytes=serialized_size;UNUSED(p);return fail_serialize?SubGhzProtocolStatusError:SubGhzProtocolStatusOk; }
static void protopirate_storage_build_history_path(uint32_t seq,FuriString* s) { furi_string_printf(s,"history/%u",seq); }
static void protopirate_storage_delete_file(const char* s) { assert(s && !strncmp(s,"history/",8)); }
static void protopirate_storage_wipe_history_cache(void) {}
static bool protopirate_storage_save_history_capture(FlipperFormat* f,uint32_t seq,FuriString* s) { assert(f->alive);save_calls++;protopirate_storage_build_history_path(seq,s);return !fail_save; }
#include "history_under_test.h"
#include "history_under_test.inc"
static ProtoPirateHistory* active;
static atomic_bool done;
static void append_records(void) {
    SubGhzRadioPreset preset={0};
    for(unsigned i=1;i<PROTOPIRATE_HISTORY_MAX;i++) {
        SubGhzProtocolDecoderBase d={(uint8_t)i};
        assert(protopirate_history_add_to_history_at(active,&d,&preset,1000+i*1000));
        protopirate_history_mark_auto_save_pending(active,(uint16_t)i);
        protopirate_history_mark_saved_match_pending(active,(uint16_t)i);
    }
    done=true;
}
#ifdef _WIN32
static DWORD WINAPI producer(LPVOID p) { UNUSED(p);append_records();return 0; }
#else
static void* producer(void* p) { UNUSED(p);append_records();return NULL; }
#endif
int main(void) {
    active=protopirate_history_alloc();SubGhzRadioPreset preset={0};SubGhzProtocolDecoderBase d={0};
    assert(protopirate_history_add_to_history_at(active,&d,&preset,1000));
    FlipperFormat* borrowed=protopirate_history_get_raw_data(active,0);assert(borrowed && borrowed->alive);
    uint32_t original_sequence=borrowed->sequence;
#ifdef _WIN32
    HANDLE worker=CreateThread(NULL,0,producer,NULL,0,NULL);assert(worker);
#else
    pthread_t worker;assert(!pthread_create(&worker,NULL,producer,NULL));
#endif
    FuriString* text=furi_string_alloc();
    do {
        /* UI keeps reading a file while new RX packets arrive and the GUI
         * copies labels from an arena which grows past its initial reserve. */
        assert(borrowed->alive && borrowed->sequence==original_sequence);
        protopirate_history_get_text_item_menu(active,text,0);assert(strstr(text->s,"Mock 0"));
        uint16_t idx=0;
        if(protopirate_history_find_pending_auto_save(active,&idx)) protopirate_history_mark_auto_save_done(active,idx);
        if(protopirate_history_find_pending_saved_match(active,&idx)) {
            protopirate_history_set_matched_saved(active,idx,"test","test/path");
            protopirate_history_mark_saved_match_done(active,idx);
        }
    } while(!done);
#ifdef _WIN32
    assert(WaitForSingleObject(worker,10000)==WAIT_OBJECT_0);CloseHandle(worker);
#else
    pthread_join(worker,NULL);
#endif
    assert(borrowed->alive); /* Before fix, every admitted RX freed this object. */
    assert(protopirate_history_get_raw_data(active,0)==borrowed);
    assert(protopirate_history_get_item(active)==PROTOPIRATE_HISTORY_MAX);
    assert(!protopirate_history_add_to_history_at(active,&d,&preset,99000));assert(borrowed->alive);
    protopirate_history_delete_item(active,19);assert(borrowed->alive);
    fail_serialize=true;assert(!protopirate_history_add_to_history_at(active,&d,&preset,100000));assert(borrowed->alive);fail_serialize=false;
    fail_save=true;assert(!protopirate_history_add_to_history_at(active,&d,&preset,101000));assert(borrowed->alive);fail_save=false;
    protopirate_history_release_scratch(active);assert(!borrowed->alive);
    borrowed=protopirate_history_get_raw_data(active,0);assert(borrowed->alive);
    FlipperFormat* next=protopirate_history_get_raw_data(active,1);assert(next->alive && !borrowed->alive);
    protopirate_history_delete_item(active,1);assert(!next->alive);
    protopirate_history_reset(active);assert(protopirate_history_get_item(active)==0);
    assert(protopirate_history_get_raw_data(active,0)==NULL);
    d.id=42;assert(protopirate_history_add_to_history_at(active,&d,&preset,200000));
    borrowed=protopirate_history_get_raw_data(active,0);assert(!protopirate_history_add_to_history_at(active,&d,&preset,200001));assert(borrowed->alive);
    protopirate_history_free(active);furi_string_free(text);
    assert(files_live==0 && strings_live==0);
    /* Rapid live RX must neither touch storage nor invalidate a borrowed reader. */
    active=protopirate_history_alloc();protopirate_history_enable_memory(active);
    unsigned before=save_calls;fail_save=true;
    d.id=1;assert(protopirate_history_add_to_history_at(active,&d,&preset,300000));
    borrowed=protopirate_history_get_raw_data(active,0);assert(borrowed->sequence==1);
    for(unsigned i=2;i<=PROTOPIRATE_HISTORY_MAX;i++) {
        d.id=i;assert(protopirate_history_add_to_history_at(active,&d,&preset,300000+i));
        assert(borrowed->alive && borrowed->sequence==1);
    }
    assert(save_calls==before); /* Receive succeeds even with failing/unavailable SD. */
    assert(!protopirate_history_add_to_history_at(active,&d,&preset,300100));
    FuriString* path=furi_string_alloc();
    assert(!protopirate_history_get_capture_path(active,0,path));
    fail_save=false;assert(protopirate_history_get_capture_path(active,0,path));
    before=save_calls;assert(protopirate_history_get_capture_path(active,0,path));assert(before==save_calls);
    protopirate_history_release_scratch(active);fail_copy=true;
    assert(!protopirate_history_get_raw_data(active,1));fail_copy=false;
    borrowed=protopirate_history_get_raw_data(active,1);assert(borrowed->sequence==2);
    protopirate_history_delete_item(active,1);assert(!borrowed->alive);
    serialized_size=4097;d.id=42;
    assert(!protopirate_history_add_to_history_at(active,&d,&preset,400000));serialized_size=256;
    protopirate_history_reset(active);assert(files_live==0);
    assert(protopirate_history_add_to_history_at(active,&d,&preset,500000));
    protopirate_history_free(active);furi_string_free(path);
    assert(files_live==0 && strings_live==0);
    puts("PASS: 20 rapid RAM captures without SD IO, stable borrowed reader, lazy persistence, size bounds and cleanup");
    for(size_t i=0;i<allocated;i++)free(allocations[i]);
    puts("PASS: reader survives concurrent RX, GUI/index reads, full/duplicate/failure paths; switch/delete/reset/free release readers without leaks");
}
