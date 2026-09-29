/* Actual app entry, callbacks, keys and renderer; only OS/UART boundaries mocked. */
#include "../../applications_user/bw16_r4tkn/bw16_r4tkn.c"
#include <u8g2_glue.h>
#include <assert.h>

static uint32_t now=1000;
static unsigned sends,closed,allocated,attached,records,alloc_call,fail_alloc,event_pos;
static unsigned passive_starts, assisted_starts, deauth_starts;
static bool queue_full;
static unsigned viewport_updates, waiting_updates, dots_drawn;
static size_t buffered_bytes;
static App* viewport_context;
static esp_err_t open_error;
static unsigned sd_records, sd_allocated, sd_opens, sd_closes, sd_writes, sd_syncs;
static unsigned sd_existing, sd_fail_write, sd_fail_sync;
static bool sd_missing, sd_fail_open, sd_fail_close, sd_opened;
static FS_Error sd_error;
static char sd_path[80];
static uint8_t sd_bytes[500000];
static size_t sd_size;
uint32_t furi_hal_rtc_get_timestamp(void) { return 1700000000U; }
uint32_t furi_hal_random_get(void) { return 0x1234abcdU; }
File* storage_file_alloc(Storage* storage) { assert(storage);++sd_allocated;return (File*)9; }
void storage_file_free(File* file) { assert(file && sd_allocated);sd_opened=false;--sd_allocated; }
bool storage_simply_mkdir(Storage* storage,const char* path) { assert(storage && path);return !sd_missing; }
bool storage_file_open(File* file,const char* path,FS_AccessMode access,FS_OpenMode mode) {
    assert(file && !sd_opened && access==FSAM_WRITE && mode==FSOM_CREATE_NEW);++sd_opens;
    if(sd_fail_open) { sd_error=FSE_NOT_READY;return false; }
    if(sd_existing) { --sd_existing;sd_error=FSE_EXIST;return false; }
    strcpy(sd_path,path);sd_size=0;sd_opened=true;sd_error=FSE_OK;return true;
}
bool storage_file_close(File* file) {
    assert(file);++sd_closes;if(!sd_opened)return false;sd_opened=false;return !sd_fail_close;
}
bool storage_file_sync(File* file) { assert(file && sd_opened);return ++sd_syncs!=sd_fail_sync; }
FS_Error storage_file_get_error(File* file) { assert(file);return sd_error; }
size_t storage_file_write(File* file,const void* bytes,size_t length) {
    assert(file && sd_opened && sd_size+length<=sizeof(sd_bytes));
    if(++sd_writes==sd_fail_write) { if(length)--length; }
    memcpy(sd_bytes+sd_size,bytes,length);sd_size+=length;return length;
}
static void sd_reset(void) {
    assert(!sd_opened && !sd_allocated && !sd_records);
    sd_opens=sd_closes=sd_writes=sd_syncs=sd_existing=sd_fail_write=sd_fail_sync=0;
    sd_missing=sd_fail_open=sd_fail_close=false;sd_path[0]=0;sd_size=0;
}


static char rendered[512];
static char command[48];
static const uint8_t* rx_input;
static size_t rx_left;
static InputEvent events[16];
static unsigned event_count;
uint32_t furi_get_tick(void) { return now / 10; } /* Deliberately 100 Hz. */
int64_t esp_timer_get_time(void) { return (int64_t)now * 1000; }
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
    assert(q && t==20 && event_pos<event_count);now+=1000;
    InputEvent event=events[event_pos++];
    if(event.type==InputTypePress)return FuriStatusError; /* Scripted no-input timeout. */
    *(InputEvent*)v=event;return FuriStatusOk;
}
ViewPort* view_port_alloc(void) { if(++alloc_call==fail_alloc)return NULL;++allocated;return (void*)3; }
void view_port_free(ViewPort* v) { assert(v && allocated && !attached);--allocated; }
void view_port_draw_callback_set(ViewPort* v,void (*cb)(Canvas*,void*),void* context) { assert(v && cb && context);viewport_context=context; }
void view_port_input_callback_set(ViewPort* v,void (*cb)(InputEvent*,void*),void* context) { assert(v && cb && context); }
void view_port_update(ViewPort* v) { assert(v);++viewport_updates;if(viewport_context && viewport_context->screen==Waiting)++waiting_updates; }
void* furi_record_open(const char* n) { if(!strcmp(n,RECORD_STORAGE)){++sd_records;return (void*)8;} assert(!strcmp(n,RECORD_GUI));++records;return (void*)4; }
void furi_record_close(const char* n) { if(!strcmp(n,RECORD_STORAGE)){assert(sd_records);--sd_records;return;} assert(!strcmp(n,RECORD_GUI) && records && !attached);--records; }
void gui_add_view_port(Gui* g,ViewPort* v,GuiLayer l) { (void)l;assert(g && v && !attached);++attached; }
void gui_remove_view_port(Gui* g,ViewPort* v) { assert(g && v && attached);--attached; }
bool bw16_uart_open(Bw16Uart* io) {
    if(open_error) { io->stage="UART install failed";io->error=open_error;return false; }
    io->installed=io->awake=true;return true;
}
esp_err_t bw16_uart_close(Bw16Uart* io) {
    if(io->awake)++closed;io->installed=io->awake=false;return ESP_OK;
}
esp_err_t bw16_uart_send(Bw16Uart* io,const char* text,size_t size) {
    assert(io->installed && size<sizeof(command));memcpy(command,text,size);command[size]=0;++sends;
    if(!strncmp(command,"CAPTURE ",8)) { assert(sd_opened && sd_size==24 && sd_syncs);++passive_starts; }
    if(!strncmp(command,"CAPTURE_DEAUTH ",15)) { assert(sd_opened && sd_size==24 && sd_syncs);++assisted_starts; }
    if(!strncmp(command,"DEAUTH_STATION ",15) || !strncmp(command,"DEAUTH_CLIENT ",14) ||
       !strncmp(command,"DEAUTH_SELECTED ",16) || !strcmp(command,"DEAUTH_ALL\n")) ++deauth_starts;
    return ESP_OK;
}
esp_err_t uart_get_buffered_data_len(int n,size_t* size) { assert(n==2);*size=buffered_bytes;return ESP_OK; }
int uart_read_bytes(int n,void* data,size_t size,unsigned timeout) {
    assert(n==2 && !timeout);
    if(size>rx_left)size=rx_left;
    if(size)memcpy(data,rx_input,size);
    if(size)rx_input+=size;
    rx_left-=size;return (int)size;
}
unsigned uxQueueMessagesWaiting(QueueHandle_t q) { (void)q;return 0; }
int xQueueReceive(QueueHandle_t q,void* v,unsigned t) { (void)q;(void)v;assert(!t);return 0; }
void canvas_clear(Canvas* c) { rendered[0]=0;u8g2_ClearBuffer(c); }
void canvas_set_font(Canvas* c,Font f) { u8g2_SetFont(c,f==FontPrimary?u8g2_font_helvB08_tr:u8g2_font_haxrcorp4089_tr); }
uint16_t canvas_string_width(Canvas* c,const char* text) { return u8g2_GetUTF8Width(c,text); }
void canvas_draw_str(Canvas* c,unsigned x,unsigned y,const char* text) {
    if(y>=64 || x+u8g2_GetUTF8Width(c,text)>128)fprintf(stderr,"Clipped UI string at (%u,%u), width %u: %s\n",x,y,(unsigned)u8g2_GetUTF8Width(c,text),text);
    assert(y<64 && x+u8g2_GetUTF8Width(c,text)<=128);u8g2_DrawUTF8(c,x,y,text);
    assert(strlen(rendered)+strlen(text)+2<sizeof(rendered));strcat(rendered,text);strcat(rendered,"\n");
}

