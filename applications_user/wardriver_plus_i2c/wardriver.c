#include "wardriver.h"
#include "wardriver_wifi.h"
#include "wardriver_gps.h"
#include "wardriver_storage.h"
#include "wardriver_oui.h"
#include <wifi/wlan_hal.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <esp_random.h>
#include <furi_hal_rtc.h>
#include <dialogs/dialogs.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time()/1000); }
static void set_status(WardriverApp* a,const char* text) {
    furi_mutex_acquire(a->lock,FuriWaitForever);
    snprintf(a->model.status,sizeof(a->model.status),"%s",text);
    a->model.phase_started=now_ms();
    furi_mutex_release(a->lock); view_port_update(a->view);
}
static void progress(void* context,const char* text) {
    WardriverApp* a=context;
    furi_mutex_acquire(a->lock,FuriWaitForever); a->model.radio_transition=false; furi_mutex_release(a->lock);
    set_status(a,text);
}
static void radio_transition(WardriverApp* a,bool active) {
    furi_mutex_acquire(a->lock,FuriWaitForever);
    a->model.radio_transition=active;
    furi_mutex_release(a->lock);
    view_port_update(a->view);
}
static void log_dirty(WardriverTable* table,WardriverLog* log,size_t* cursor,size_t budget) {
    size_t examined=0;
    while(examined<table->capacity && budget) {
        WardriverNetwork* n=&table->entries[*cursor];
        *cursor=(*cursor+1)%table->capacity; ++examined;
        if(n->used && n->dirty) {
            if(wardriver_log_ok(log)) wardriver_log_record(log,n);
            n->dirty=false; --budget;
        }
    }
}
static void consume(WardriverApp* a,WardriverTable* table,WardriverGps* gps,
                    WardriverOui* oui,WardriverNetwork** last) {
    WardriverObservation observation;
    for(unsigned i=0;i<192 && furi_message_queue_get(a->observations,&observation,0)==FuriStatusOk;++i) {
        uint32_t now=now_ms(),age=now-observation.seen_ms;
        WardriverGpsData position=wardriver_nmea_snapshot(&gps->parser,now);
        /* A backed-up queue must not assign today's position to an older
         * radio observation after a slow/removable SD card stalls logging. */
        if(age>1000) { position.has_fix=false; position.timestamp_valid=false; }
        uint64_t stamp=position.timestamp_valid?position.timestamp:furi_hal_rtc_get_timestamp();
        if(stamp>age/1000) stamp-=age/1000;
        bool is_new;
        WardriverNetwork* n=wardriver_table_observe(table,&observation,stamp,&position,&is_new);
        if(n) {
            if(is_new) {
                if(observation.hints & WARDRIVER_NOTABLE_HINTS) {
                    furi_mutex_acquire(a->lock,FuriWaitForever);
                    if(wardriver_filter_match(&observation,a->model.filter,a->model.min_confidence)) {
                        snprintf(a->model.alert,sizeof(a->model.alert),"NEW: %s",wardriver_hint_text(observation.primary_hint?observation.primary_hint:observation.hints));
                        a->model.alert_until=now+5000;
                    }
                    furi_mutex_release(a->lock);
                }
                if(observation.random_address) strcpy(n->vendor,"Random BLE address");
                else wardriver_oui_lookup(oui,observation.mac,n->vendor);
            }
            *last=n;
        }
    }
}
static void publish(WardriverApp* a,WardriverTable* t,WardriverGps* gps,
                    WardriverRadio* radio,WardriverLog* log,WardriverNetwork* last,bool running) {
    furi_mutex_acquire(a->lock,FuriWaitForever);
    size_t selected=a->model.selected; bool notable_only=a->model.notable_only;
    unsigned filter=a->model.filter; WardriverConfidence minimum=a->model.min_confidence;
    bool detail=a->model.page==WardPageDetail;
    WardriverObservation selected_device=a->model.rows[0].observation;
    furi_mutex_release(a->lock);
    size_t list_count=t->count;
    if(notable_only) {
        list_count=0;
        for(size_t i=0;i<t->capacity;++i) if(t->entries[i].used && wardriver_filter_match(&t->entries[i].observation,filter,minimum)) ++list_count;
    }
    if(selected>=list_count) selected=list_count?list_count-1:0;
    WardriverNetwork rows[3]={0}; unsigned row_count=0; size_t index=0;
    for(size_t i=0;i<t->capacity && row_count<3;++i) {
        if(!t->entries[i].used) continue;
        if(notable_only && !wardriver_filter_match(&t->entries[i].observation,filter,minimum)) continue;
        if(index++>=selected) rows[row_count++]=t->entries[i];
    }
    bool detail_found=false;
    WardriverNetwork current_detail={0};
    if(detail) for(size_t i=0;i<t->capacity;++i) {
        const WardriverNetwork* n=&t->entries[i];
        if(n->used && n->observation.mode==selected_device.mode && n->observation.random_address==selected_device.random_address && !memcmp(n->observation.mac,selected_device.mac,6)) {
            current_detail=*n; detail_found=true; break;
        }
    }
    WardriverGpsData data=wardriver_nmea_snapshot(&gps->parser,now_ms());
    furi_mutex_acquire(a->lock,FuriWaitForever);
    WardriverModel* m=&a->model;
    m->count=t->count; m->capacity=t->capacity; m->detections=t->detections;
    m->wifi_count=t->wifi_count; m->ble_count=t->ble_count; m->notable_count=t->notable_count;
    m->overflow=t->overflow; m->dropped=wardriver_radio_dropped(radio);
    /* Keep the selected detail snapshot stable as new hash-table entries arrive. */
    if(m->page!=WardPageDetail && m->notable_only==notable_only && m->filter==filter && m->min_confidence==minimum) {
        m->selected=selected; m->list_count=list_count; m->row_count=row_count; memcpy(m->rows,rows,sizeof(rows));
    }
    /* Update live RSSI/RID by identity, never by the moving list index. */
    if(detail_found && m->page==WardPageDetail && m->rows[0].observation.mode==selected_device.mode &&
       m->rows[0].observation.random_address==selected_device.random_address && !memcmp(m->rows[0].observation.mac,selected_device.mac,6)) m->rows[0]=current_detail;
    if(last) m->last=*last;
    m->gps=data; m->active=running; m->log_ok=wardriver_log_ok(log);
    m->log_started=wardriver_log_has_records(log);
    m->wifi_online=wlan_hal_is_connected();
    snprintf(m->wifi_status,sizeof(m->wifi_status),"%s",wardriver_radio_wifi_status(radio));
    snprintf(m->ble_status,sizeof(m->ble_status),"%s",wardriver_radio_ble_status(radio));
    bool gps_pin_blocked=!strcmp(gps->status,"PIN IN USE");
    snprintf(m->gps_status,sizeof(m->gps_status),"%s",gps_pin_blocked?gps->status:
             !data.enabled?"OFF":!data.connected?"WAITING":data.has_fix?"FIX":"NO FIX");
    if(running) {
        snprintf(m->status,sizeof(m->status),"%s",wardriver_radio_status(radio));
        if((data.enabled || gps_pin_blocked) && !gps->installed) snprintf(m->notice,sizeof(m->notice),"GPS: %s",gps->status);
        else if(!m->log_ok) strcpy(m->notice,"SD ERROR - NOT SAVING");
        else if(!data.has_fix || !data.timestamp_valid) strcpy(m->notice,"GPS FIX NEEDED - NOT SAVING");
        else if(!m->log_started) strcpy(m->notice,"WAITING FOR GPS WI-FI DATA");
        else m->notice[0]=0;
    }
    furi_mutex_release(a->lock); view_port_update(a->view);
}
static void wigle_progress(void* context,const char* stage,unsigned percent) {
    WardriverApp* a=context;
    furi_mutex_acquire(a->lock,FuriWaitForever);
    snprintf(a->model.wigle_status,sizeof(a->model.wigle_status),"%s",stage);
    a->model.upload_percent=percent;
    furi_mutex_release(a->lock); view_port_update(a->view);
}
static bool wigle_cancelled(void* context) {
    WardriverApp* a=context;
    return __atomic_load_n(&a->cancel_upload,__ATOMIC_ACQUIRE) || __atomic_load_n(&a->quit,__ATOMIC_ACQUIRE);
}
static void wigle_process(WardriverApp* a,WardWigleCommand command) {
    WardWigleUpload job={.cancelled=wigle_cancelled,.progress=wigle_progress,.context=a};
    furi_mutex_acquire(a->lock,FuriWaitForever);
    bool wdg=command==WardWdgSave;
    bool both=command==WardWigleSend;
    job.service=wdg?WardUploadWdgWars:WardUploadWigle;
    job.credentials=wdg?a->wdg_credentials:a->wigle_credentials;
    snprintf(job.path,sizeof(job.path),"%s",a->model.wigle_path);
    furi_mutex_release(a->lock);
    bool update=false;
    WardWigleCredentials loaded_wdg={0};
    if(both) {
        WardWigleUpload second={.cancelled=wigle_cancelled,.progress=wigle_progress,.context=a};
        furi_mutex_acquire(a->lock,FuriWaitForever);
        job.credentials=a->wigle_credentials; second.credentials=a->wdg_credentials;
        furi_mutex_release(a->lock);
        snprintf(second.path,sizeof(second.path),"%s",job.path);
        wardriver_upload_both(&job,&second);
        furi_mutex_acquire(a->lock,FuriWaitForever);
        snprintf(a->model.upload_results[0],48,"%s",job.result);
        snprintf(a->model.upload_results[1],48,"%s",second.result);
        a->model.batch_result=true; a->model.upload_result_service=0;
        furi_mutex_release(a->lock);
        wardriver_wigle_erase(&second,sizeof(second));
    } else if(command==WardWigleSave || command==WardWdgSave) {
        bool ok=wdg?wardriver_wdg_credentials_save(&job.credentials):wardriver_wigle_credentials_save(&job.credentials);
        strcpy(job.result,ok?"Credentials saved on SD":"SD save failed; settings in RAM");
    } else if(command==WardWigleReload) {
        bool wigle_ok=wardriver_wigle_credentials_load(&job.credentials);
        bool wdg_ok=wardriver_wdg_credentials_load(&loaded_wdg); update=true;
        strcpy(job.result,wigle_ok && wdg_ok?"Both credentials loaded from SD":
            wigle_ok?"WiGLE loaded; set WDGWars key":wdg_ok?"WDGWars loaded; set WiGLE API":"Keys missing or invalid on SD");
    } else if(command==WardWigleClear) {
        wardriver_wigle_erase(&job.credentials,sizeof(job.credentials)); update=true;
        bool wigle_ok=wardriver_wigle_credentials_clear();
        bool wdg_ok=wardriver_wdg_credentials_clear();
        strcpy(job.result,wigle_ok && wdg_ok?"Both credentials cleared":"RAM cleared; SD delete failed");
    }
    furi_mutex_acquire(a->lock,FuriWaitForever);
    if(update) { a->wigle_credentials=job.credentials; a->wdg_credentials=loaded_wdg; }
    a->model.wdg_key_set=wardriver_wdg_key_valid(a->wdg_credentials.token);
    a->model.wigle_name_set=a->wigle_credentials.name[0]!=0;
    a->model.wigle_token_set=a->wigle_credentials.token[0]!=0;
    a->model.wigle_busy=false; a->model.wigle_uploading=false;
    snprintf(a->model.wigle_status,sizeof(a->model.wigle_status),"%s",job.result);
    furi_mutex_release(a->lock);
    wardriver_wigle_erase(&job,sizeof(job));
    wardriver_wigle_erase(&loaded_wdg,sizeof(loaded_wdg)); view_port_update(a->view);
}
static int32_t wardriver_worker(void* context) {
    WardriverApp* a=context;
    WardriverTable table={0}; WardriverNetwork* last=NULL;
    WardriverGps gps={0}; WardriverLog* log=NULL;
    set_status(a,"READING SETTINGS");
    WardriverConfig loaded;
    wardriver_config_load(&loaded);
    furi_mutex_acquire(a->lock,FuriWaitForever); a->model.config=loaded; furi_mutex_release(a->lock);
    wigle_process(a,WardWigleReload);
    WardriverRadio* radio=wardriver_radio_alloc(a->observations,progress,a);
    for(size_t n=4096;n>=256;n/=2) {
        table.entries=heap_caps_calloc(n,sizeof(WardriverNetwork),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if(table.entries) { table.capacity=n; break; }
    }
    if(!table.entries) { table.capacity=128; table.entries=calloc(table.capacity,sizeof(WardriverNetwork)); }
    if(!radio || !table.entries) {
        set_status(a,"LOW MEMORY - BACK TO EXIT");
        free(table.entries); wardriver_radio_free(radio);
        while(!__atomic_load_n(&a->quit,__ATOMIC_ACQUIRE)) furi_delay_ms(100);
        __atomic_store_n(&a->finished,1,__ATOMIC_RELEASE); return 0;
    }
    WardriverOui* oui=NULL; bool oui_opened=false;
    size_t vendor_cursor=0;
    set_status(a,"READY");
    furi_mutex_acquire(a->lock,FuriWaitForever); a->model.ready=true; furi_mutex_release(a->lock);
    bool running=false; size_t cursor=0;
    uint32_t flush_at=now_ms(),publish_at=0;
    while(!__atomic_load_n(&a->quit,__ATOMIC_ACQUIRE)) {
        uint32_t now=now_ms();
        if(__atomic_exchange_n(&a->save_config,0,__ATOMIC_ACQ_REL)) {
            furi_mutex_acquire(a->lock,FuriWaitForever); WardriverConfig config=a->model.config; furi_mutex_release(a->lock);
            bool ok=wardriver_config_save(&config);
            furi_mutex_acquire(a->lock,FuriWaitForever);
            strcpy(a->model.notice,ok?"SETTINGS SAVED":"SETTINGS: SD ERROR");
            furi_mutex_release(a->lock);
        }
        WardWigleCommand command=__atomic_exchange_n(&a->wigle_command,WardWigleNone,__ATOMIC_ACQ_REL);
        if(command!=WardWigleNone && !running) wigle_process(a,command);
        bool want=__atomic_load_n(&a->want_running,__ATOMIC_ACQUIRE)!=0;
        if(want && !running) {
            set_status(a,"STARTING GPS");
            furi_mutex_acquire(a->lock,FuriWaitForever);
            WardriverConfig config=a->model.config;
            a->model.selected=0; a->model.notice[0]=0; a->model.alert[0]=0; memset(&a->model.last,0,sizeof(a->model.last));
            furi_mutex_release(a->lock);
            memset(table.entries,0,table.capacity*sizeof(*table.entries));
            table.count=table.wifi_count=table.ble_count=table.notable_count=0;
            table.detections=0; table.overflow=0; cursor=0; vendor_cursor=0; last=NULL;
            WardriverObservation discard;
            while(furi_message_queue_get(a->observations,&discard,0)==FuriStatusOk) {}
            wardriver_gps_start(&gps,&config);
            set_status(a,"ACQUIRING RADIO");
            radio_transition(a,true);
            running=wardriver_radio_start(radio);
            radio_transition(a,false);
            if(running && !__atomic_load_n(&a->quit,__ATOMIC_ACQUIRE)) {
                /* Begin the nonblocking scan before any optional vendor I/O. */
                wardriver_radio_poll(radio,now_ms());
                set_status(a,"WAITING FOR GPS FIX");
                log=wardriver_log_open(furi_hal_rtc_get_timestamp(),esp_random()); flush_at=now_ms();
            }
            else {
                if(!running) wardriver_gps_stop(&gps);
                __atomic_store_n(&a->want_running,0,__ATOMIC_RELEASE);
                set_status(a,wardriver_radio_status(radio));
            }
            furi_mutex_acquire(a->lock,FuriWaitForever);
            a->model.active=running; a->model.transitioning=false;
            furi_mutex_release(a->lock);
        } else if(!want && running) {
            wardriver_radio_quiesce(radio);
            consume(a,&table,&gps,oui,&last); wardriver_gps_stop(&gps);
            set_status(a,"SAVING LAST OBSERVATIONS");
            log_dirty(&table,log,&cursor,table.capacity);
            bool recorded=wardriver_log_has_records(log);
            bool saved=wardriver_log_close(log,progress,a); log=NULL;
            set_status(a,"RESTORING RADIO"); radio_transition(a,true);
            wardriver_radio_stop(radio); radio_transition(a,false);
            bool restored=strcmp(wardriver_radio_status(radio),"RADIO RESTORE FAILED")!=0;
            running=false;
            const char* stopped=!restored?"RADIO RESTORE FAILED":!saved?"STOPPED - SD ERROR":
                recorded?"SESSION SAVED":"NO GPS DATA - NOT SAVED";
            set_status(a,stopped);
            furi_mutex_acquire(a->lock,FuriWaitForever);
            strcpy(a->model.notice,stopped);
            a->model.active=false; a->model.transitioning=false;
            furi_mutex_release(a->lock);
        }
        if(__atomic_load_n(&a->quit,__ATOMIC_ACQUIRE)) break;
        if(running) {
            now=now_ms();
            wardriver_gps_poll(&gps,now);
            wardriver_radio_poll(radio,now);
            consume(a,&table,&gps,oui,&last);
            log_dirty(&table,log,&cursor,8);
            if(now-flush_at>=5000) { wardriver_log_flush(log); flush_at=now; }
            if(!oui_opened) {
                set_status(a,"OPENING VENDOR INDEX");
                furi_mutex_acquire(a->lock,FuriWaitForever); a->model.oui_state=WardOuiLoading; furi_mutex_release(a->lock);
                oui=wardriver_oui_open(); oui_opened=true;
                if(!oui) {
                    furi_mutex_acquire(a->lock,FuriWaitForever); a->model.oui_state=WardOuiUnavailable; furi_mutex_release(a->lock);
                }
            }
            if(oui) {
                if(wardriver_oui_step(oui)) {
                    furi_mutex_acquire(a->lock,FuriWaitForever);
                    a->model.oui_state=wardriver_oui_ready(oui)?WardOuiReady:WardOuiUnavailable;
                    furi_mutex_release(a->lock);
                }
                if(wardriver_oui_ready(oui)) {
                    /* Backfill devices discovered before the index was ready. */
                    for(unsigned i=0;i<16 && vendor_cursor<table.capacity;++i,++vendor_cursor) {
                        WardriverNetwork* n=&table.entries[vendor_cursor];
                        if(n->used && !n->vendor[0] && !n->observation.random_address) {
                            wardriver_oui_lookup(oui,n->observation.mac,n->vendor); n->dirty=true;
                        }
                    }

                }
            }
        }
        if(now-publish_at>=150) { publish(a,&table,&gps,radio,log,last,running); publish_at=now; }
        furi_delay_ms(30);
    }
    wardriver_radio_quiesce(radio);
    set_status(a,"SAVING LAST OBSERVATIONS");
    consume(a,&table,&gps,oui,&last); wardriver_gps_stop(&gps);
    log_dirty(&table,log,&cursor,table.capacity); wardriver_log_close(log,progress,a);
    set_status(a,"CLOSING VENDOR INDEX"); wardriver_oui_free(oui); free(table.entries);
    if(__atomic_exchange_n(&a->save_config,0,__ATOMIC_ACQ_REL)) {
        furi_mutex_acquire(a->lock,FuriWaitForever); WardriverConfig config=a->model.config; furi_mutex_release(a->lock);
        set_status(a,"SAVING SETTINGS"); wardriver_config_save(&config);
    }
    set_status(a,"RESTORING RADIO"); radio_transition(a,true);
    wardriver_radio_stop(radio); radio_transition(a,false); wardriver_radio_free(radio);
    set_status(a,"WORKER FINISHED");
    __atomic_store_n(&a->finished,1,__ATOMIC_RELEASE);
    return 0;
}
static void startup_error(const char* stage) {
    size_t available=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    size_t largest=heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    FURI_LOG_E("Wardriver","Startup: %s; internal free=%u largest=%u",
               stage,(unsigned)available,(unsigned)largest);
    DialogMessage* message=dialog_message_alloc();
    if(!message) return; /* If even the error message cannot allocate, USB has the reason. */
    char text[96];
    snprintf(text,sizeof(text),"%s\nFree %u B\nLargest %u B",stage,(unsigned)available,(unsigned)largest);
    /* dialog_message_alloc does not initialize its fields in this port. */
    dialog_message_set_header(message,"Wardriver 1.5.2",64,1,AlignCenter,AlignTop);
    dialog_message_set_text(message,text,2,18,AlignLeft,AlignTop);
    dialog_message_set_icon(message,NULL,0,0);
    dialog_message_set_buttons(message,NULL,"Back",NULL);
    DialogsApp* dialogs=furi_record_open(RECORD_DIALOGS);
    dialog_message_show(dialogs,message);
    furi_record_close(RECORD_DIALOGS);
    dialog_message_free(message);
}

int32_t wardriver_app(void* context) {
    (void)context;
    /* Total free memory and the largest block are diagnostics, not an arbitrary
     * admission threshold. No startup allocation requires one 48 KiB block.
     * Keep the atomic control words in internal RAM; never move this object to PSRAM. */
    FURI_LOG_I("Wardriver","Start 1.5.2: internal free=%u largest=%u app=%u",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
               (unsigned)sizeof(WardriverApp));
    const char* failure="App memory failed";
    WardriverApp* a=heap_caps_calloc(1,sizeof(*a),MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if(!a) { startup_error(failure); return -1; }
    failure="Mutex memory failed";
    a->lock=furi_mutex_alloc(FuriMutexTypeNormal);
    if(!a->lock) goto fail;
    failure="Input queue failed";
    a->inputs=furi_message_queue_alloc(16,sizeof(InputEvent));
    if(!a->inputs) goto fail;
    failure="Scan queue failed";
    a->observations=furi_message_queue_alloc(192,sizeof(WardriverObservation));
    if(!a->observations) goto fail;
    failure="View memory failed";
    a->view=view_port_alloc();
    if(!a->view) goto fail;
    failure="Worker memory failed";
    a->worker=furi_thread_alloc_ex("Wardriver",16384,wardriver_worker,a);
    if(!a->worker) goto fail;
    a->gui=furi_record_open(RECORD_GUI);
    wardriver_config_defaults(&a->model.config); strcpy(a->model.gps_status,"OFF");
    strcpy(a->model.wifi_status,"OFF"); strcpy(a->model.ble_status,"OFF");
    strcpy(a->model.status,"PREPARING");
    view_port_draw_callback_set(a->view,wardriver_ui_draw,a);
    view_port_input_callback_set(a->view,wardriver_ui_input,a);
    gui_add_view_port(a->gui,a->view,GuiLayerFullscreen);
    furi_thread_start(a->worker);
    while(!__atomic_load_n(&a->finished,__ATOMIC_ACQUIRE)) {
        InputEvent event;
        if(furi_message_queue_get(a->inputs,&event,furi_ms_to_ticks(100))==FuriStatusOk)
            wardriver_ui_handle(a,&event);
        unsigned action=__atomic_exchange_n(&a->wigle_action,0,__ATOMIC_ACQ_REL);
        if(action) wardriver_wigle_dialog(a,action);
        view_port_update(a->view);
    }
    set_status(a,"JOINING WORKER"); furi_thread_join(a->worker); furi_thread_free(a->worker);
    gui_remove_view_port(a->gui,a->view); view_port_free(a->view); furi_record_close(RECORD_GUI);
    furi_message_queue_free(a->observations); furi_message_queue_free(a->inputs);
    furi_mutex_free(a->lock); wardriver_wigle_erase(&a->wigle_credentials,sizeof(a->wigle_credentials));
    wardriver_wigle_erase(&a->wdg_credentials,sizeof(a->wdg_credentials)); free(a); return 0;
fail:
    if(a->view) view_port_free(a->view);
    if(a->observations) furi_message_queue_free(a->observations);
    if(a->inputs) furi_message_queue_free(a->inputs);
    if(a->lock) furi_mutex_free(a->lock);
    free(a);
    startup_error(failure); /* Release partial startup allocations before the dialog. */
    return -1;
}
