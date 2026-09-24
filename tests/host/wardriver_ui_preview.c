/* Draw production UI code with Sor3nt's actual u8g2 renderer and fonts.
 * Only OS/Canvas boundary wrappers below are host-specific. */
#include "wardriver.h"
#include <u8g2_glue.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
FuriStatus furi_mutex_acquire(FuriMutex* m,uint32_t t) { (void)m; (void)t; return FuriStatusOk; }
FuriStatus furi_mutex_release(FuriMutex* m) { (void)m; return FuriStatusOk; }
void furi_delay_ms(uint32_t t) { (void)t; }
void furi_delay_us(uint32_t t) { (void)t; }
const char* wlan_hal_survey_status(void) { return "RELEASING BLUETOOTH"; }
void view_port_update(ViewPort* view) { (void)view; }
FuriStatus furi_message_queue_put(FuriMessageQueue* q,const void* v,uint32_t t) {
    (void)q; (void)v; (void)t; abort(); /* This harness calls the consumer directly. */
}
/* These optional u8g2 entry points are absent from this trimmed firmware.
 * ELF gc-sections discards them; COFF needs definitions. Never mask a use. */
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
/* Exercise the production input handler, including repeated Start presses. */
static void input_tests(void) {
    WardriverApp app={0}; InputEvent e={.key=InputKeyOk,.type=InputTypeShort};
    wardriver_ui_handle(&app,&e);
    if(app.want_running) abort(); /* Not ready: don't queue a hidden Start. */
    app.model.ready=true;
    wardriver_ui_handle(&app,&e);
    if(!app.want_running || !app.model.transitioning) abort();
    for(unsigned i=0;i<20;++i) wardriver_ui_handle(&app,&e);
    if(!app.want_running) abort(); /* Repeated presses cannot cancel/restart. */
    app.model.active=true; app.model.transitioning=false;
    e.type=InputTypeRepeat; wardriver_ui_handle(&app,&e);
    if(!app.want_running) abort(); /* A held knob is not repeated Start/Stop. */
    e.type=InputTypeShort; wardriver_ui_handle(&app,&e);
    if(app.want_running || !app.model.transitioning) abort();
    wardriver_ui_handle(&app,&e);
    if(app.want_running) abort();
    e.key=InputKeyBack; wardriver_ui_handle(&app,&e);
    if(!app.quit) abort(); /* Exit request remains available during transition. */
    app=(WardriverApp){0}; app.model.ready=true; app.model.active=true;
    app.model.page=WardPageMenu; app.model.menu=1; app.model.notable_count=3;
    e.key=InputKeyOk; wardriver_ui_handle(&app,&e);
    if(app.model.page!=WardPageList || !app.model.notable_only || app.model.list_count!=3) abort();
    app.model.page=WardPageMenu; app.model.menu=2; wardriver_ui_handle(&app,&e);
    if(app.model.page!=WardPageStats) abort();
    e.key=InputKeyBack; wardriver_ui_handle(&app,&e);
    if(app.model.page!=WardPageMenu) abort();
    app=(WardriverApp){0}; app.model.ready=true; app.model.active=true;
    app.model.page=WardPageMenu; app.model.menu=4; e.key=InputKeyOk;
    wardriver_ui_handle(&app,&e);
    if(app.model.page!=WardPageHome || app.wigle_command) abort(); /* Never upload while surveying. */
    app.model.active=false; app.model.page=WardPageMenu; wardriver_ui_handle(&app,&e);
    if(app.model.page!=WardPageWigle) abort();
    app.model.wigle_menu=3; app.model.wigle_name_set=app.model.wigle_token_set=true;
    strcpy(app.model.wigle_path,WARD_WIGLE_DIR "wardrive_1790164800_abcd1234_wigle.csv");
    wardriver_ui_handle(&app,&e);
    if(app.model.page!=WardPageUploadConfirm || app.wigle_command) abort();
    wardriver_ui_handle(&app,&e);
    if(app.wigle_command!=WardWigleSend || !app.model.wigle_busy || !app.model.wigle_uploading) abort();
    for(unsigned i=0;i<10;++i) wardriver_ui_handle(&app,&e);
    e.key=InputKeyBack; wardriver_ui_handle(&app,&e);
    if(!app.cancel_upload || app.quit || app.model.page!=WardPageUploadStatus) abort();
    puts("PASS: upload requires idle radio and explicit confirmation; repeated OK gated; BACK cancels without unloading");
    puts("PASS: production UI ignores repeated Start/Stop during transitions; BACK remains available");
}
void canvas_clear(Canvas* c) { u8g2_ClearBuffer(c); }
void canvas_set_font(Canvas* c,Font f) { u8g2_SetFont(c,f==FontPrimary?u8g2_font_helvB08_tr:u8g2_font_haxrcorp4089_tr); }
void canvas_set_color(Canvas* c,Color color) { u8g2_SetDrawColor(c,(uint8_t)color); }
int64_t esp_timer_get_time(void) { return 5000000; }
void canvas_draw_rframe(Canvas* c,unsigned x,unsigned y,unsigned w,unsigned h,unsigned r) { u8g2_DrawRFrame(c,x,y,w,h,r); }
void canvas_draw_rbox(Canvas* c,unsigned x,unsigned y,unsigned w,unsigned h,unsigned r) { u8g2_DrawRBox(c,x,y,w,h,r); }
void canvas_draw_line(Canvas* c,unsigned x,unsigned y,unsigned x2,unsigned y2) { u8g2_DrawLine(c,x,y,x2,y2); }
void canvas_draw_str(Canvas* c,unsigned x,unsigned y,const char* text) { u8g2_DrawUTF8(c,x,y,text); }
size_t canvas_string_width(Canvas* c,const char* text) { return u8g2_GetUTF8Width(c,text); }
void canvas_draw_str_aligned(Canvas* c,unsigned x,unsigned y,Align h,Align v,const char* text) {
    (void)v; if(h==AlignRight) x-=canvas_string_width(c,text); canvas_draw_str(c,x,y,text);
}
int main(int argc,char** argv) {
    if(argc!=2) return 1;
    input_tests();
    Canvas c; u8g2_Setup_st756x_flipper(&c,U8G2_R0,u8x8_hw_spi_esp32,u8g2_gpio_and_delay_esp32);
    u8g2_SetFontMode(&c,1); u8g2_SetFontPosBaseline(&c);
    WardriverApp a={0}; WardriverModel* m=&a.model;
    m->ready=true; m->active=true; m->count=164; m->list_count=164; m->wifi_count=143; m->ble_count=21; m->notable_count=7; m->capacity=4096; m->detections=387; m->log_ok=true;
    m->gps=(WardriverGpsData){.has_fix=true,.satellites_valid=true,.satellites=9};
    strcpy(m->gps_status,"FIX"); strcpy(m->status,"LISTENING");
    m->last.used=true; strcpy(m->last.observation.ssid,"Cafe Atlas");
    memcpy(m->last.observation.mac,"\xAA\xBB\xCC\xDD\xEE\xFF",6);
    m->last.observation.rssi=-67; m->last.observation.channel=6; m->last.observation.auth=3;
    m->last.detections=12; m->last.last_fix=(WardriverGpsData){.has_fix=true,.latitude=48.1173,
        .longitude=11.5167,.altitude_valid=true,.altitude=545.4,.satellites=9,
        .timestamp_valid=true,.timestamp=1790118000};
    m->rows[0]=m->last; m->rows[1]=m->last; m->rows[2]=m->last; m->row_count=3;
    strcpy(m->rows[1].observation.ssid,"Possible camera"); m->rows[1].observation.hints=WardriverHintFlock; m->rows[1].observation.mode=WardriverBle;
    strcpy(m->rows[2].observation.ssid,"Remote ID"); m->rows[2].observation.hints=WardriverHintRemoteId; m->rows[2].observation.mode=WardriverBle;
    m->config=(WardriverConfig){.gps_mode=0,.gps_uart=2,.gps_rx=-1,.gps_tx=-1,.gps_baud=9600};
    unsigned char pages[16][1024];
    for(unsigned i=0;i<16;++i) {
        m->page=i==0||i==1?WardPageHome:i==2?WardPageMenu:i==3?WardPageList:
            i==4?WardPageSettings:i==7?WardPageAbout:WardPageDetail;
        if(i==1) { strcpy(m->gps_status,"OFF"); m->gps.has_fix=false; m->gps.satellites_valid=false; }
        m->detail=i==6?3:0;
        if(i==8) {
            m->page=WardPageList; m->notable_only=true; m->list_count=7;
            m->rows[0]=m->rows[1]; m->rows[1]=m->rows[2]; m->row_count=2;
        }
        if(i==9) { m->page=WardPageStats; strcpy(m->wifi_status,"SCAN"); strcpy(m->ble_status,"SCAN"); }
        if(i==10) { m->page=WardPageHome; m->wifi_count=4096; m->ble_count=0; m->notable_count=0; strcpy(m->status,"BLE FAILED - WIFI RUNNING"); }
        if(i==11) { a.quit=1; strcpy(m->status,"SYNCING LOCAL CSV"); }
        if(i>=12) { a.quit=0; m->active=false; m->wifi_online=true; }
        if(i==12) { m->page=WardPageWigle; m->wigle_name_set=m->wigle_token_set=true; }
        if(i==13) { m->page=WardPageUploadConfirm; strcpy(m->wigle_path,WARD_WIGLE_DIR "wardrive_1790164800_abcd1234_wigle.csv"); }
        if(i==14) { m->page=WardPageUploadStatus; m->wigle_busy=m->wigle_uploading=true; m->upload_percent=68; strcpy(m->wigle_status,"UPLOADING CSV"); }
        if(i==15) { m->page=WardPageUploadStatus; m->wigle_busy=m->wigle_uploading=false; strcpy(m->wigle_status,"TLS/network error; check Wi-Fi/time"); }
        wardriver_ui_draw(&c,&a); memcpy(pages[i],u8g2_GetBufferPtr(&c),1024);
    }
    FILE* f=fopen(argv[1],"wb"); if(!f) return 2;
    /* Two columns, eight rows, 3x nearest-neighbour pixels, 12-pixel gutter. */
    fprintf(f,"P5\n804 1836\n255\n");
    for(unsigned y=0;y<1836;++y) for(unsigned x=0;x<804;++x) {
        unsigned gx=x/396,gy=y/228,lx=x%396,ly=y%228; unsigned char pixel=205;
        if(gx<2 && gy<8 && lx>=12 && ly>=18 && lx<396 && ly<210) {
            unsigned px=(lx-12)/3,py=(ly-18)/3;
            pixel=(pages[gy*2+gx][px+128*(py/8)]&(1U<<(py%8)))?16:248;
        }
        fputc(pixel,f);
    }
    fclose(f); printf("PASS: native UI preview; model snapshot %zu bytes\n",sizeof(*m)); return 0;
}