void canvas_draw_dot(Canvas* c,unsigned x,unsigned y) {
    assert(x<128 && y<64);++dots_drawn;u8g2_DrawPixel(c,x,y);
}
void canvas_draw_line(Canvas* c,unsigned x0,unsigned y0,unsigned x1,unsigned y1) {
    assert(x0<128 && x1<128 && y0<64 && y1<64);u8g2_DrawLine(c,x0,y0,x1,y1);
}

static void press(App* a,InputKey k,InputType t) { key(a,(InputEvent){.key=k,.type=t}); }
static void setup(App* a) {
    memset(a,0,sizeof(*a));a->io.installed=a->io.awake=true;a->screen=Menu;
    bw16_control_init(&a->control,send_command,a);a->control.native_selected=true;a->control.capture_supported=true;a->control.capture_deauth_supported=true;
    passive_starts=assisted_starts=deauth_starts=0;sends=0;now=1000;rx_input=NULL;rx_left=0;buffered_bytes=0;
}
static void loaded(App* a) {
    setup(a);a->menu=MenuStations;press(a,InputKeyOk,InputTypeShort);
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
    press(&a,InputKeyBack,InputTypeLong);
    assert(a.screen==Operation && a.control.state==Bw16Stopping && !a.exit_requested && sends==3);
    for(unsigned i=0;i<10;++i)press(&a,InputKeyBack,InputTypeShort);
    assert(sends==3 && !a.exit_requested);
    bw16_control_line(&a.control,"TARGETED_DONE",1100);sync_state(&a);
    press(&a,InputKeyBack,InputTypeShort);assert(a.screen==Menu);
    press(&a,InputKeyBack,InputTypeShort);assert(a.exit_requested && closed==1);

    loaded(&a);assert(bw16_control_start(&a.control,Bw16TargetStation,0,0,now));sync_state(&a);
    queue_full=true;InputEvent e={.key=InputKeyBack,.type=InputTypeShort};input(&e,&a);
    assert(a.input_overflow && a.back_latched);queue_full=false;
    fail(&a,"Input queue full");back(&a);assert(a.screen==Operation && a.control.state==Bw16Stopping);
    a.dirty=false;bw16_control_tick(&a.control,now+BW16_STOP_TIMEOUT_MS);sync_state(&a);
    assert(a.screen==Error && a.dirty && a.control.remote_unknown);
    press(&a,InputKeyBack,InputTypeShort);assert(a.screen==Unplug && a.control.remote_unknown);
    press(&a,InputKeyOk,InputTypeShort);assert(!a.exit_requested);
    press(&a,InputKeyOk,InputTypeLong);assert(a.exit_requested);
    setup(&a);a.screen=Connect;a.io.installed=a.io.awake=false;
    open_error=0x108;press(&a,InputKeyOk,InputTypeShort);
    assert(a.screen==Error && a.setup_failed && !a.io.installed);open_error=0;
    puts("UI: confirmation, repeated keys, Back waits for STOP, uncertain-stop exit and queue overflow PASS");
}
static void entry_tests(void) {
    closed=0;
    for(fail_alloc=1;fail_alloc<=3;++fail_alloc) {
        alloc_call=0;assert(bw16_r4tkn_app(NULL)==-1);assert(!allocated && !attached && !records && !closed);
    }
    fail_alloc=0;alloc_call=0;event_pos=0;
    events[0]=(InputEvent){.key=InputKeyBack,.type=InputTypeShort};event_count=1;
    assert(bw16_r4tkn_app(NULL)==0 && !allocated && !attached && !records && !closed);
    for(unsigned i=0;i<20;++i) {
        alloc_call=0;event_pos=0;
        events[0]=(InputEvent){.key=InputKeyOk,.type=InputTypeShort};
        events[1]=(InputEvent){.key=InputKeyBack,.type=InputTypeShort};event_count=2;
        assert(bw16_r4tkn_app(NULL)==0 && !allocated && !attached && !records && closed==i+1);
    }
    open_error=0x108;alloc_call=0;event_pos=0;event_count=2;
    assert(bw16_r4tkn_app(NULL)==0 && !allocated && !attached && !records && closed==20);
    open_error=0;
    puts("UI: actual entry, 20 repeated connect/exit cycles and allocation/setup failure cleanup PASS");
}

