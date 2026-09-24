/* Actual app entry, callbacks, keys and renderer; only OS/UART boundaries mocked. */
#include "../../applications_user/bw16_r4tkn/bw16_r4tkn.c"
#include <u8g2_glue.h>
#include <assert.h>

static uint32_t now=1000;
static unsigned sends,closed,allocated,attached,records,alloc_call,fail_alloc,event_pos;
static bool queue_full;
static char command[48];
static InputEvent events[8];
static unsigned event_count;
uint32_t furi_get_tick(void) { return now; }
void furi_delay_ms(uint32_t ms) { (void)ms; }
void furi_delay_us(uint32_t us) { (void)us; }
FuriMutex* furi_mutex_alloc(FuriMutexType t) { (void)t; if(++alloc_call==fail_alloc)return NULL; ++allocated; return (void*)1; }
void furi_mutex_free(FuriMutex* m) { assert(m && allocated && !attached); --allocated; }
FuriStatus furi_mutex_acquire(FuriMutex* m,uint32_t t) { (void)m;(void)t;return FuriStatusOk; }
FuriStatus furi_mutex_release(FuriMutex* m) { (void)m;return FuriStatusOk; }
FuriMessageQueue* furi_message_queue_alloc(uint32_t c,uint32_t s) { (void)c;(void)s;if(++alloc_call==fail_alloc)return NULL;++allocated;return (void*)2; }
void furi_message_queue_free(FuriMessageQueue* q) { assert(q && allocated && !attached);--allocated; }
FuriStatus furi_message_queue_put(FuriMessageQueue* q,const void* v,uint32_t t) { (void)q;(void)v;assert(!t);return queue_full?FuriStatusError:FuriStatusOk; }
FuriStatus furi_message_queue_get(FuriMessageQueue* q,void* v,uint32_t t) {
    assert(q && t==20 && event_pos<event_count);now+=1000; *(InputEvent*)v=events[event_pos++];return FuriStatusOk;
}
ViewPort* view_port_alloc(void) { if(++alloc_call==fail_alloc)return NULL;++allocated;return (void*)3; }
void view_port_free(ViewPort* v) { assert(v && allocated && !attached);--allocated; }
void view_port_draw_callback_set(ViewPort* v,void (*cb)(Canvas*,void*),void* context) { assert(v && cb && context); }
void view_port_input_callback_set(ViewPort* v,void (*cb)(InputEvent*,void*),void* context) { assert(v && cb && context); }
void view_port_update(ViewPort* v) { assert(v); }
void* furi_record_open(const char* n) { assert(!strcmp(n,RECORD_GUI));++records;return (void*)4; }
void furi_record_close(const char* n) { assert(!strcmp(n,RECORD_GUI) && records && !attached);--records; }
void gui_add_view_port(Gui* g,ViewPort* v,GuiLayer l) { (void)l;assert(g && v && !attached);++attached; }
void gui_remove_view_port(Gui* g,ViewPort* v) { assert(g && v && attached);--attached; }
bool bw16_uart_open(Bw16Uart* io) { io->installed=io->leased=true;return true; }
esp_err_t bw16_uart_close(Bw16Uart* io) { assert(io->leased);++closed;io->installed=io->leased=false;return ESP_OK; }
esp_err_t furi_hal_bw16_guard_send(FuriHalBw16Guard* g,const char* text,size_t size) {
    (void)g;assert(size<sizeof(command));memcpy(command,text,size);command[size]=0;++sends;return ESP_OK;
}
esp_err_t uart_get_buffered_data_len(int n,size_t* size) { assert(n==1);*size=0;return ESP_OK; }
int uart_read_bytes(int n,void* data,size_t size,unsigned timeout) { (void)data;(void)size;assert(n==1 && !timeout);return 0; }
unsigned uxQueueMessagesWaiting(QueueHandle_t q) { (void)q;return 0; }
int xQueueReceive(QueueHandle_t q,void* v,unsigned t) { (void)q;(void)v;assert(!t);return 0; }
void canvas_clear(Canvas* c) { u8g2_ClearBuffer(c); }
void canvas_set_font(Canvas* c,Font f) { u8g2_SetFont(c,f==FontPrimary?u8g2_font_helvB08_tr:u8g2_font_haxrcorp4089_tr); }
void canvas_draw_str(Canvas* c,unsigned x,unsigned y,const char* text) {
    assert(y<64 && x+u8g2_GetUTF8Width(c,text)<=128);u8g2_DrawUTF8(c,x,y,text);
}

