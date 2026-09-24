"""Exercise the actual FAP entry point under fragmented memory and allocation faults."""
from pathlib import Path
from host_build import run
root=Path(__file__).resolve().parents[2]
app=root/'applications_user/wardriver'
out=root/'build_t_embed/host_tests'; out.mkdir(parents=True,exist_ok=True)
production=(app/'wardriver.c').read_text(encoding='utf-8')
production=production[production.index('static void startup_error('):]
prefix=r'''
#include "wardriver.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#define RECORD_GUI "gui"
#define RECORD_DIALOGS "dialogs"
#define MALLOC_CAP_INTERNAL 4
#define MALLOC_CAP_8BIT 2
#define FuriMutexTypeNormal 0
#define GuiLayerFullscreen 0
#define furi_ms_to_ticks(n) (n)
#define FURI_LOG_I(...) do { ++info_logs; } while(0)
#define FURI_LOG_E(...) do { ++error_logs; } while(0)
typedef struct { unsigned fields; } DialogMessage;
typedef void DialogsApp;
static unsigned allocations,fail_at,live,dialogs,info_logs,error_logs,joined,started,records;
static bool attached,dialog_oom;
static size_t largest;
static WardriverApp* active;
static char reason[96];
static void* pointers[32];
static void* allocate(size_t n) {
    ++allocations; if(allocations==fail_at) return NULL;
    void* p=calloc(1,n); assert(p); assert(allocations<32);
    pointers[allocations]=p; ++live; return p;
}
static void release(void* p) {
    assert(p); bool found=false;
    for(unsigned i=1;i<32;++i) if(pointers[i]==p) { pointers[i]=NULL; found=true; break; }
    assert(found && live); --live; free(p);
}
static size_t heap_caps_get_free_size(unsigned caps) { assert(caps==6);return 64000; }
static size_t heap_caps_get_largest_free_block(unsigned caps) { assert(caps==6);return largest; }
static void* heap_caps_calloc(size_t count,size_t size,unsigned caps) {
    assert(caps==6 && count==1 && size==sizeof(WardriverApp));
    if(size>largest) return NULL;
    active=allocate(size);return active;
}
static FuriMutex* furi_mutex_alloc(int kind) { assert(kind==0);return allocate(32); }
void furi_mutex_free(FuriMutex* m) { assert(!attached);release(m); }
static FuriMessageQueue* furi_message_queue_alloc(unsigned count,unsigned size) {
    assert((count==16 && size==sizeof(InputEvent)) || (count==192 && size==sizeof(WardriverObservation)));
    return allocate(count*size);
}
static void furi_message_queue_free(FuriMessageQueue* q) { assert(!attached);release(q); }
static FuriStatus furi_message_queue_get(FuriMessageQueue* q,void* p,unsigned timeout) {
    assert(attached && started && q==active->inputs && p && timeout==100);
    active->finished=1;return (FuriStatus)1;
}
static ViewPort* view_port_alloc(void) { return allocate(32); }
static void view_port_free(ViewPort* v) { assert(!attached);release(v); }
void view_port_update(ViewPort* v) { assert(attached && v==active->view); }
void wardriver_ui_draw(Canvas* c,void* p) { (void)c;(void)p;abort(); }
void wardriver_ui_input(InputEvent* e,void* p) { (void)e;(void)p;abort(); }
void wardriver_ui_handle(WardriverApp* a,const InputEvent* e) { (void)a;(void)e;abort(); }
void wardriver_wigle_dialog(WardriverApp* a,unsigned action) { (void)a;(void)action;abort(); }
void wardriver_wigle_erase(void* p,size_t n) { memset(p,0,n); }
void wardriver_config_defaults(WardriverConfig* c) { memset(c,0,sizeof(*c));c->gps_rx=c->gps_tx=-1; }
static void view_port_draw_callback_set(ViewPort* v,void (*cb)(Canvas*,void*),void* a) { assert(v && cb==wardriver_ui_draw && a==active); }
static void view_port_input_callback_set(ViewPort* v,void (*cb)(InputEvent*,void*),void* a) { assert(v && cb==wardriver_ui_input && a==active); }
void* furi_record_open(const char* name) { assert(!strcmp(name,"gui") || !strcmp(name,"dialogs"));++records;return (void*)1; }
void furi_record_close(const char* name) { (void)name;assert(records);--records; }
static void gui_add_view_port(Gui* gui,ViewPort* view,int layer) {
    assert(gui && view && layer==0 && !attached);attached=true;
}
static void gui_remove_view_port(Gui* gui,ViewPort* view) {
    assert(gui && view && attached && joined);attached=false;
}
static int32_t wardriver_worker(void* p) { (void)p;abort(); }
static FuriThread* furi_thread_alloc_ex(const char* name,unsigned stack,int32_t (*cb)(void*),void* ctx) {
    assert(!strcmp(name,"Wardriver") && stack==16384 && cb==wardriver_worker && ctx==active);
    return allocate(32);
}
static void furi_thread_start(FuriThread* t) { assert(attached && t && !started);started=1; }
static void furi_thread_join(FuriThread* t) { assert(attached && t && started && active->finished);joined=1; }
static void furi_thread_free(FuriThread* t) { assert(joined);release(t); }
static void set_status(WardriverApp* a,const char* text) { assert(a==active && attached && !strcmp(text,"JOINING WORKER")); }
static DialogMessage* dialog_message_alloc(void) { return dialog_oom?NULL:allocate(sizeof(DialogMessage)); }
static void dialog_message_free(DialogMessage* m) { release(m); }
static void dialog_message_set_header(DialogMessage* m,const char* s,int x,int y,Align h,Align v) {
    assert(strstr(s,"1.3.2") && x==64 && y==1 && h==AlignCenter && v==AlignTop);m->fields|=1;
}
static void dialog_message_set_text(DialogMessage* m,const char* s,int x,int y,Align h,Align v) {
    assert(x==2 && y==18 && h==AlignLeft && v==AlignTop && strstr(s,"Free ") && strstr(s,"Largest "));
    snprintf(reason,sizeof(reason),"%s",s);m->fields|=2;
}
static void dialog_message_set_icon(DialogMessage* m,const void* p,int x,int y) { assert(!p && !x && !y);m->fields|=4; }
static void dialog_message_set_buttons(DialogMessage* m,const char* l,const char* c,const char* r) { assert(!l && !r && !strcmp(c,"Back"));m->fields|=8; }
static void dialog_message_show(DialogsApp* d,DialogMessage* m) {
    assert(d && m->fields==15 && !attached && !started && live==1 && records==1);++dialogs;
}
#define free(p) release(p)
'''
suffix=r'''
static void reset(void) {
    assert(!live && !records && !attached);
    allocations=fail_at=dialogs=info_logs=error_logs=joined=started=0;
    largest=4096;dialog_oom=false;reason[0]=0;
}
int main(void) {
    size_t fragmented[]={4096,8192,32768,49151,65536};
    for(unsigned i=0;i<sizeof(fragmented)/sizeof(fragmented[0]);++i) {
        reset();largest=fragmented[i];assert(wardriver_app(NULL)==0);
        assert(started && joined && !dialogs && !live && !records && !attached && info_logs==1);
    }
    const char* expected[]={"App memory failed","Mutex memory failed","Input queue failed",
        "Scan queue failed","View memory failed","Worker memory failed"};
    for(unsigned i=1;i<=6;++i) {
        reset();fail_at=i;assert(wardriver_app(NULL)==-1);
        assert(dialogs==1 && !strncmp(reason,expected[i-1],strlen(expected[i-1])));
        assert(!live && !records && !started && !attached && error_logs==1);
    }
    reset();largest=128;assert(wardriver_app(NULL)==-1 && dialogs==1 && !live);
    reset();fail_at=1;dialog_oom=true;assert(wardriver_app(NULL)==-1 && !dialogs && !live && error_logs==1);
    puts("PASS: actual Wardriver entry launches with 4/8/32 KiB fragmented internal blocks; normal exit cleans resources");
    puts("PASS: six startup allocation failures show the reason after cleanup; error-dialog OOM retains USB diagnostics");
}
'''
c=out/'wardriver_startup_test.c';c.write_text(prefix+production+suffix,encoding='utf-8')
binary=out/'wardriver_startup_test'
run(['gcc','-std=c17','-Wall','-Wextra','-Werror','-O1','-fsanitize=address,undefined',
     '-I'+str(root/'tests/host/wardriver_host'),'-I'+str(app),'-I'+str(root/'components/u8g2'),
     str(c),'-o',str(binary)],check=True)
run([str(binary)],check=True)