static void inject(App* a,const void* data,size_t size) {
    rx_input=data;rx_left=size;while(rx_left)receive(a);poll_control(a);
}
static void capture_begin_mode(App* a,bool deauth) {
    sd_reset();loaded(a);a->intent=deauth?MenuCaptureDeauth:MenuCapture;
    press(a,InputKeyOk,InputTypeShort);
    assert(a->screen==Confirm && a->pending==Bw16TargetCapture);
    press(a,InputKeyOk,InputTypeShort);assert(sends==1 && !sd_opens);
    press(a,InputKeyOk,InputTypeLong);
    assert(a->screen==Operation && a->control.state==Bw16Starting);
    assert(!strcmp(command,deauth?"CAPTURE_DEAUTH 0 1234abcd\n":"CAPTURE 0 1234abcd\n") && sd_size==24 && sd_opened);
    assert(a->control.capture_with_deauth==deauth && a->capture_deauth_pending==deauth);
    assert(passive_starts==!deauth && assisted_starts==deauth && !deauth_starts);
    assert(!memcmp(sd_bytes,"\xd4\xc3\xb2\xa1",4));
}
static void capture_begin(App* a) { capture_begin_mode(a,false); }
static void capture_wire(App* a,uint32_t sequence,uint32_t usec,bool eapol,uint32_t session) {
    BwcPacket p={.session=session,.sequence=sequence,.usec=usec,.length=36,.kind=eapol?BwcEapol:BwcBeacon};
    const uint8_t bssid[]={0,0x11,0x22,0x33,0x44,0x55};
    if(eapol) {
        p.data[0]=8;p.data[1]=1;memcpy(p.data+4,bssid,6);
        const uint8_t snap[]={0xaa,0xaa,3,0,0,0,0x88,0x8e,2,1,0,0};memcpy(p.data+24,snap,sizeof(snap));
    } else {p.data[0]=0x80;memcpy(p.data+16,bssid,6);}
    char line[BWC_LINE_SIZE];size_t length=bwc_encode(line,sizeof(line),&p);assert(length);
    line[length++]='\n';inject(a,line,length);
}
static void capture_reply(App* a,const char* reply) { inject(a,reply,strlen(reply)); }
static void capture_tests(Canvas* canvas) {
    App a;capture_begin(&a);
    capture_wire(&a,0,0,true,0x1234abcd);assert(!a.capture.packets && a.capture.dropped==1);
    capture_reply(&a,"CAPTURE_START:1234ABCD\n");assert(a.control.state==Bw16Active);
    capture_wire(&a,0,1500000,false,0x1234abcd);
    assert(a.capture.packets==1 && !a.capture.eapol && sd_size==76);
    uint32_t seconds=0;for(unsigned i=0;i<4;++i)seconds|=(uint32_t)sd_bytes[24+i]<<(8*i);
    assert(seconds==1700000001U && sd_bytes[32]==36 && sd_bytes[36]==36);
    capture_wire(&a,1,1600000,true,0x1234abce);assert(a.capture.packets==1);
    capture_wire(&a,2,1700000,true,0x1234abcd);assert(a.capture.eapol==1 && a.capture.gaps==1);
    capture_wire(&a,2,1800000,true,0x1234abcd);assert(a.capture.packets==2);
    capture_wire(&a,3,120000001,true,0x1234abcd);assert(a.capture.packets==2);
    capture_reply(&a,"PCAP1:bad\n");assert(a.control.state==Bw16Active);
    char oversized[BWC_LINE_SIZE+20];memset(oversized,'P',sizeof(oversized));oversized[sizeof(oversized)-1]='\n';
    inject(&a,oversized,sizeof(oversized));assert(a.control.state==Bw16Active && a.capture.dropped==6);
    draw(canvas,&a);assert(strstr(rendered,"EAPOL: 1") && strstr(rendered,"Reconnect own client"));
    press(&a,InputKeyRight,InputTypeShort);draw(canvas,&a);assert(strstr(rendered,"capture_0001.pcap"));
    press(&a,InputKeyRight,InputTypeShort);
    press(&a,InputKeyBack,InputTypeShort);
    assert(sd_opened && a.control.state==Bw16Stopping && !strcmp(command,"STOP\n"));
    capture_wire(&a,3,1800000,true,0x1234abcd);assert(a.capture.eapol==2 && sd_opened);
    capture_reply(&a,"CAPTURE_STATS:1234abcd:4:7\nCAPTURE_DONE:1234abcd\n");
    assert(a.control.state==Bw16Stopped && !sd_opened && a.capture.closed && !a.capture.failed && !sd_records);
    draw(canvas,&a);assert(strstr(rendered,"Saved PCAP") && strstr(rendered,"capture_0001.pcap"));
    press(&a,InputKeyBack,InputTypeShort);assert(a.screen==Menu);
    sd_reset();sd_existing=1;
    assert(bw16_capture_prepare(&a.capture,"00:11:22:33:44:55",3,0));
    assert(sd_opens==2 && strstr(a.capture.path,"capture_0002.pcap"));assert(bw16_capture_close(&a.capture));

    capture_begin(&a);capture_reply(&a,"CAPTURE_START:1234abcd\n");
    BwcPacket large={.session=0x1234abcd,.sequence=0,.usec=5,.length=BWC_MAX_FRAME,.kind=BwcEapol};
    large.data[0]=8;large.data[1]=1;memcpy(large.data+4,a.capture.bssid,6);
    const uint8_t snap[]={0xaa,0xaa,3,0,0,0,0x88,0x8e,2,1,0,0};memcpy(large.data+24,snap,sizeof(snap));
    char wire[BWC_LINE_SIZE];size_t count=bwc_encode(wire,sizeof(wire),&large);assert(count>BW16_LINE_SIZE);
    wire[count++]='\n';wire[100]=wire[100]=='0'?'1':'0';
    inject(&a,wire,count);assert(!a.capture.packets && a.capture.dropped==1);
    ++large.data[4];count=bwc_encode(wire,sizeof(wire),&large);wire[count++]='\n';
    inject(&a,wire,count);assert(!a.capture.packets && a.capture.dropped==2);
    --large.data[4];large.kind=BwcBeacon;count=bwc_encode(wire,sizeof(wire),&large);wire[count++]='\n';
    inject(&a,wire,count);assert(!a.capture.packets && a.capture.dropped==3);
    large.kind=BwcEapol;count=bwc_encode(wire,sizeof(wire),&large);wire[count++]='\n';
    inject(&a,wire,count);assert(a.capture.eapol==1 && sd_size==24+16+BWC_MAX_FRAME);
    press(&a,InputKeyBack,InputTypeShort);capture_reply(&a,"CAPTURE_DONE:1234abcd\n");

    capture_begin(&a);press(&a,InputKeyBack,InputTypeShort);
    capture_reply(&a,"CAPTURE_START:1234abcd\n");
    assert(a.control.state==Bw16Stopping && a.control.start_confirmed);
    capture_wire(&a,0,100,true,0x1234abcd);assert(a.capture.eapol==1);
    capture_reply(&a,"CAPTURE_DONE:1234abcd\n");assert(a.control.state==Bw16Stopped);
    capture_begin(&a);capture_reply(&a,"CAPTURE_DONE:1234abcd\n");
    assert(a.control.state==Bw16Fault && a.capture.failed && !sd_opened);
    draw(canvas,&a);assert(!strstr(rendered,"Saved"));

    for(unsigned fault=0;fault<3;++fault) {
        sd_reset();loaded(&a);a.intent=MenuCapture;press(&a,InputKeyOk,InputTypeShort);
        if(fault==0)sd_missing=true;else if(fault==1)sd_fail_open=true;else sd_fail_write=1;
        press(&a,InputKeyOk,InputTypeLong);
        assert(a.screen==CaptureReport && sends==1 && !sd_opened && a.capture.failed && !sd_records && !sd_allocated);
        assert(a.capture.created==(fault==2));draw(canvas,&a);assert(!strstr(rendered,"Saved"));
    }
    for(unsigned fault=0;fault<3;++fault) {
        capture_begin(&a);capture_reply(&a,"CAPTURE_START:1234abcd\n");
        if(fault==0)sd_fail_write=sd_writes+2;
        else if(fault==1)sd_fail_sync=sd_syncs+1;
        else sd_fail_close=true;
        capture_wire(&a,0,10,true,0x1234abcd);
        if(fault<2)assert(a.control.state==Bw16Stopping && sd_opened && !a.capture.eapol);
        else press(&a,InputKeyBack,InputTypeShort);
        capture_reply(&a,"CAPTURE_DONE:1234abcd\n");
        assert(a.control.state==Bw16Fault && a.capture.failed && !sd_opened && !sd_records && !sd_allocated);
        assert(strstr(a.capture.path,"capture_0001.pcap"));draw(canvas,&a);assert(!strstr(rendered,"Saved"));
    }
    capture_begin(&a);capture_reply(&a,"CAPTURE_START:1234abcd\n");
    for(unsigned i=0;i<BW16_CAPTURE_PACKETS;++i)capture_wire(&a,i,i*100,true,0x1234abcd);
    assert(a.capture.eapol==BW16_CAPTURE_PACKETS && a.control.state==Bw16Stopping && sd_opened);
    size_t limit_size=sd_size;
    capture_wire(&a,BW16_CAPTURE_PACKETS,30000,true,0x1234abcd);assert(sd_size==limit_size);
    capture_reply(&a,"CAPTURE_DONE:1234abcd\n");assert(a.control.state==Bw16Stopped);

    capture_begin(&a);capture_reply(&a,"CAPTURE_START:1234abcd\n");
    now=121000;poll_control(&a);assert(a.control.state==Bw16Stopping && sd_opened);
    now=126000;poll_control(&a);assert(a.control.state==Bw16Fault && !sd_opened && a.capture.failed);
    sd_reset();setup(&a);a.control.capture_supported=false;a.menu=MenuCapture;
    press(&a,InputKeyOk,InputTypeShort);assert(a.screen==Menu && !sends && !sd_opens);
    puts("Capture UI: passive review/start, PCAP bytes/RTC, framing, session/order/length rejection, STOP drain, 256 limit, unique paths and SD create/write/sync/close failure PASS");
}

