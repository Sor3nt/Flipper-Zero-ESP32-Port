/* WirelessPi ESP32 port of coolshrimp/flipperzero-walkie-talkie (MIT).
 * Receive-only channel monitor; pulse audio is experimental, NOT FM voice. */
#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_speaker.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <lib/subghz/devices/devices.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <esp_timer.h>

static const uint32_t channels[22] = {
462562500,462587500,462612500,462637500,462662500,462687500,462712500,
467562500,467587500,467612500,467637500,467662500,467687500,467712500,
462550000,462575000,462600000,462625000,462650000,462675000,462700000,462725000};
#define LOAD(x) __atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define STORE(x,v) __atomic_store_n(&(x),(v),__ATOMIC_RELEASE)
typedef struct {uint32_t us, epoch, captured; bool level;} Pulse;
typedef struct {
    FuriMessageQueue *events, *pulses;
    FuriMutex* mutex;
    FuriThread* audio;
    ViewPort* view;
    const SubGhzDevice* radio;
    bool running, begun, rx, scan, held, mute, squelch, edit;
    int page, cursor, channel, label, threshold, direction;
    float rssi;
    uint32_t tuned, last_signal, confirmations;
    /* Cross-thread values accessed only through atomic load/store. */
    uint32_t audio_run, enabled, epoch, volume, status, dropped;
} App;
static int wrap(int n,int count) { return (n+count)%count; }
static void capture(bool level,uint32_t us,void* context) {
    App* a=context;
    if(!LOAD(a->enabled)) return;
    Pulse p={us,LOAD(a->epoch),(uint32_t)esp_timer_get_time(),level};
    if(furi_message_queue_put(a->pulses,&p,0)!=FuriStatusOk)
        __atomic_fetch_add(&a->dropped,1,__ATOMIC_RELAXED);
}
static int32_t audio_worker(void* context) {
    App* a=context;
    if(!furi_hal_speaker_acquire(100)) {STORE(a->status,2); return 0;}
    furi_hal_speaker_stop(); STORE(a->status,1);
    int16_t pcm[128]; size_t used=0;
    uint32_t fraction=0, epoch=LOAD(a->epoch), drops=0;
    int32_t dc=0;
    while(LOAD(a->audio_run)) {
        Pulse p;
        if(furi_message_queue_get(a->pulses,&p,furi_ms_to_ticks(10))!=FuriStatusOk) {
            /* End bursts with silence; never repeat the last noisy buffer. */
            if(used) { memset(pcm+used,0,(128-used)*sizeof(*pcm));
                if(!furi_hal_speaker_write_pcm(pcm,128)) {STORE(a->status,3); break;} }
            used=0; fraction=0; dc=0; continue;
        }
        uint32_t current=LOAD(a->epoch), lost=LOAD(a->dropped);
        if(current!=epoch || lost!=drops) {used=0; fraction=0; dc=0; epoch=current; drops=lost;}
        if(!LOAD(a->enabled) || p.epoch!=current ||
           (uint32_t)((uint32_t)esp_timer_get_time()-p.captured)>40000) {used=0; continue;}
        /* A long edge gap is silence, not seconds of DC at the speaker. */
        if(p.us>20000) {used=0; fraction=0; dc=0; continue;}
        uint32_t scaled=p.us*441u+fraction;
        uint32_t count=scaled/10000u; fraction=scaled%10000u;
        int32_t v=LOAD(a->volume), amplitude=v*v*100;
        int32_t value=p.level?amplitude:-amplitude;
        while(count--) {
            dc+=(value-dc)/256;
            pcm[used++]=(int16_t)(value-dc);
            if(used==128) {
                if(!LOAD(a->enabled) || LOAD(a->epoch)!=epoch) {used=0;break;}
                if(!furi_hal_speaker_write_pcm(pcm,128)) {STORE(a->status,3);STORE(a->audio_run,0);break;}
                used=0;
            }
        }
    }
    memset(pcm,0,sizeof(pcm)); furi_hal_speaker_write_pcm(pcm,128);
    furi_hal_speaker_release(); return 0;
}
static void stop_capture(App* a) {
    STORE(a->enabled,0); __atomic_fetch_add(&a->epoch,1,__ATOMIC_RELEASE);
    if(a->rx) {subghz_devices_stop_async_rx(a->radio);a->rx=false;}
}
static void tune(App* a) {
    stop_capture(a); subghz_devices_idle(a->radio);
    subghz_devices_set_frequency(a->radio,channels[a->channel]);
    subghz_devices_set_rx(a->radio); a->tuned=furi_get_tick();
    a->rssi=-127; a->confirmations=0;
}
static void gate_audio(App* a) {
    bool on=a->begun && a->page==0 && !a->mute && LOAD(a->status)==1 &&
        (!a->scan || a->held) && (!a->squelch || a->rssi>=a->threshold) &&
        (uint32_t)(furi_get_tick()-a->tuned)>=furi_ms_to_ticks(75);
    if(on && !a->rx) {
        STORE(a->enabled,1);subghz_devices_start_async_rx(a->radio,capture,a);a->rx=true;
    } else if(!on && a->rx) {stop_capture(a);subghz_devices_set_rx(a->radio);}
}
static void input(InputEvent* e,void* context) {furi_message_queue_put(context,e,0);}
static void line(Canvas* c,int y,const char* s) {canvas_draw_str(c,2,y,s);}
static void draw(Canvas* c,void* context) {
    App* a=context; furi_mutex_acquire(a->mutex,FuriWaitForever);
    canvas_clear(c);canvas_set_font(c,FontPrimary);line(c,9,"Walkie Talkie");
    canvas_set_font(c,FontSecondary); char s[40];
    if(!a->begun) {line(c,28,"Radio unavailable");line(c,45,"Back: exit");}
    else if(a->page==0) {
        snprintf(s,sizeof(s),"CH %02d  SUB %02d (label)",a->channel+1,a->label);line(c,22,s);
        uint32_t f=channels[a->channel];snprintf(s,sizeof(s),"%lu.%04lu MHz",(unsigned long)(f/1000000),(unsigned long)((f%1000000)/100));line(c,33,s);
        snprintf(s,sizeof(s),"%d dBm  %s",(int)a->rssi,a->scan?(a->held?"HOLD":(a->direction>0?"SCAN UP":"SCAN DN")):"MANUAL");line(c,44,s);
        uint32_t st=LOAD(a->status);
        line(c,55,st==2?"Speaker busy":st==3?"Audio write error":a->mute?"Muted | OK: sound":"Experimental sound");
        int bars=a->rssi>-50?5:a->rssi>-60?4:a->rssi>-70?3:a->rssi>-85?2:a->rssi>-100?1:0;
        for(int i=0;i<5;i++) {if(i<bars) canvas_draw_box(c,98+i*6,10-i*2,4,3+i*2);else canvas_draw_frame(c,98+i*6,10-i*2,4,3+i*2);}
        line(c,64,"Hold OK: scan  Back: menu");
    } else if(a->page==1) {
        const char* items[]={"Monitor","Settings","FRS channels","About / diagnostics","Exit"};
        for(int i=0;i<5;i++){snprintf(s,sizeof(s),"%c %s",i==a->cursor?'>':' ',items[i]);line(c,22+i*10,s);}
    } else if(a->page==2) {
        int start=a->cursor>3?a->cursor-3:0;
        for(int i=start;i<start+4 && i<6;i++) {
            char v[32];
            switch(i) {
            case 0:snprintf(v,sizeof(v),"Auto squelch: %s",a->squelch?"ON":"OFF");break;
            case 1:snprintf(v,sizeof(v),"Threshold: %d dBm",a->threshold);break;
            case 2:snprintf(v,sizeof(v),"Direction: %s",a->direction>0?"UP":"DOWN");break;
            case 3:snprintf(v,sizeof(v),"Sub label: %02d",a->label);break;
            case 4:snprintf(v,sizeof(v),"Volume: %lu/10",(unsigned long)LOAD(a->volume));break;
            default:snprintf(v,sizeof(v),"Reset settings");break;
            }
            snprintf(s,sizeof(s),"%c %s",i==a->cursor?(a->edit?'*':'>'):' ',v);line(c,22+(i-start)*10,s);
        }
        line(c,64,a->edit?"Turn: adjust  OK: save":"Turn: select  OK: edit");
    } else if(a->page==3) {
        int start=a->cursor>2?a->cursor-2:0;if(start>17)start=17;
        for(int i=start;i<start+5;i++) {uint32_t f=channels[i];snprintf(s,sizeof(s),"%c %02d %lu.%04lu MHz",a->cursor==i?'>':' ',i+1,(unsigned long)(f/1000000),(unsigned long)((f%1000000)/100));line(c,22+(i-start)*10,s);}
    } else {
        line(c,21,"coolshrimp / Native 1.2");line(c,31,"RX only; no voice decode");line(c,41,"Sub labels: no CTCSS");
        snprintf(s,sizeof(s),"Audio dropped: %lu",(unsigned long)LOAD(a->dropped));line(c,51,s);
        line(c,63,"467 MHz: experimental");
    }
    furi_mutex_release(a->mutex);
}
static void change_setting(App* a,int d) {
    switch(a->cursor) {
    case 0:a->squelch=!a->squelch;break;
    case 1:a->threshold+=d;if(a->threshold<-120)a->threshold=-120;if(a->threshold>-30)a->threshold=-30;break;
    case 2:a->direction=-a->direction;break;
    case 3:a->label=wrap(a->label+d,39);break;
    case 4:{int v=(int)LOAD(a->volume)+d;if(v<1)v=1;if(v>10)v=10;STORE(a->volume,v);break;}
    }
}
static void handle(App* a,InputEvent e) {
    if(e.type==InputTypeLong && e.key==InputKeyBack) {a->running=false;return;}
    if(!a->begun) {if(e.key==InputKeyBack)a->running=false;return;}
    if(e.type==InputTypeLong && e.key==InputKeyOk && a->page==0) {
        if(a->scan && a->held) {a->held=false;a->channel=wrap(a->channel+a->direction,22);tune(a);}
        else {a->scan=!a->scan;a->held=false;}return;
    }
    if(e.type!=InputTypeShort && e.type!=InputTypeRepeat)return;
    if(e.key==InputKeyBack && e.type==InputTypeShort) {
        if(a->edit)a->edit=false;
        else {a->page=a->page==1?0:1;a->cursor=0;}return;
    }
    int d=e.key==InputKeyDown || e.key==InputKeyRight?1:e.key==InputKeyUp || e.key==InputKeyLeft?-1:0;
    if(d) {
        if(a->page==0){a->scan=false;a->held=false;a->channel=wrap(a->channel+d,22);tune(a);}
        else if(a->page==1)a->cursor=wrap(a->cursor+d,5);
        else if(a->page==2){if(a->edit)change_setting(a,d);else a->cursor=wrap(a->cursor+d,6);}
        else if(a->page==3)a->cursor=wrap(a->cursor+d,22);
    } else if(e.key==InputKeyOk && e.type==InputTypeShort) {
        if(a->page==0)a->mute=!a->mute;
        else if(a->page==1){int n=a->cursor;if(n==4)a->running=false;else a->page=n==0?0:n+1;a->cursor=0;}
        else if(a->page==2){if(a->cursor==5){a->squelch=true;a->threshold=-68;a->direction=1;a->label=0;STORE(a->volume,3);a->edit=false;}else a->edit=!a->edit;}
        else if(a->page==3){a->channel=a->cursor;a->scan=false;a->held=false;a->page=0;tune(a);}
    }
}
int32_t walkie_talkie_app(void* p) {
    UNUSED(p);App* a=calloc(1,sizeof(*a));if(!a)return -1;
    a->events=furi_message_queue_alloc(16,sizeof(InputEvent));
    a->pulses=furi_message_queue_alloc(256,sizeof(Pulse));
    a->mutex=furi_mutex_alloc(FuriMutexTypeNormal);a->view=view_port_alloc();
    if(!a->events || !a->pulses || !a->mutex || !a->view) {
        if(a->events)furi_message_queue_free(a->events);
        if(a->pulses)furi_message_queue_free(a->pulses);
        if(a->mutex)furi_mutex_free(a->mutex);
        if(a->view)view_port_free(a->view);
        free(a);return -1;
    }
    a->running=true;a->direction=1;a->threshold=-68;a->squelch=true;a->rssi=-127;
    STORE(a->volume,3);STORE(a->audio_run,1);
    subghz_devices_init();a->radio=subghz_devices_get_by_name("cc1101_int");
    if(a->radio && subghz_devices_is_connect(a->radio)) {
        /* Internal device has no begin callback; generic begin returns false. */
        a->begun=subghz_devices_is_frequency_valid(a->radio,channels[0]);
        if(a->begun){furi_hal_power_insomnia_enter();subghz_devices_reset(a->radio);subghz_devices_load_preset(a->radio,FuriHalSubGhzPreset2FSKDev238Async,NULL);tune(a);}
    }
    if(a->begun){a->audio=furi_thread_alloc_ex("WalkieAudio",4096,audio_worker,a);if(a->audio)furi_thread_start(a->audio);else STORE(a->status,3);}
    view_port_draw_callback_set(a->view,draw,a);view_port_input_callback_set(a->view,input,a->events);
    Gui* gui=furi_record_open(RECORD_GUI);gui_add_view_port(gui,a->view,GuiLayerFullscreen);
    uint32_t drawn=0;
    while(a->running) {
        InputEvent e;bool event=furi_message_queue_get(a->events,&e,furi_ms_to_ticks(20))==FuriStatusOk;
        furi_mutex_acquire(a->mutex,FuriWaitForever);
        if(event)handle(a,e);
        uint32_t now=furi_get_tick();
        if(a->begun && (uint32_t)(now-a->tuned)>=furi_ms_to_ticks(75)) {
            a->rssi=subghz_devices_get_rssi(a->radio);
            if(a->scan && a->page==0) {
                bool signal=a->rssi>=a->threshold;
                if(a->held) {if(signal)a->last_signal=now;else if((uint32_t)(now-a->last_signal)>=furi_ms_to_ticks(500)){a->held=false;a->channel=wrap(a->channel+a->direction,22);tune(a);}}
                else if(signal){if(++a->confirmations>=2){a->held=true;a->last_signal=now;}}
                else {a->channel=wrap(a->channel+a->direction,22);tune(a);}
            }
        }
        gate_audio(a);furi_mutex_release(a->mutex);
        if(event || (uint32_t)(now-drawn)>=furi_ms_to_ticks(100)){view_port_update(a->view);drawn=now;}
    }
    stop_capture(a);STORE(a->audio_run,0);
    if(a->audio){furi_thread_join(a->audio);furi_thread_free(a->audio);}
    if(a->begun){subghz_devices_idle(a->radio);subghz_devices_sleep(a->radio);subghz_devices_end(a->radio);furi_hal_power_insomnia_exit();}
    subghz_devices_deinit();gui_remove_view_port(gui,a->view);furi_record_close(RECORD_GUI);
    view_port_free(a->view);furi_message_queue_free(a->events);furi_message_queue_free(a->pulses);furi_mutex_free(a->mutex);free(a);return 0;
}