static void press(App* a,InputKey k,InputType t) { key(a,(InputEvent){.key=k,.type=t}); }
static void setup(App* a) {
    memset(a,0,sizeof(*a));a->io.installed=a->io.leased=true;a->screen=Menu;
    bw16_control_init(&a->control,send_command,a);sends=0;now=1000;
}
static void loaded(App* a) {
    setup(a);a->menu=1;press(a,InputKeyOk,InputTypeShort);
    assert(a->screen==Waiting && sends==1 && !strcmp(command,"DEAUTH_STATION\n"));
    bw16_control_line(&a->control,"LIST_STATION_START",1005);
    bw16_control_line(&a->control,"list-station:Lab,6,001122334455,-40",1010);
    bw16_control_line(&a->control,"LIST_STATION_DONE",1020);sync_state(a);
    assert(a->screen==Results);
}
static void input_tests(void) {
    App a;loaded(&a);
    press(&a,InputKeyOk,InputTypeShort);assert(a.screen==ApActions);
    press(&a,InputKeyDown,InputTypeShort);press(&a,InputKeyOk,InputTypeShort);
    assert(a.screen==Confirm && sends==1);
    for(unsigned i=0;i<10;++i)press(&a,InputKeyOk,InputTypeShort);
    assert(sends==1);press(&a,InputKeyBack,InputTypeShort);assert(a.screen==ApActions);
    press(&a,InputKeyOk,InputTypeShort);press(&a,InputKeyOk,InputTypeLong);
    assert(a.screen==Operation && !strcmp(command,"DEAUTH_STATION 0\n") && sends==2);
    press(&a,InputKeyBack,InputTypeShort);
    assert(a.screen==Operation && a.control.state==Bw16Stopping && !a.exit_requested && sends==3);
    for(unsigned i=0;i<10;++i)press(&a,InputKeyBack,InputTypeShort);
    assert(sends==3 && !a.exit_requested);
    bw16_control_line(&a.control,"TARGETED_DONE",1100);sync_state(&a);
    press(&a,InputKeyBack,InputTypeShort);assert(a.screen==Menu);
    press(&a,InputKeyBack,InputTypeShort);assert(a.screen==Unplug && !a.exit_requested);
    press(&a,InputKeyOk,InputTypeShort);assert(!a.exit_requested);
    press(&a,InputKeyOk,InputTypeLong);assert(a.exit_requested && closed==1);

    loaded(&a);assert(bw16_control_start(&a.control,Bw16TargetStation,0,0,now));sync_state(&a);
    queue_full=true;InputEvent e={.key=InputKeyBack,.type=InputTypeShort};input(&e,&a);
    assert(a.input_overflow && a.back_latched);queue_full=false;
    fail(&a,"Input queue full");back(&a);assert(a.screen==Operation && a.control.state==Bw16Stopping);
    a.dirty=false;bw16_control_tick(&a.control,now+BW16_STOP_TIMEOUT_MS);sync_state(&a);
    assert(a.screen==Error && a.dirty && a.control.remote_unknown);
    press(&a,InputKeyBack,InputTypeShort);assert(a.screen==Unplug && a.control.remote_unknown);
    puts("UI: hold confirmation, repeated keys, Back waits for STOP, unknown-stop disconnect and queue overflow PASS");
}
static void entry_tests(void) {
    closed=0;
    for(fail_alloc=1;fail_alloc<=3;++fail_alloc) {
        alloc_call=0;assert(bw16_r4tkn_app(NULL)==-1);assert(!allocated && !attached && !records && !closed);
    }
    fail_alloc=0;alloc_call=0;event_pos=0;
    events[0]=(InputEvent){.key=InputKeyBack,.type=InputTypeShort};event_count=1;
    assert(bw16_r4tkn_app(NULL)==0 && !allocated && !attached && !records && !closed);
    alloc_call=0;event_pos=0;
    events[0]=(InputEvent){.key=InputKeyOk,.type=InputTypeLong};
    events[1]=(InputEvent){.key=InputKeyBack,.type=InputTypeShort};
    events[2]=(InputEvent){.key=InputKeyOk,.type=InputTypeLong};event_count=3;
    assert(bw16_r4tkn_app(NULL)==0 && !allocated && !attached && !records && closed==1);
    puts("UI: actual entry setup, unplug-confirmed cleanup and allocation failures PASS");
}