static void capture_deauth_tests(Canvas* canvas) {
    App a;sd_reset();loaded(&a);a.intent=MenuCaptureDeauth;
    press(&a,InputKeyOk,InputTypeShort);assert(a.screen==Confirm && a.capture_deauth_pending);
    draw(canvas,&a);assert(strstr(rendered,"Capture + deauth") && strstr(rendered,"One burst (3 frames)"));
    press(&a,InputKeyOk,InputTypeShort);assert(!assisted_starts && !sd_opens);
    press(&a,InputKeyBack,InputTypeShort);assert(a.screen==Results && !assisted_starts);
    press(&a,InputKeyBack,InputTypeShort);a.menu=MenuCapture;press(&a,InputKeyOk,InputTypeShort);
    assert(!a.capture_deauth_pending && a.screen==Waiting);

    capture_begin_mode(&a,true);draw(canvas,&a);assert(strstr(rendered,"TX attempts: pending"));
    capture_reply(&a,"CAPTURE_DEAUTH:1234abcd:3\n");assert(!a.control.capture_deauth_reported);
    capture_reply(&a,"CAPTURE_START:1234abcd\nCAPTURE_DEAUTH:1234abce:3\n");
    assert(a.control.state==Bw16Active && !a.control.capture_deauth_reported);
    capture_reply(&a,"CAPTURE_DEAUTH:1234abcd:4\n");assert(!a.control.capture_deauth_reported);
    capture_reply(&a,"CAPTURE_DEAUTH:1234abcd:3\n");
    assert(a.control.capture_deauth_reported && a.control.capture_tx_attempts==3);
    capture_reply(&a,"CAPTURE_DEAUTH:1234abcd:0\n");assert(a.control.capture_tx_attempts==3);
    capture_wire(&a,0,100,true,0x1234abcd);capture_reply(&a,"CAPTURE_STATS:1234abcd:1:2\n");
    draw(canvas,&a);assert(strstr(rendered,"EAPOL: 1") && strstr(rendered,"TX attempts: 3") && strstr(rendered,"Drops: 2"));
    assert(!strstr(rendered,"Reconnect own client"));
    press(&a,InputKeyRight,InputTypeShort);draw(canvas,&a);
    assert(strstr(rendered,"capture_0001.pcap") && strstr(rendered,"Drops: 2"));
    press(&a,InputKeyRight,InputTypeShort);press(&a,InputKeyBack,InputTypeShort);
    unsigned before=sends;for(unsigned i=0;i<5;++i)press(&a,InputKeyBack,InputTypeShort);
    assert(sends==before && sd_opened && a.control.state==Bw16Stopping);
    capture_wire(&a,1,200,true,0x1234abcd);capture_reply(&a,"CAPTURE_DONE:1234abcd\n");
    assert(a.control.state==Bw16Stopped && !sd_opened && a.capture.eapol==2);
    draw(canvas,&a);assert(strstr(rendered,"Saved PCAP") && strstr(rendered,"TX attempts: 3") && strstr(rendered,"capture_0001.pcap"));
    assert(assisted_starts==1 && !passive_starts && !deauth_starts);

    capture_begin_mode(&a,true);capture_reply(&a,"CAPTURE_START:1234abcd\n");
    press(&a,InputKeyBack,InputTypeShort);capture_reply(&a,"CAPTURE_DEAUTH:1234abcd:0\nCAPTURE_DONE:1234abcd\n");
    draw(canvas,&a);assert(strstr(rendered,"TX attempts: 0") && !a.control.capture_tx_attempts);
    capture_begin_mode(&a,true);capture_reply(&a,"CAPTURE_START:1234abcd\n");
    press(&a,InputKeyBack,InputTypeShort);capture_reply(&a,"CAPTURE_DONE:1234abcd\n");
    draw(canvas,&a);assert(strstr(rendered,"TX attempts: unknown") && !a.control.capture_deauth_reported);

    capture_begin_mode(&a,true);capture_reply(&a,"CAPTURE_START:1234abcd\nCAPTURE_DEAUTH:1234abcd:3\n");
    press(&a,InputKeyBack,InputTypeShort);capture_reply(&a,"CAPTURE_DONE:1234abcd\n");
    press(&a,InputKeyBack,InputTypeShort);
    a.intent=MenuCapture;a.screen=Results;now+=500;
    press(&a,InputKeyOk,InputTypeShort);assert(!a.capture_deauth_pending);
    press(&a,InputKeyOk,InputTypeLong);
    assert(!a.control.capture_with_deauth && !a.control.capture_deauth_reported && !a.control.capture_tx_attempts);
    assert(!strcmp(command,"CAPTURE 0 1234abcd\n") && passive_starts==1 && assisted_starts==1 && !deauth_starts);
    capture_reply(&a,"CAPTURE_START:1234abcd\nCAPTURE_DEAUTH:1234abcd:3\n");
    assert(!a.control.capture_deauth_reported && !a.control.capture_tx_attempts);
    capture_reply(&a,"CAPTURE_DONE:1234abcd\n");

    capture_begin_mode(&a,true);capture_reply(&a,"CAPTURE_START:1234abcd\nCAPTURE_DEAUTH:1234abcd:3\n");
    press(&a,InputKeyBack,InputTypeShort);capture_reply(&a,"CAPTURE_DONE:1234abcd\n");
    press(&a,InputKeyBack,InputTypeShort);a.intent=MenuCaptureDeauth;a.screen=Results;now+=500;
    sd_fail_write=sd_writes+1;press(&a,InputKeyOk,InputTypeShort);press(&a,InputKeyOk,InputTypeLong);
    assert(a.screen==CaptureReport && assisted_starts==1 && !sd_opened);
    draw(canvas,&a);assert(strstr(rendered,"Capture not started") && !strstr(rendered,"TX attempts: 3"));

    for(unsigned fault=0;fault<3;++fault) {
        sd_reset();loaded(&a);a.intent=MenuCaptureDeauth;
        press(&a,InputKeyOk,InputTypeShort);
        if(!fault)sd_missing=true;else if(fault==1)sd_fail_write=1;else sd_fail_sync=1;
        press(&a,InputKeyOk,InputTypeLong);
        assert(a.screen==CaptureReport && a.capture.failed && !sd_opened && !sd_records && !sd_allocated);
        assert(!assisted_starts && !passive_starts && !deauth_starts && sends==1);
    }
    for(unsigned capability=0;capability<2;++capability) {
        sd_reset();setup(&a);a.control.capture_deauth_supported=false;
        a.control.capture_supported=capability!=0;a.menu=MenuCaptureDeauth;
        press(&a,InputKeyOk,InputTypeShort);
        assert(a.screen==Menu && !sends && !sd_opens && strstr(a.notice,"4.4"));
        a.menu=MenuCapture;press(&a,InputKeyOk,InputTypeShort);
        assert(!a.capture_deauth_pending && a.screen==(capability?Waiting:Menu));
        assert(!assisted_starts && !passive_starts && !deauth_starts);
    }
    puts("Capture + deauth UI: separate review, one assisted command after SD preflight, telemetry, STOP drain, passive reset, old-capability rejection and SD failure isolation PASS");
}

