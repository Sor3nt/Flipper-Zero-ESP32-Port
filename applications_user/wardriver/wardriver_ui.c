#include "wardriver.h"
#include "wardriver_storage.h"
#include <wifi/wlan_hal.h>
#include <stdio.h>
#include <string.h>
#include <esp_timer.h>

static const char* menus[]={"All devices","Notable devices","Session stats","GPS / settings","WiGLE upload","About / limits"};
static const char* wigle_items[]={"API Name","API Token","Choose GPS CSV","Upload selected CSV","Reload from SD","Clear credentials"};
static const char* settings[]={"GPS","UART","RX GPIO","TX GPIO","Baud","Pins isolated"};
static const int pins[]={-1,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,21,38,39,40,41,42,43,44,45,46,47,48};
static const unsigned bauds[]={9600,38400,115200};
static void text(Canvas* c,unsigned x,unsigned y,const char* s,unsigned width) {
    char out[96]; snprintf(out,sizeof(out),"%s",s);
    for(char* p=out;*p;++p) if((unsigned char)*p<32) *p=' ';
    size_t n=strlen(out);
    while(n && canvas_string_width(c,out)>width) out[--n]=0;
    canvas_draw_str(c,x,y,out);
}
static void title(Canvas* c,const char* left,const char* right) {
    canvas_set_color(c,ColorBlack); canvas_draw_rbox(c,0,0,128,13,2);
    canvas_set_color(c,ColorWhite); canvas_set_font(c,FontPrimary);
    text(c,4,10,left,88); canvas_set_font(c,FontSecondary);
    canvas_draw_str_aligned(c,124,10,AlignRight,AlignBottom,right);
    canvas_set_color(c,ColorBlack);
}
static void footer(Canvas* c,const char* s) {
    canvas_draw_line(c,0,54,127,54); canvas_set_font(c,FontSecondary); text(c,2,63,s,124);
}
static void wrapped(Canvas* c,const char* value,unsigned y,unsigned lines) {
    while(*value && lines--) {
        char part[25]; size_t n=strlen(value); if(n>24) n=24;
        memcpy(part,value,n); part[n]=0;
        while(n && canvas_string_width(c,part)>122) part[--n]=0;
        if(!n) return;
        if(value[n] && n>12) {
            size_t space=n; while(space && value[space]!=' ') --space;
            if(space>8) { n=space; part[n]=0; }
        }
        text(c,3,y,part,122); value+=n; while(*value==' ') ++value; y+=10;
    }
}
static void setting_value(const WardriverConfig* g,unsigned index,char out[32]) {
    switch(index) {
    case 0: snprintf(out,32,"%s",g->gps_mode==0?"OFF":g->gps_mode==1?"AUTO":"ON"); break;
    case 1: snprintf(out,32,"UART%lu",(unsigned long)g->gps_uart); break;
    case 2: if(g->gps_rx<0) strcpy(out,"NONE"); else snprintf(out,32,"GPIO %ld",(long)g->gps_rx); break;
    case 3: if(g->gps_tx<0) strcpy(out,"NONE"); else snprintf(out,32,"GPIO %ld",(long)g->gps_tx); break;
    case 4: snprintf(out,32,"%lu",(unsigned long)g->gps_baud); break;
    default: strcpy(out,g->isolated_uart_pins?"YES":"NO"); break;
    }
}
static void detail(Canvas* c,const WardriverModel* m) {
    const WardriverNetwork* n=&m->rows[0]; char line[96],mac[18],first[20],last[20];
    title(c,n->observation.mode==WardriverWifi?"WI-FI DEVICE":"BLE DEVICE",m->detail==0?"1/4":m->detail==1?"2/4":m->detail==2?"3/4":"4/4");
    if(!m->row_count) { text(c,4,32,"No observations yet",120); footer(c,"BACK list"); return; }
    if(m->detail==0) {
        text(c,3,23,*n->observation.ssid?n->observation.ssid:"<no name>",122);
        wardriver_mac_text(n->observation.mac,mac); text(c,3,33,mac,122);
        if(n->observation.mode==WardriverWifi) snprintf(line,sizeof(line),"CH %u  %d dBm  x%lu",n->observation.channel,n->observation.rssi,(unsigned long)n->detections);
        else snprintf(line,sizeof(line),"BLE  %d dBm  x%lu",n->observation.rssi,(unsigned long)n->detections);
        text(c,3,43,line,122); text(c,3,52,n->observation.mode==WardriverWifi?wardriver_security(n->observation.auth):"BLE advertisement",122);
    } else if(m->detail==1) {
        text(c,3,23,*n->vendor?n->vendor:"Vendor unknown",122);
        text(c,3,34,wardriver_hint_text(n->observation.hints),122);
        text(c,3,44,"Matches are unverified",122);
        if(n->observation.remote_id[0]) text(c,3,52,n->observation.remote_id,122);
    } else if(m->detail==2) {
        wardriver_format_time(n->first_seen,first); wardriver_format_time(n->last_seen,last);
        text(c,3,23,"First seen (UTC)",122); text(c,3,33,*first?first:"Clock unset",122);
        text(c,3,43,"Last seen (UTC)",122); text(c,3,52,*last?last:"Clock unset",122);
    } else if(n->last_fix.has_fix) {
        snprintf(line,sizeof(line),"LAT %.6f",n->last_fix.latitude); text(c,3,23,line,122);
        snprintf(line,sizeof(line),"LON %.6f",n->last_fix.longitude); text(c,3,33,line,122);
        if(n->last_fix.altitude_valid) snprintf(line,sizeof(line),"ALT %.1fm  SAT %u",n->last_fix.altitude,n->last_fix.satellites);
        else snprintf(line,sizeof(line),"ALT --  SAT %u",n->last_fix.satellites);
        text(c,3,43,line,122); wardriver_format_time(n->last_fix.timestamp_valid?n->last_fix.timestamp:0,last);
        text(c,3,52,*last?last:"GPS date unavailable",122);
    } else { text(c,4,30,"No location for this sighting",120); text(c,4,44,"Survey works without GPS",120); }
    footer(c,m->detail==3?"Last GPS fix   OK next   BACK":"OK next page    BACK list");
}
static void counter(Canvas* c,unsigned x,unsigned width,const char* label,size_t count,bool highlight) {
    if(highlight) { canvas_draw_rbox(c,x,14,width,22,3); canvas_set_color(c,ColorWhite); }
    else canvas_draw_rframe(c,x,14,width,22,3);
    canvas_set_font(c,FontSecondary); text(c,x+4,24,label,width-8);
    char number[12]; snprintf(number,sizeof(number),"%u",(unsigned)count);
    canvas_set_font(c,FontPrimary);
    canvas_draw_str_aligned(c,x+width-4,34,AlignRight,AlignBottom,number);
    canvas_set_color(c,ColorBlack); canvas_set_font(c,FontSecondary);
}
void wardriver_ui_draw(Canvas* c,void* context) {
    WardriverApp* a=context;
    /* Snapshot only. No radio, SD, UART or waiting for worker shutdown here. */
    furi_mutex_acquire(a->lock,FuriWaitForever); WardriverModel m=a->model; furi_mutex_release(a->lock);
    if(m.radio_transition) snprintf(m.status,sizeof(m.status),"%s",wlan_hal_survey_status());
    canvas_clear(c); canvas_set_font(c,FontSecondary);
    char line[96];
    if(__atomic_load_n(&a->quit,__ATOMIC_ACQUIRE)) {
        title(c,"WRAPPING UP","EXIT");
        canvas_draw_rframe(c,2,17,124,26,3);
        text(c,6,29,m.status,116);
        uint32_t seconds=((uint32_t)(esp_timer_get_time()/1000)-m.phase_started)/1000;
        snprintf(line,sizeof(line),"Elapsed: %lus",(unsigned long)seconds); text(c,6,39,line,116);
        text(c,4,52,seconds>=15?"Waiting on storage / radio":"Saving and releasing devices",120);
        footer(c,"Keep power on while saving"); return;
    }
    if(m.page==WardPageHome) {
        title(c,"WARDRIVER",m.transitioning||!m.ready?"WAIT":m.active?(m.log_ok?"LIVE":"SD!"):"READY");
        counter(c,0,38,"WI-FI",m.wifi_count,false);
        counter(c,41,34,"BLE",m.ble_count,false);
        counter(c,78,50,"NOTABLE",m.notable_count,m.notable_count>0);
        char satellites[8]="--";
        if(m.gps.satellites_valid) snprintf(satellites,sizeof(satellites),"%u",m.gps.satellites);
        snprintf(line,sizeof(line),"GPS %s   SAT %s   SD %s",m.gps_status,satellites,m.active?(m.log_ok?"OK":"ERR"):"--");
        text(c,2,44,line,124);
        if(m.transitioning || !m.ready || (m.active && strcmp(m.status,"LISTENING"))) text(c,2,53,m.status,124);
        else if(m.notice[0]) text(c,2,53,m.notice,124);
        else if(m.last.used) {
            snprintf(line,sizeof(line),"%s  %s",m.last.observation.mode==WardriverWifi?"W":"B",
                *m.last.observation.ssid?m.last.observation.ssid:"<unnamed>"); text(c,2,53,line,124);
        } else text(c,2,53,m.active?"Listening for Wi-Fi + BLE":"One session. Both radios.",124);
        footer(c,m.transitioning||!m.ready?"Please wait   BACK exit":m.active?"OK stop   UP menu   DN list":"OK start   UP menu   DN list");
    } else if(m.page==WardPageMenu) {
        title(c,"WARDRIVER","MENU");
        unsigned top=m.menu>2?m.menu-2:0;
        for(unsigned i=0;i<3 && top+i<6;++i) {
            unsigned item=top+i,y=15+13*i;
            if(item==m.menu) { canvas_draw_rbox(c,1,y,126,12,2); canvas_set_color(c,ColorWhite); }
            text(c,5,y+9,menus[item],118); canvas_set_color(c,ColorBlack);
        }
        footer(c,"Turn select   OK open   BACK");
    } else if(m.page==WardPageList) {
        snprintf(line,sizeof(line),"%u/%u",m.list_count?(unsigned)m.selected+1:0,(unsigned)m.list_count); title(c,m.notable_only?"NOTABLE":"ALL DEVICES",line);
        for(unsigned i=0;i<m.row_count;++i) {
            unsigned y=15+13*i;
            if(!i) { canvas_draw_rbox(c,1,y,126,12,2); canvas_set_color(c,ColorWhite); }
            snprintf(line,sizeof(line),"%c%s %s",m.rows[i].observation.mode==WardriverWifi?'W':'B',
                m.rows[i].observation.hints & WARDRIVER_NOTABLE_HINTS?"*":"",
                *m.rows[i].observation.ssid?m.rows[i].observation.ssid:"<unnamed>");
            text(c,4,y+9,line,92);
            snprintf(line,sizeof(line),"%d",m.rows[i].observation.rssi); canvas_draw_str_aligned(c,123,y+9,AlignRight,AlignBottom,line);
            canvas_set_color(c,ColorBlack);
        }
        if(!m.list_count) text(c,5,34,m.notable_only?"No notable hints recorded":"Start a survey to discover",118);
        footer(c,m.notable_only?"Hints only   OK view   BACK":"W Wi-Fi / B BLE   OK view");
    } else if(m.page==WardPageDetail) detail(c,&m);
    else if(m.page==WardPageSettings) {
        title(c,"GPS SETTINGS",m.editing?"EDIT":"SELECT");
        unsigned top=m.setting>2?m.setting-2:0;
        for(unsigned i=0;i<3 && top+i<6;++i) {
            unsigned item=top+i,y=15+13*i; char value[32]; setting_value(&m.config,item,value);
            if(item==m.setting) { canvas_draw_rbox(c,1,y,126,12,2); canvas_set_color(c,ColorWhite); }
            text(c,4,y+9,settings[item],64); canvas_draw_str_aligned(c,123,y+9,AlignRight,AlignBottom,value);
            canvas_set_color(c,ColorBlack);
        }
        footer(c,m.editing?"Turn change   OK done":"OK edit   BACK save");
    } else if(m.page==WardPageWigle) {
        title(c,"WIGLE",m.wifi_online?"ONLINE":"OFFLINE");
        unsigned top=m.wigle_menu>2?m.wigle_menu-2:0;
        for(unsigned i=0;i<3 && top+i<6;++i) {
            unsigned item=top+i,y=15+13*i;
            if(item==m.wigle_menu) { canvas_draw_rbox(c,1,y,126,12,2); canvas_set_color(c,ColorWhite); }
            text(c,4,y+9,wigle_items[item],item<3?91:118);
            if(item<3) {
                bool set=item==0?m.wigle_name_set:item==1?m.wigle_token_set:m.wigle_path[0]!=0;
                canvas_draw_str_aligned(c,123,y+9,AlignRight,AlignBottom,set?"SET":"--");
            }
            canvas_set_color(c,ColorBlack);
        }
        footer(c,m.wigle_menu<2?"Credentials saved on SD":"OK select    BACK menu");
    } else if(m.page==WardPageUploadConfirm) {
        title(c,"UPLOAD TO","WIGLE");
        const char* name=strrchr(m.wigle_path,'/');
        wrapped(c,name?name+1:m.wigle_path,23,2);
        text(c,3,44,"Share networks + GPS",122);
        footer(c,"OK upload    BACK cancel");
    } else if(m.page==WardPageUploadStatus) {
        char percent[12]; snprintf(percent,sizeof(percent),"%u%%",m.upload_percent);
        title(c,"WIGLE",m.wigle_uploading?percent:m.wigle_busy?"WAIT":"RESULT");
        wrapped(c,m.wigle_status,24,3);
        footer(c,m.wigle_uploading?"BACK cancel upload":m.wigle_busy?"Saving settings...":"BACK WiGLE menu");
    } else if(m.page==WardPageStats) {
        title(c,"SESSION","DUAL");
        snprintf(line,sizeof(line),"Wi-Fi %s   BLE %s",m.wifi_status,m.ble_status); text(c,3,23,line,122);
        snprintf(line,sizeof(line),"Detections  %llu",(unsigned long long)m.detections); text(c,3,33,line,122);
        snprintf(line,sizeof(line),"Stored  %u / %u",(unsigned)m.count,(unsigned)m.capacity); text(c,3,43,line,122);
        snprintf(line,sizeof(line),"Missed  %lu",(unsigned long)(m.overflow+m.dropped)); text(c,3,52,line,122);
        footer(c,"BACK menu");
    } else {
        title(c,"PASSIVE ONLY","1.3.2");
        text(c,3,23,"Wi-Fi 2.4 GHz / BLE / RID",122);
        text(c,3,33,"Hints do not prove identity",122);
        text(c,3,43,"No cellular / GSM receiver",122);
        const char* oui=m.oui_state==WardOuiNotStarted?"OUI: start scan to load":
            m.oui_state==WardOuiLoading?(m.active?"OUI: loading database":"OUI: paused; start scan"):
            m.oui_state==WardOuiReady?"OUI: database loaded":"OUI: check SD vendor file";
        text(c,3,52,oui,122);
        footer(c,"BACK menu");
    }
}
void wardriver_ui_input(InputEvent* e,void* context) {
    WardriverApp* a=context; furi_message_queue_put(a->inputs,e,0);
}
static void change_setting(WardriverConfig* g,unsigned item,int delta) {
    if(item==0) g->gps_mode=(g->gps_mode+3+delta)%3;
    else if(item==1) g->gps_uart=g->gps_uart==1?2:1;
    else if(item==2 || item==3) {
        int32_t* field=item==2?&g->gps_rx:&g->gps_tx;
        unsigned i=0; while(i<sizeof(pins)/sizeof(*pins) && pins[i]!=*field) ++i;
        i=(i+sizeof(pins)/sizeof(*pins)+delta)%(sizeof(pins)/sizeof(*pins)); *field=pins[i];
    } else if(item==4) {
        unsigned i=0; while(i<3 && bauds[i]!=g->gps_baud) ++i;
        g->gps_baud=bauds[(i+3+delta)%3];
    } else g->isolated_uart_pins=!g->isolated_uart_pins;
}
void wardriver_ui_handle(WardriverApp* a,const InputEvent* e) {
    if(e->type!=InputTypeShort && e->type!=InputTypeRepeat) return;
    if(__atomic_load_n(&a->quit,__ATOMIC_ACQUIRE)) return;
    furi_mutex_acquire(a->lock,FuriWaitForever);
    WardriverModel* m=&a->model;
    bool busy=m->wigle_busy || !m->ready || m->transitioning || m->active || __atomic_load_n(&a->want_running,__ATOMIC_ACQUIRE);
    int delta=e->key==InputKeyUp?-1:e->key==InputKeyDown?1:0;
    if(m->wigle_busy) {
        if(e->key==InputKeyBack && m->wigle_uploading) {
            __atomic_store_n(&a->cancel_upload,1,__ATOMIC_RELEASE);
            strcpy(m->wigle_status,"Cancelling upload...");
        }
        furi_mutex_release(a->lock); view_port_update(a->view); return;
    }
    if(e->key==InputKeyBack) {
        if(m->page==WardPageHome) __atomic_store_n(&a->quit,1,__ATOMIC_RELEASE);
        else if(m->page==WardPageDetail) m->page=WardPageList;
        else if(m->page==WardPageSettings) {
            __atomic_store_n(&a->save_config,1,__ATOMIC_RELEASE);
            if(m->editing) m->editing=false; else m->page=WardPageMenu;
        } else if(m->page==WardPageUploadConfirm || m->page==WardPageUploadStatus) m->page=WardPageWigle;
        else m->page=m->page==WardPageAbout || m->page==WardPageStats || m->page==WardPageWigle?WardPageMenu:WardPageHome;
    } else if(m->page==WardPageHome) {
        if(e->key==InputKeyOk && e->type==InputTypeShort && m->ready && !m->transitioning) {
            uint32_t want=__atomic_load_n(&a->want_running,__ATOMIC_ACQUIRE);
            m->transitioning=true;
            m->notice[0]=0;
            strcpy(m->status,want?"STOP REQUESTED":"START REQUESTED");
            __atomic_store_n(&a->want_running,!want,__ATOMIC_RELEASE);
        } else if(delta<0) m->page=WardPageMenu;
        else if(delta>0) { m->notable_only=false; m->selected=0; m->row_count=0; m->list_count=m->count; m->page=WardPageList; }
    } else if(m->page==WardPageMenu) {
        if(delta) m->menu=(m->menu+6+delta)%6;
        else if(e->key==InputKeyOk) {
            if(m->menu<2) {
                m->notable_only=m->menu==1; m->selected=0; m->row_count=0;
                m->list_count=m->notable_only?m->notable_count:m->count; m->page=WardPageList;
            } else if(m->menu==2) m->page=WardPageStats;
            else if(m->menu==3) {
                if(!busy) m->page=WardPageSettings;
                else { strcpy(m->notice,"Stop survey for settings"); m->page=WardPageHome; }
            } else if(m->menu==4) {
                if(!busy) m->page=WardPageWigle;
                else { strcpy(m->notice,"Stop survey for uploads"); m->page=WardPageHome; }
            } else m->page=WardPageAbout;
        }
    } else if(m->page==WardPageWigle) {
        if(delta) m->wigle_menu=(m->wigle_menu+6+delta)%6;
        else if(e->key==InputKeyOk && e->type==InputTypeShort) {
            if(m->wigle_menu<3) {
                m->wigle_busy=true;
                __atomic_store_n(&a->wigle_action,m->wigle_menu+1,__ATOMIC_RELEASE);
            } else if(m->wigle_menu==3) {
                if(!m->wigle_name_set || !m->wigle_token_set || !m->wigle_path[0]) {
                    strcpy(m->wigle_status,"Set credentials and select GPS CSV"); m->page=WardPageUploadStatus;
                } else m->page=WardPageUploadConfirm;
            } else {
                m->wigle_busy=true; m->page=WardPageUploadStatus;
                strcpy(m->wigle_status,m->wigle_menu==4?"Reading credentials from SD":"Clearing credentials");
                __atomic_store_n(&a->wigle_command,m->wigle_menu==4?WardWigleReload:WardWigleClear,__ATOMIC_RELEASE);
            }
        }
    } else if(m->page==WardPageUploadConfirm && e->key==InputKeyOk && e->type==InputTypeShort) {
        m->wigle_busy=true; m->wigle_uploading=true; m->upload_percent=0;
        m->page=WardPageUploadStatus; strcpy(m->wigle_status,"Preparing upload");
        __atomic_store_n(&a->cancel_upload,0,__ATOMIC_RELEASE);
        __atomic_store_n(&a->wigle_command,WardWigleSend,__ATOMIC_RELEASE);
    } else if(m->page==WardPageList) {
        if(delta<0 && m->selected) --m->selected;
        if(delta>0 && m->selected+1<m->list_count) ++m->selected;
        if(e->key==InputKeyOk && m->row_count) { m->page=WardPageDetail; m->detail=0; }
    } else if(m->page==WardPageDetail && e->key==InputKeyOk) m->detail=(m->detail+1)%4;
    else if(m->page==WardPageSettings) {
        if(delta) { if(m->editing) change_setting(&m->config,m->setting,delta); else m->setting=(m->setting+6+delta)%6; }
        else if(e->key==InputKeyOk) { m->editing=!m->editing; if(!m->editing) __atomic_store_n(&a->save_config,1,__ATOMIC_RELEASE); }
    }
    furi_mutex_release(a->lock); view_port_update(a->view);
}