#ifdef _WIN32
uint8_t u8x8_capture_get_pixel_1(uint16_t x,uint16_t y,uint8_t* p,uint8_t w) { (void)x;(void)y;(void)p;(void)w;abort(); }
uint8_t u8x8_capture_get_pixel_2(uint16_t x,uint16_t y,uint8_t* p,uint8_t w) { (void)x;(void)y;(void)p;(void)w;abort(); }
void u8x8_capture_write_pbm_pre(uint8_t w,uint8_t h,void (*o)(const char*)) { (void)w;(void)h;(void)o;abort(); }
void u8x8_capture_write_xbm_pre(uint8_t w,uint8_t h,void (*o)(const char*)) { (void)w;(void)h;(void)o;abort(); }
void u8x8_capture_write_pbm_buffer(uint8_t* p,uint8_t w,uint8_t h,uint8_t (*g)(uint16_t,uint16_t,uint8_t*,uint8_t),void (*o)(const char*)) { (void)p;(void)w;(void)h;(void)g;(void)o;abort(); }
void u8x8_capture_write_xbm_buffer(uint8_t* p,uint8_t w,uint8_t h,uint8_t (*g)(uint16_t,uint16_t,uint8_t*,uint8_t),void (*o)(const char*)) { (void)p;(void)w;(void)h;(void)g;(void)o;abort(); }
uint8_t u8g2_GetKerning(u8g2_t* u,u8g2_kerning_t* k,uint16_t a,uint16_t b) { (void)u;(void)k;(void)a;(void)b;abort(); }
uint8_t u8g2_GetKerningByTable(u8g2_t* u,const uint16_t* k,uint16_t a,uint16_t b) { (void)u;(void)k;(void)a;(void)b;abort(); }
#endif

int main(int argc,char** argv) {
    assert(argc==2);input_tests();entry_tests();
    Canvas canvas;u8g2_Setup_st756x_flipper(&canvas,U8G2_R0,u8x8_hw_spi_esp32,u8g2_gpio_and_delay_esp32);
    u8g2_SetFontMode(&canvas,1);u8g2_SetFontPosBaseline(&canvas);
    unsigned char pages[12][1024];App a;loaded(&a);
    a.control.client_count=1;strcpy(a.control.clients[0],"112233445566");
    a.pending=Bw16TargetClient;
    strcpy(a.control.error,"STOP unconfirmed; power off BW16");
    for(unsigned i=0;i<12;++i) {
        a.screen=i<11?(Screen)i:Operation;
        a.control.state=i==11?Bw16Stopped:Bw16Stopping;
        a.control.remote_unknown=true;a.control.counter=UINT32_MAX;
        draw(&canvas,&a);memcpy(pages[i],u8g2_GetBufferPtr(&canvas),1024);
    }
    FILE* file=fopen(argv[1],"wb");assert(file);fprintf(file,"P5\n804 1380\n255\n");
    for(unsigned y=0;y<1380;++y)for(unsigned x=0;x<804;++x) {
        unsigned gx=x/396,gy=y/228,lx=x%396,ly=y%228;unsigned char pixel=205;
        if(gx<2 && gy<6 && lx>=12 && ly>=18 && lx<396 && ly<210) {
            unsigned px=(lx-12)/3,py=(ly-18)/3;pixel=(pages[gy*2+gx][px+128*(py/8)]&(1U<<(py%8)))?16:248;
        }
        fputc(pixel,file);
    }
    fclose(file);puts("UI: all screens rendered with native fonts and checked for horizontal clipping PASS");
}