static void scan_diagnostics_tests(Canvas* canvas) {
    App a;setup(&a);press(&a,InputKeyOk,InputTypeShort);
    assert(a.screen==Waiting && !strcmp(command,"SCAN\n"));
    assert(now_ms()==1000 && furi_get_tick()==100); /* Clock units differ. */
    now=3000;poll_control(&a);assert(a.screen==Waiting && a.elapsed_ms==2000);
    now=20999;poll_control(&a);assert(a.screen==Waiting);
    now=21000;poll_control(&a);assert(a.screen==Error && a.elapsed_ms==20000 && !a.rx_bytes);
    draw(canvas,&a);assert(strstr(rendered,"No UART reply received") && strstr(rendered,"Wait:20s"));
    press(&a,InputKeyOk,InputTypeShort);poll_control(&a);assert(a.screen==Diagnostics && sends==1);
    draw(canvas,&a);assert(strstr(rendered,"RX bytes: 0") && strstr(rendered,"TX: SCAN"));
    press(&a,InputKeyOk,InputTypeShort);draw(canvas,&a);assert(strstr(rendered,"No complete line"));
    press(&a,InputKeyBack,InputTypeShort);assert(a.screen==Error);
    press(&a,InputKeyBack,InputTypeShort);assert(a.exit_requested);

    setup(&a);press(&a,InputKeyOk,InputTypeShort);
    const char boot[]="Other firmware ready\r\n";inject(&a,boot,5);inject(&a,boot+5,sizeof(boot)-6);
    assert(a.rx_bytes==sizeof(boot)-1 && a.rx_lines==1 && !a.rx_dropped);
    assert(!strcmp(a.last_line,"Other firmware ready"));
    now=21000;poll_control(&a);assert(a.screen==Error);
    draw(canvas,&a);assert(strstr(rendered,"No completion reply"));
    press(&a,InputKeyOk,InputTypeShort);press(&a,InputKeyOk,InputTypeShort);
    draw(canvas,&a);assert(strstr(rendered,"Other firmware ready"));
    press(&a,InputKeyOk,InputTypeShort);press(&a,InputKeyOk,InputTypeShort);
    draw(canvas,&a);assert(strstr(rendered,"4F 74 68 65") && sends==1);

    setup(&a);press(&a,InputKeyOk,InputTypeShort);
    inject(&a,"ERR scan busy\n",14);
    assert(a.screen==Error && !a.control.remote_unknown && sends==1);
    assert(strstr(a.control.error,"rejected scan/list"));

    setup(&a);press(&a,InputKeyOk,InputTypeShort);
    const uint8_t bad[]={0,0xff,'\n'};inject(&a,bad,sizeof(bad));
    assert(a.rx_bytes==3 && a.rx_dropped==1 && a.screen==Error && a.rx_first[1]==0xff);

    setup(&a);press(&a,InputKeyOk,InputTypeShort);now=2000;
    const char response[]="Lab,6,001122334455,-40\r\nSCAN_DONE\r\n";
    inject(&a,response,7);inject(&a,response+7,sizeof(response)-8);
    assert(a.screen==Waiting && a.index_pending && a.control.ap_count==1 && a.rx_lines==2);
    assert(!strcmp(a.last_line,"SCAN_DONE") && !a.transaction_running);
    now=2499;poll_control(&a);assert(a.screen==Waiting && sends==1);
    now=2500;poll_control(&a);assert(a.screen==Waiting && sends==2);
    assert(!strcmp(command,"DEAUTH_STATION\n"));
    const char indexed[]="LIST_STATION_START\nlist-station:Lab,6,001122334455,-40\nLIST_STATION_DONE\n";
    now=2600;inject(&a,indexed,sizeof(indexed)-1);
    assert(a.screen==Groups && a.selection_ready && a.control.indices_valid && a.elapsed_ms==100);
    now=30000;poll_control(&a);assert(a.screen==Groups && a.elapsed_ms==100);
    press(&a,InputKeyBack,InputTypeShort);a.menu=MenuDiagnostics;press(&a,InputKeyOk,InputTypeShort);
    assert(a.screen==Diagnostics && a.diagnostics_return==Menu && sends==2);
    press(&a,InputKeyBack,InputTypeShort);assert(a.screen==Menu);

    setup(&a);now=UINT32_MAX-1000;press(&a,InputKeyOk,InputTypeShort);
    now=18998;poll_control(&a);assert(a.screen==Waiting);
    now=18999;poll_control(&a);assert(a.screen==Error && a.elapsed_ms==20000);
    puts("UI scan: true 20-second deadline, clock-rate independence/wrap, silent UART, partial/invalid replies, fragmented success and passive diagnostics PASS");
}

static void startup_sync_tests(Canvas* canvas) {
    App a;setup(&a);a.screen=Connect;
    press(&a,InputKeyOk,InputTypeShort);
    assert(a.screen==Synchronizing && sends==1 && !strcmp(command," \n"));
    const uint8_t startup_echo[]={0,'\r','\n'};
    inject(&a,startup_echo,sizeof(startup_echo));
    assert(a.screen==Synchronizing && a.rx_dropped==1);
    for(unsigned i=0;i<4;++i)press(&a,InputKeyOk,InputTypeShort);
    assert(sends==1);now=1499;poll_control(&a);assert(a.screen==Synchronizing);
    now=1500;buffered_bytes=1;poll_control(&a);assert(sends==1);
    buffered_bytes=0;poll_control(&a);
    assert(a.screen==Synchronizing && a.caps_queried && a.control.state==Bw16Capabilities);
    assert(sends==2 && !strcmp(command,"BW16_CAPS\n") && !a.control.native_selected);
    assert(!a.rx_bytes && !a.rx_dropped);
    draw(canvas,&a);assert(strstr(rendered,"Checking BW16 support") && loading(&a));
    for(unsigned i=0;i<4;++i)press(&a,InputKeyOk,InputTypeShort);
    assert(sends==2);
    now=1600;inject(&a,"BW16_CAPS:1\n",12);
    assert(a.screen==Menu && a.control.native_selected && sends==2);
    now=2100;press(&a,InputKeyOk,InputTypeShort);
    assert(a.screen==Waiting && sends==3 && !strcmp(command,"SCAN\n"));
    const char reply[]="Lab,6,001122334455,-40\r\nSCAN_DONE\r\n";
    inject(&a,reply,sizeof(reply)-1);assert(a.screen==Waiting && a.index_pending && a.control.ap_count==1);
    now=2599;poll_control(&a);assert(sends==3);
    now=2600;poll_control(&a);assert(sends==4 && !strcmp(command,"DEAUTH_STATION\n"));
    const char indexed[]="LIST_STATION_START\nlist-station:Lab,6,001122334455,-40\nLIST_STATION_DONE\n";
    inject(&a,indexed,sizeof(indexed)-1);assert(a.screen==Groups && a.control.ap_count==1);

    setup(&a);a.screen=Connect;press(&a,InputKeyOk,InputTypeShort);
    now=1400;const char fragment[]="noise";inject(&a,fragment,sizeof(fragment)-1);
    now=1600;poll_control(&a);assert(a.screen==Synchronizing);
    now=1700;inject(&a,"\n",1);now=2199;poll_control(&a);assert(a.screen==Synchronizing);
    now=2200;poll_control(&a);assert(a.screen==Synchronizing && sends==2);
    /* Old BW16 ignores the capability query: startup still finishes after 1s. */
    now=3199;poll_control(&a);assert(a.screen==Synchronizing);
    now=3200;poll_control(&a);
    assert(a.screen==Menu && !a.control.native_selected && !a.control.restart_required && sends==2);
    inject(&a,"BW16_CAPS:1\n",12);assert(!a.control.native_selected);
    now+=500;press(&a,InputKeyOk,InputTypeShort);assert(a.screen==Waiting && !strcmp(command,"SCAN\n"));

    setup(&a);a.screen=Connect;press(&a,InputKeyOk,InputTypeShort);
    inject(&a,"partial",7);now=4000;poll_control(&a);assert(a.screen==Error && sends==1);
    draw(canvas,&a);assert(strstr(rendered,"UART did not settle"));
    setup(&a);a.screen=Connect;press(&a,InputKeyOk,InputTypeShort);
    press(&a,InputKeyBack,InputTypeShort);assert(a.exit_requested && sends==1);
    setup(&a);a.screen=Connect;press(&a,InputKeyOk,InputTypeShort);
    now+=500;poll_control(&a);assert(a.control.state==Bw16Capabilities);
    press(&a,InputKeyBack,InputTypeShort);assert(a.exit_requested && sends==2);
    puts("Startup: NUL/echo sync, quiet/buffer gating, one read-only capability query, exact reply, legacy 1s timeout and cancellation PASS");
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


static void menu_feature_tests(void) {
    for(unsigned item=0;item<MenuCount;++item) {
        App a;setup(&a);a.menu=item;press(&a,InputKeyOk,InputTypeShort);
        if(item==MenuDiagnostics) { assert(a.screen==Diagnostics && !sends);continue; }
        if(item==MenuInfo) { assert(a.screen==Info && !sends);continue; }
        if(item==MenuSelected) { assert(a.screen==Menu && !sends);continue; }
        assert(a.screen==Waiting && sends==1);
        if(item==MenuScan || item==MenuSelect || item==MenuPair) {
            assert(!strcmp(command,"SCAN\n"));continue;
        }
        assert(!strcmp(command,"DEAUTH_STATION\n"));
        bw16_control_line(&a.control,"LIST_STATION_START",now);
        bw16_control_line(&a.control,"list-station:Lab,6,001122334455,-40",now);
        bw16_control_line(&a.control,"LIST_STATION_DONE",now);sync_state(&a);
        if(item==MenuAll) {
            assert(a.screen==Confirm && a.pending==Bw16TargetAll);
            press(&a,InputKeyOk,InputTypeShort);assert(sends==1);
            press(&a,InputKeyOk,InputTypeLong);assert(!strcmp(command,"DEAUTH_ALL\n"));continue;
        }
        assert(a.screen==Results);press(&a,InputKeyOk,InputTypeShort);
        if(item==MenuStations) { assert(a.screen==ApActions);continue; }
        if(item==MenuCapture || item==MenuCaptureDeauth) {
            assert(a.screen==Confirm && a.pending==Bw16TargetCapture);
            assert(a.capture_deauth_pending==(item==MenuCaptureDeauth));continue;
        }
        if(item==MenuStation) {
            assert(a.screen==Confirm);press(&a,InputKeyOk,InputTypeLong);
            assert(!strcmp(command,"DEAUTH_STATION 0\n"));continue;
        }
        assert(a.screen==Waiting && !strcmp(command,"LIST_CLIENTS 0\n"));
        bw16_control_line(&a.control,"list-client:112233445566",now);
        bw16_control_line(&a.control,"LIST_CLIENTS_DONE",now);sync_state(&a);
        assert(a.screen==Clients);press(&a,InputKeyOk,InputTypeShort);
        assert(a.screen==Confirm);press(&a,InputKeyOk,InputTypeLong);
        assert(!strcmp(command,"DEAUTH_CLIENT 0 0\n"));
        bw16_control_line(&a.control,"TARGETED_START",now);sync_state(&a);
        press(&a,InputKeyBack,InputTypeShort);assert(!strcmp(command,"STOP\n"));
    }
    puts("UI: all thirteen menu options, station/client selection, legacy commands and STOP reachable PASS");
}

static const char grouped_scan[] =
    "Home,6,001122334400,-40\nHome,44,001122334401,-41\n"
    "LAB_0305,11,001122334402,-42\nLAB_0305_5G,36,001122334403,-43\n"
    "Nearby,149,001122334404,-44\nSCAN_DONE\n";
static const char grouped_index[] =
    "LIST_STATION_START\n"
    "list-station:Home,6,001122334400,-40\nlist-station:Home,44,001122334401,-41\n"
    "list-station:LAB_0305,11,001122334402,-42\nlist-station:LAB_0305_5G,36,001122334403,-43\n"
    "list-station:Nearby,149,001122334404,-44\nLIST_STATION_DONE\n";

static void grouped(App* a) {
    setup(a);a->menu=MenuScan;press(a,InputKeyOk,InputTypeShort);
    assert(a->screen==Waiting && sends==1 && !strcmp(command,"SCAN\n"));
    now=1100;inject(a,grouped_scan,sizeof(grouped_scan)-1);
    assert(a->screen==Waiting && a->index_pending && !a->selection_ready && sends==1);
    now=1599;poll_control(a);assert(sends==1);
    now=1600;poll_control(a);assert(sends==2 && !strcmp(command,"DEAUTH_STATION\n"));
    now=1700;inject(a,grouped_index,sizeof(grouped_index)-1);
    assert(a->screen==Groups && a->selection_ready && a->control.indices_valid);
    assert(a->selection.ap_count==5 && a->selection.group_count==4);
    assert(!bw16_selection_count(&a->selection));
}

static void grouping_ui_tests(Canvas* canvas) {
    App a;grouped(&a);
    assert(a.selection.groups[0].count==2);
    assert(a.selection.groups[0].bands==(BW16_BAND_24|BW16_BAND_5));
    press(&a,InputKeyOk,InputTypeShort);
    assert(a.selection.selected[0] && a.selection.selected[1] && bw16_selection_count(&a.selection)==2);
    assert(!a.selection.selected[2] && !a.selection.selected[3] && !a.selection.selected[4]);
    press(&a,InputKeyDown,InputTypeShort);press(&a,InputKeyOk,InputTypeShort);
    assert(bw16_selection_count(&a.selection)==3 && a.selection.selected[2] && !a.selection.selected[3]);
    press(&a,InputKeyOk,InputTypeShort);assert(bw16_selection_count(&a.selection)==2);
    press(&a,InputKeyUp,InputTypeShort);press(&a,InputKeyOk,InputTypeShort);
    assert(!bw16_selection_count(&a.selection) && sends==2);

    press(&a,InputKeyBack,InputTypeShort);a.menu=MenuPair;press(&a,InputKeyOk,InputTypeShort);
    assert(a.screen==PairNames && a.pair_first==BW16_MAX_APS && sends==2);
    press(&a,InputKeyDown,InputTypeShort);assert(a.group==1);
    press(&a,InputKeyOk,InputTypeShort);assert(a.pair_first==1 && a.group==2);
    draw(canvas,&a);assert(strstr(rendered,"LAB_0305_5G") && strstr(rendered,"With: LAB_0305"));
    press(&a,InputKeyOk,InputTypeShort);assert(a.screen==Groups && a.selection.group_count==3);
    assert(a.selection.group_for_ap[2]==a.selection.group_for_ap[3]);
    assert(a.selection.group_for_ap[4]!=a.selection.group_for_ap[2]);
    assert(!bw16_selection_count(&a.selection));
    press(&a,InputKeyOk,InputTypeShort);assert(bw16_selection_count(&a.selection)==2);
    assert(a.selection.selected[2] && a.selection.selected[3] && !a.selection.selected[4]);
    press(&a,InputKeyUp,InputTypeShort);press(&a,InputKeyOk,InputTypeShort);
    assert(bw16_selection_count(&a.selection)==4 && !a.selection.selected[4] && sends==2);
    press(&a,InputKeyOk,InputTypeLong);assert(a.screen==Confirm && a.confirm_batch && sends==2);
    const unsigned channels[]={6,44,11,36};
    for(unsigned i=0;i<4;++i) {
        draw(canvas,&a);
        char mac[18],channel[16];snprintf(mac,sizeof(mac),"00112233440%u",i);
        snprintf(channel,sizeof(channel),"CH%u",channels[i]);
        assert(strstr(rendered,mac) && strstr(rendered,channel));
        assert(strstr(rendered,channels[i]<=14?"2.4 GHz":"5 GHz"));
        press(&a,InputKeyOk,InputTypeShort);assert(a.screen==Confirm && sends==2);
        if(i<3)press(&a,InputKeyDown,InputTypeShort);
    }
    press(&a,InputKeyDown,InputTypeRepeat);assert(a.review==3 && sends==2);
    press(&a,InputKeyBack,InputTypeShort);assert(a.screen==Groups);
    press(&a,InputKeyBack,InputTypeShort);assert(a.screen==Menu);
    a.menu=MenuSelected;press(&a,InputKeyOk,InputTypeShort);
    assert(a.screen==Confirm && a.confirm_batch && a.review==0 && sends==2);
    /* A long confirmation still cannot send during recent UART activity. */
    press(&a,InputKeyOk,InputTypeLong);assert(a.screen==Confirm && sends==2);
    now=2200;press(&a,InputKeyOk,InputTypeLong);
    assert(a.screen==Operation && a.batch.running && a.batch.count==4 && sends==3);
    assert(!strcmp(command,"DEAUTH_SELECTED 000000000000f\n"));
    for(unsigned i=0;i<4;++i)assert(a.batch.targets[i]==i);
    press(&a,InputKeyBack,InputTypeShort);assert(!strcmp(command,"STOP\n"));
    inject(&a,"SELECTED_DONE\n",14);assert(!a.batch.running && a.control.state==Bw16Stopped);
    press(&a,InputKeyBack,InputTypeShort);assert(a.screen==Menu);
    now+=500;a.menu=MenuScan;press(&a,InputKeyOk,InputTypeShort);
    assert(a.screen==Waiting && !a.selection_ready && !a.selection.group_count);
    assert(!bw16_selection_count(&a.selection));
    now+=10;inject(&a,grouped_scan,sizeof(grouped_scan)-1);
    now+=500;poll_control(&a);assert(!strcmp(command,"DEAUTH_STATION\n"));
    inject(&a,grouped_index,sizeof(grouped_index)-1);
    assert(a.screen==Groups && a.selection.group_count==4 && !bw16_selection_count(&a.selection));
    assert(a.selection.group_for_ap[2]!=a.selection.group_for_ap[3]);
    puts("UI selection: exact dual-band groups, manual different-name pair, multiple marking, BSSID/channel review, long confirmation and rescan reset PASS");
}

static void start_two(App* a) {
    grouped(a);press(a,InputKeyOk,InputTypeShort);press(a,InputKeyOk,InputTypeLong);
    assert(a->screen==Confirm && bw16_selection_count(&a->selection)==2);
    now=2200;press(a,InputKeyOk,InputTypeLong);
    assert(a->batch.running && a->batch.native && a->batch.count==2 && sends==3);
    assert(a->control.state==Bw16Starting && !strcmp(command,"DEAUTH_SELECTED 0000000000003\n"));
}
static void selected_active(App* a) {
    now+=10;inject(a,"SELECTED_START\n",15);
    assert(a->control.state==Bw16Active && a->batch.active_seen && sends==3);
}
static void selected_stop(App* a) {
    selected_active(a);press(a,InputKeyBack,InputTypeShort);
    assert(a->control.state==Bw16Stopping && sends==4 && !strcmp(command,"STOP\n"));
}
static void batch_ui_tests(Canvas* canvas) {
    App a;start_two(&a);
    now+=1000;poll_control(&a);assert(a.control.state==Bw16Starting && sends==3);
    selected_active(&a);
    inject(&a,"SELECTED_AP:1\n",14);
    assert(a.control.selected_ap==1 && a.batch.position==1 && sends==3);
    draw(canvas,&a);assert(strstr(rendered,"5 GHz CH44"));
    for(unsigned i=0;i<10;++i) {
        now+=1000;poll_control(&a);
        assert(a.control.state==Bw16Active && sends==3); /* No host per-target cycling. */
    }
    buffered_bytes=1;now+=100;poll_control(&a);assert(sends==3);
    buffered_bytes=0;
    press(&a,InputKeyBack,InputTypeShort);
    assert(a.batch.cancel_requested && sends==4 && !strcmp(command,"STOP\n"));
    for(unsigned i=0;i<5;++i)press(&a,InputKeyBack,InputTypeShort);
    assert(sends==4 && !a.exit_requested);
    inject(&a,"TARGETED_DONE\n",14);assert(a.control.state==Bw16Stopping);
    inject(&a,"SELECTED_DONE\n",14);assert(!a.batch.running && a.control.state==Bw16Stopped);
    now+=1000;poll_control(&a);assert(sends==4);
    draw(canvas,&a);assert(strstr(rendered,"Device confirmed stop") && !loading(&a));
    press(&a,InputKeyBack,InputTypeShort);assert(a.screen==Menu);

    start_two(&a);selected_stop(&a);now+=BW16_STOP_TIMEOUT_MS;poll_control(&a);
    assert(a.screen==Error && !a.batch.running && a.control.remote_unknown && sends==4);
    now+=10000;poll_control(&a);assert(sends==4);
    press(&a,InputKeyBack,InputTypeShort);assert(a.screen==Unplug);

    start_two(&a);selected_active(&a);inject(&a,"SELECTED_AP:4\n",14);
    assert(a.control.state==Bw16Stopping && a.control.restart_required && sends==4);
    inject(&a,"SELECTED_DONE\n",14);
    assert(a.screen==Error && !a.batch.running && !a.control.remote_unknown && sends==4);

    start_two(&a);selected_active(&a);
    const uint8_t corrupt[]={0,'\n'};inject(&a,corrupt,sizeof(corrupt));
    assert(a.control.state==Bw16Stopping && sends==4);
    inject(&a,"SELECTED_DONE\n",14);now+=1000;poll_control(&a);
    assert(a.screen==Error && !a.batch.running && sends==4);

    start_two(&a);selected_active(&a);
    now=a.batch.started+BW16_OPERATION_MS-1;poll_control(&a);
    assert(a.control.state==Bw16Active && sends==3);
    ++now;poll_control(&a);assert(a.control.state==Bw16Stopping && sends==4);
    bw16_control_line(&a.control,"SELECTED_DONE",now+1);a.dirty=false;poll_control(&a);
    assert(!a.batch.running && a.dirty && a.control.state==Bw16Stopped && sends==4);
    draw(canvas,&a);assert(strstr(rendered,"Device confirmed stop") && !loading(&a));

    /* START and final status can share a single UART receive chunk. */
    start_two(&a);
    const char complete[]="SELECTED_START\nTX:7\nSELECTED_DONE\n";
    inject(&a,complete,sizeof(complete)-1);
    assert(a.control.state==Bw16Stopped && !a.batch.running && !a.control.restart_required);
    assert(bw16_batch_counter(&a.batch,&a.control)==7 && sends==3);

    setup(&a);press(&a,InputKeyOk,InputTypeShort);
    now+=10;inject(&a,grouped_scan,sizeof(grouped_scan)-1);
    assert(a.index_pending && a.control.state==Bw16Idle && sends==1);
    inject(&a,corrupt,sizeof(corrupt));now+=1000;poll_control(&a);
    assert(a.screen==Error && sends==1);
    puts("UI native batch: one mask, reported selected band, no host cycling, whole-session STOP, timeout/RX/index faults and completion redraw PASS");
}
static void legacy_selected_ui_tests(Canvas* canvas) {
    App a;grouped(&a);a.control.native_selected=false;
    press(&a,InputKeyOk,InputTypeShort);press(&a,InputKeyOk,InputTypeLong);
    assert(a.screen==Confirm && a.confirm_batch);
    draw(canvas,&a);assert(strstr(rendered,"BW16 update needed"));
    now=2200;
    for(unsigned i=0;i<4;++i)press(&a,InputKeyOk,InputTypeLong);
    assert(a.screen==Confirm && !a.batch.running && sends==2 && !a.control.remote_unknown);
    assert(strstr(a.notice,"BW16 update needed"));
    press(&a,InputKeyBack,InputTypeShort);assert(a.screen==Groups);
    press(&a,InputKeyOk,InputTypeShort); /* Clear same-name dual-band group. */
    press(&a,InputKeyDown,InputTypeShort);press(&a,InputKeyOk,InputTypeShort);
    assert(bw16_selection_count(&a.selection)==1);
    press(&a,InputKeyOk,InputTypeLong);press(&a,InputKeyOk,InputTypeLong);
    assert(a.screen==Operation && a.batch.running && !a.batch.native && sends==3);
    assert(!strcmp(command,"DEAUTH_STATION 2\n"));
    press(&a,InputKeyOk,InputTypeLong);assert(a.control.state==Bw16Stopping && sends==4);
    inject(&a,"TARGETED_DONE\n",14);assert(!a.batch.running && a.control.state==Bw16Stopped);

    grouped(&a);press(&a,InputKeyOk,InputTypeShort);press(&a,InputKeyOk,InputTypeLong);
    a.control.indices_valid=false;now=2200;press(&a,InputKeyOk,InputTypeLong);
    assert(sends==2 && !a.batch.running && a.screen==Confirm);
    puts("UI old firmware: explicit multi-target update prompt, no fallback/all operation, single-target compatibility and invalid-cache refusal PASS");
}

static void spinner_tests(Canvas* canvas) {
    App a;setup(&a);a.screen=Synchronizing;a.sync_started=now;
    unsigned before=sends;unsigned char first[1024];dots_drawn=0;
    draw(canvas,&a);assert(dots_drawn>0);memcpy(first,u8g2_GetBufferPtr(canvas),sizeof(first));
    now+=100;draw(canvas,&a);assert(memcmp(first,u8g2_GetBufferPtr(canvas),sizeof(first)));
    assert(sends==before);a.screen=Menu;dots_drawn=0;draw(canvas,&a);assert(!dots_drawn);
    a.screen=Waiting;draw(canvas,&a);assert(dots_drawn>0);
    a.screen=Operation;a.control.state=Bw16Starting;dots_drawn=0;draw(canvas,&a);assert(dots_drawn>0);
    a.control.state=Bw16Stopping;dots_drawn=0;draw(canvas,&a);assert(dots_drawn>0);
    assert(sends==before);

    /* Exercise main-loop redraw scheduling while there are no received bytes. */
    alloc_call=0;fail_alloc=0;event_pos=0;event_count=7;waiting_updates=viewport_updates=0;
    events[0]=(InputEvent){.key=InputKeyOk,.type=InputTypeShort};
    events[1]=(InputEvent){.key=InputKeyUp,.type=InputTypePress};
    events[2]=(InputEvent){.key=InputKeyUp,.type=InputTypePress};
    events[3]=(InputEvent){.key=InputKeyOk,.type=InputTypeShort};
    events[4]=(InputEvent){.key=InputKeyUp,.type=InputTypePress};
    events[5]=(InputEvent){.key=InputKeyUp,.type=InputTypePress};
    events[6]=(InputEvent){.key=InputKeyBack,.type=InputTypeShort};
    assert(bw16_r4tkn_app(NULL)==0 && !allocated && !attached && !records);
    assert(waiting_updates>=3 && viewport_updates>=4);
    puts("UI loading: changing 100 ms native frames, passive rendering, busy main-loop redraw PASS");
}

static void max_groups(App* a) {
    setup(a);a->control.ap_count=a->control.ap_total=BW16_MAX_APS;
    a->control.aps_valid=a->control.indices_valid=true;
    for(unsigned i=0;i<BW16_MAX_APS;++i) {
        Bw16Ap* ap=&a->control.aps[i];
        snprintf(ap->ssid,sizeof(ap->ssid),"WWWWWWWWWWWWWWWWWWWWWWWWWWW%05u",i);
        memcpy(ap->raw_ssid,ap->ssid,sizeof(ap->raw_ssid));
        snprintf(ap->mac,sizeof(ap->mac),"AA:BB:CC:DD:EE:%02X",i);
        ap->channel=i%2?149:6;ap->rssi=-99;
    }
    bw16_selection_build(&a->selection,a->control.aps,BW16_MAX_APS);
    a->selection_ready=true;a->pair_first=BW16_MAX_APS;a->screen=Groups;
    for(unsigned i=0;i<BW16_MAX_APS;++i)assert(bw16_selection_toggle_group(&a->selection,i));
}

int main(int argc,char** argv) {
    setvbuf(stdout,NULL,_IONBF,0);assert(argc==2);input_tests();entry_tests();menu_feature_tests();
    Canvas canvas;u8g2_Setup_st756x_flipper(&canvas,U8G2_R0,u8x8_hw_spi_esp32,u8g2_gpio_and_delay_esp32);
    u8g2_SetFontMode(&canvas,1);u8g2_SetFontPosBaseline(&canvas);
    scan_diagnostics_tests(&canvas);startup_sync_tests(&canvas);capture_tests(&canvas);capture_deauth_tests(&canvas);
    grouping_ui_tests(&canvas);batch_ui_tests(&canvas);legacy_selected_ui_tests(&canvas);spinner_tests(&canvas);
    unsigned char pages[60][1024];App a;
    for(unsigned i=0;i<60;++i) {
        loaded(&a);a.control.client_count=1;strcpy(a.control.clients[0],"112233445566");
        a.pending=Bw16TargetClient;a.control.counter=UINT32_MAX;
        if(i<15) {
            a.screen=(Screen)i;
            if(a.screen==Groups || a.screen==PairNames) {
                bw16_selection_build(&a.selection,a.control.aps,a.control.ap_count);
                a.selection_ready=true;a.pair_first=BW16_MAX_APS;
            }
        } else if(i<26) { a.screen=Menu;a.menu=i-15; }
        else if(i==26) { a.screen=Operation;a.control.state=Bw16Stopped; }
        else if(i==27) { a.screen=Error;a.setup_failed=true;a.io.error=0x108;strcpy(a.control.error,"UART install failed"); }
        else if(i==28) { a.screen=Error;a.control.remote_unknown=true; }
        else {
            max_groups(&a);a.group=49;a.review=49;
            if(i==29) a.screen=Groups;
            else if(i==30 || i==31) { a.screen=PairNames;if(i==31)a.pair_first=0; }
            else if(i==32) { a.screen=Confirm;a.confirm_batch=true; }
            else if(i==33) {
                bw16_selection_link_groups(&a.selection,0,1);a.group=0;
                bw16_selection_toggle_ap(&a.selection,1);
            } else if(i>=34 && i<=38) {
                a.screen=Operation;a.confirm_batch=true;a.batch.count=50;a.batch.position=49;
                for(unsigned j=0;j<50;++j)a.batch.targets[j]=j;
                a.batch.running=i!=38;a.control.counter=UINT32_MAX;
                a.control.state=i==34?Bw16Starting:i==35?Bw16Active:i==36?Bw16Stopping:Bw16Stopped;
            } else if(i==39) {
                a.screen=Diagnostics;a.rx_bytes=a.rx_lines=a.rx_dropped=UINT32_MAX;
                a.elapsed_ms=UINT32_MAX;strcpy(a.last_command,"DEAUTH_STATION 49");
            } else if(i==40) {
                a.screen=Diagnostics;a.diagnostics_page=1;
                memset(a.last_line,'W',BW16_LINE_SIZE-1);a.last_line[BW16_LINE_SIZE-1]=0;
            } else if(i==41) {
                a.screen=Diagnostics;a.diagnostics_page=3;a.rx_first_count=8;
                memset(a.rx_first,0xff,sizeof(a.rx_first));
            } else if(i==42) {
                bw16_selection_link_groups(&a.selection,0,1);a.group=0;
            } else { a.screen=Groups;bw16_selection_build(&a.selection,NULL,0); }
        }
        if(i>=44) {
            a.screen=Operation;a.control.target=Bw16TargetCapture;a.capture.eapol=12;a.capture.packets=14;
            a.capture.dropped=2;strcpy(a.capture.path,BW16_CAPTURE_DIRECTORY "/capture_0001.pcap");
            a.control.state=i==44?Bw16Starting:i==46?Bw16Stopping:i==47?Bw16Stopped:Bw16Active;
            a.capture_details=i==48 || i==50;
            if(i==49 || i==50){a.screen=Error;a.capture.created=a.capture.failed=true;strcpy(a.capture.error,"SD write failed; partial file");}
            if(i==51){a.screen=Confirm;a.confirm_batch=false;a.pending=Bw16TargetCapture;}
        }
        if(i>=52) {
            a.capture_details=false;a.control.capture_with_deauth=true;
            a.control.capture_deauth_reported=i!=52 && i!=56;
            a.control.capture_tx_attempts=i==54?0:3;
            a.control.state=i==52?Bw16Starting:i==55 || i==56?Bw16Stopped:
                i==54?Bw16Stopping:Bw16Active;
            if(i==57) a.capture_details=true;
            if(i==58) {a.screen=Confirm;a.pending=Bw16TargetCapture;a.capture_deauth_pending=true;a.confirm_batch=false;}
            if(i==59) {a.screen=Menu;a.menu=MenuCaptureDeauth;}
        }
        draw(&canvas,&a);memcpy(pages[i],u8g2_GetBufferPtr(&canvas),1024);
    }
    FILE* file=fopen(argv[1],"wb");assert(file);fprintf(file,"P5\n1596 3432\n255\n");
    for(unsigned y=0;y<3432;++y)for(unsigned x=0;x<1596;++x) {
        unsigned gx=x/396,gy=y/228,lx=x%396,ly=y%228;unsigned char pixel=205;
        if(gx<4 && gy<15 && lx>=12 && ly>=18 && lx<396 && ly<210) {
            unsigned px=(lx-12)/3,py=(ly-18)/3;pixel=(pages[gy*4+gx][px+128*(py/8)]&(1U<<(py%8)))?16:248;
        }
        fputc(pixel,file);
    }
    fclose(file);puts("UI: 60 native-font screens, 50 groups, long names, pairs/reviews/max counters; canvas bounds checked PASS");
}
