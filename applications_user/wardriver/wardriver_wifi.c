#include "wardriver_wifi.h"
#include "wardriver_detect.h"
#include <wifi/wlan_hal.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_gap_ble_api.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define SCAN_BATCH 128
struct WardriverRadio {
    FuriMessageQueue* queue;
    bool leased, registered, scanning, ble_owned, halted, quiesced;
    bool ble_requested, ble_scanning, ble_halted;
    WardriverRadioProgress progress;
    void* progress_context;
    unsigned retries;
    uint32_t done, failed, dropped, callbacks, next_scan, scan_started;
    uint32_t ble_params, ble_started, ble_failed, ble_requested_at;
    wifi_ap_record_t* records;
    esp_err_t error;
    char status[32];
};
static WardriverRadio* owner;
/* FAP .bss lives in PSRAM. Keep read/modify/write atomics in the internal-RAM
 * radio object; ESP32-S3 S32C1I atomics must not target external memory. */
static uint32_t* callback_count;
static void push(WardriverRadio* r, const WardriverObservation* o) {
    WardriverObservation stamped=*o;
    stamped.seen_ms=(uint32_t)(esp_timer_get_time()/1000);
    if(furi_message_queue_put(r->queue,&stamped,0)!=FuriStatusOk)
        __atomic_fetch_add(&r->dropped,1,__ATOMIC_RELAXED);
}
static WardriverRadio* enter(void) {
    uint32_t* count=__atomic_load_n(&callback_count,__ATOMIC_ACQUIRE);
    if(count) __atomic_fetch_add(count,1,__ATOMIC_ACQUIRE);
    return __atomic_load_n(&owner,__ATOMIC_ACQUIRE);
}
static void leave(void) {
    uint32_t* count=__atomic_load_n(&callback_count,__ATOMIC_ACQUIRE);
    if(count) __atomic_fetch_sub(count,1,__ATOMIC_RELEASE);
}
static void wifi_event(void* arg,esp_event_base_t base,int32_t id,void* data) {
    (void)arg; (void)base; (void)id;
    WardriverRadio* r=enter();
    if(r) {
        wifi_event_sta_scan_done_t* done=data;
        if(done && done->status) __atomic_store_n(&r->failed,1,__ATOMIC_RELEASE);
        __atomic_store_n(&r->done,1,__ATOMIC_RELEASE);
    }
    leave();
}
static void wifi_frame(void* buffer,wifi_promiscuous_pkt_type_t type) {
    WardriverRadio* r=enter();
    if(r && type==WIFI_PKT_MGMT) {
        const wifi_promiscuous_pkt_t* packet=buffer;
        WardriverObservation o={.mode=WardriverWifi,.auth=255,
            .rssi=packet->rx_ctrl.rssi,.channel=packet->rx_ctrl.channel};
        size_t length=packet->rx_ctrl.sig_len;
        /* IDF includes four bytes of FCS in sig_len. */
        if(length>=40 && length<=2500 &&
           wardriver_detect_beacon(packet->payload,length-4,&o)) push(r,&o);
    }
    leave();
}
static void ble_event(esp_gap_ble_cb_event_t event,esp_ble_gap_cb_param_t* p) {
    WardriverRadio* r=enter();
    if(r && p) {
        if(event==ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT) {
            __atomic_store_n(&r->ble_params,p->scan_param_cmpl.status==ESP_BT_STATUS_SUCCESS?1:2,__ATOMIC_RELEASE);
        } else if(event==ESP_GAP_BLE_SCAN_START_COMPLETE_EVT) {
            __atomic_store_n(&r->ble_started,p->scan_start_cmpl.status==ESP_BT_STATUS_SUCCESS?1:2,__ATOMIC_RELEASE);
        } else if(event==ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT) {
            __atomic_store_n(&r->ble_failed,1,__ATOMIC_RELEASE);
        } else if(event==ESP_GAP_BLE_SCAN_RESULT_EVT &&
                  p->scan_rst.search_evt==ESP_GAP_SEARCH_INQ_RES_EVT) {
            WardriverObservation o={.mode=WardriverBle,.auth=255,.rssi=p->scan_rst.rssi};
            o.random_address=p->scan_rst.ble_addr_type==BLE_ADDR_TYPE_RANDOM ||
                p->scan_rst.ble_addr_type==BLE_ADDR_TYPE_RPA_RANDOM;
            memcpy(o.mac,p->scan_rst.bda,6);
            size_t length=p->scan_rst.adv_data_len;
            if(length<=sizeof(p->scan_rst.ble_adv) &&
               wardriver_detect_ble(p->scan_rst.ble_adv,length,&o)) push(r,&o);
        }
    }
    leave();
}
static void progress(WardriverRadio* r,const char* text) {
    if(r->progress) r->progress(r->progress_context,text);
}
static void setup_wifi(void* context) {
    WardriverRadio* r=context;
    r->error=esp_event_handler_register(WIFI_EVENT,WIFI_EVENT_SCAN_DONE,wifi_event,NULL);
    if(r->error!=ESP_OK) return;
    r->registered=true;
    wifi_promiscuous_filter_t filter={.filter_mask=WIFI_PROMIS_FILTER_MASK_MGMT};
    r->error=esp_wifi_set_promiscuous_filter(&filter);
    if(r->error==ESP_OK) r->error=esp_wifi_set_promiscuous_rx_cb(wifi_frame);
    if(r->error==ESP_OK) r->error=esp_wifi_set_promiscuous(true);
}
static void setup_ble(void* context) {
    WardriverRadio* r=context;
    r->error=ESP_ERR_INVALID_STATE;
    if(esp_bluedroid_get_status()!=ESP_BLUEDROID_STATUS_UNINITIALIZED ||
       esp_bt_controller_get_status()!=ESP_BT_CONTROLLER_STATUS_IDLE) return;
    esp_bt_controller_config_t config=BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    /* One passive scanner, no advertising, GATT or BLE connections. */
    config.ble_max_act=1;
    r->error=esp_bt_controller_init(&config);
    if(r->error!=ESP_OK) return;
    r->ble_owned=true;
    r->error=esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if(r->error==ESP_OK) r->error=esp_bluedroid_init();
    if(r->error==ESP_OK) r->error=esp_bluedroid_enable();
    if(r->error==ESP_OK) r->error=esp_ble_gap_register_callback(ble_event);
    esp_ble_scan_params_t params={.scan_type=BLE_SCAN_TYPE_PASSIVE,
        .own_addr_type=BLE_ADDR_TYPE_PUBLIC,.scan_filter_policy=BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval=0xA0,.scan_window=0x50,.scan_duplicate=BLE_SCAN_DUPLICATE_DISABLE};
    if(r->error==ESP_OK) r->error=esp_ble_gap_set_scan_params(&params);
}
static void scan(void* context) {
    WardriverRadio* r=context;
    wifi_scan_config_t config={.show_hidden=true,.scan_type=WIFI_SCAN_TYPE_PASSIVE,
        .scan_time={.passive=300}};
    r->error=esp_wifi_scan_start(&config,false);
}
static void scan_ble(void* context) {
    WardriverRadio* r=context;
    r->error=esp_ble_gap_start_scanning(0);
}
static void collect(void* context) {
    WardriverRadio* r=context;
    uint16_t total=0,count=SCAN_BATCH;
    r->error=esp_wifi_scan_get_ap_num(&total);
    if(r->error!=ESP_OK) { esp_wifi_clear_ap_list(); return; }
    if(total<SCAN_BATCH) count=total;
    if(total>SCAN_BATCH) __atomic_fetch_add(&r->dropped,total-SCAN_BATCH,__ATOMIC_RELAXED);
    if(!count) { esp_wifi_clear_ap_list(); return; }
    r->error=esp_wifi_scan_get_ap_records(&count,r->records);
    if(r->error!=ESP_OK) { esp_wifi_clear_ap_list(); return; }
    for(unsigned i=0;i<count;++i) {
        wifi_ap_record_t* ap=&r->records[i];
        WardriverObservation o={.mode=WardriverWifi,.channel=ap->primary,.rssi=ap->rssi,
            .auth=ap->authmode,.pairwise=ap->pairwise_cipher,.group=ap->group_cipher};
        memcpy(o.mac,ap->bssid,6); memcpy(o.ssid,ap->ssid,32); o.ssid[32]=0;
        /* Apply name heuristics without manufacturing any service signature. */
        wardriver_detect_ble(NULL,0,&o);
        push(r,&o);
    }
}
static void cancel_scan(void* context) {
    (void)context;
    esp_wifi_scan_stop();
    esp_wifi_clear_ap_list();
}
static void retry_scan(WardriverRadio* r,uint32_t now) {
    /* Recover a lost completion/error without reinitializing the shared driver.
     * Bound retries so a persistent driver fault does not become a busy loop. */
    wlan_hal_survey_run(cancel_scan,r);
    r->scanning=false;
    __atomic_store_n(&r->done,0,__ATOMIC_RELEASE);
    __atomic_store_n(&r->failed,0,__ATOMIC_RELEASE);
    r->next_scan=now+3000;
    if(++r->retries>3) {
        r->halted=true; strcpy(r->status,"SCAN FAILED - RESTART");
    } else snprintf(r->status,sizeof(r->status),"SCAN RETRY %u/3",r->retries);
}
static void stop_wifi_scan(void* context) {
    (void)context;
    esp_wifi_scan_stop();
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);
    esp_wifi_clear_ap_list();
}
static void remove_wifi_handler(void* context) {
    WardriverRadio* r=context;
    if(r->registered) esp_event_handler_unregister(WIFI_EVENT,WIFI_EVENT_SCAN_DONE,wifi_event);
    r->registered=false;
}
static void stop_ble_scan(void* context) {
    WardriverRadio* r=context;
    if(r->ble_owned && esp_bluedroid_get_status()==ESP_BLUEDROID_STATUS_ENABLED)
        esp_ble_gap_stop_scanning();
}
static void release_ble(void* context) {
    WardriverRadio* r=context;
    if(!r->ble_owned) return;
    if(esp_bluedroid_get_status()==ESP_BLUEDROID_STATUS_ENABLED) esp_bluedroid_disable();
    if(esp_bluedroid_get_status()==ESP_BLUEDROID_STATUS_INITIALIZED) esp_bluedroid_deinit();
    if(esp_bt_controller_get_status()==ESP_BT_CONTROLLER_STATUS_ENABLED) esp_bt_controller_disable();
    if(esp_bt_controller_get_status()==ESP_BT_CONTROLLER_STATUS_INITED) esp_bt_controller_deinit();
    r->ble_owned=false;
}
WardriverRadio* wardriver_radio_alloc(FuriMessageQueue* q,WardriverRadioProgress callback,void* context) {
    WardriverRadio* r=heap_caps_calloc(1,sizeof(*r),MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if(r) { r->queue=q; r->progress=callback; r->progress_context=context; strcpy(r->status,"READY"); }
    return r;
}
bool wardriver_radio_start(WardriverRadio* r) {
    r->done=r->failed=r->dropped=0; r->scanning=false; r->halted=false; r->retries=0;
    r->ble_params=r->ble_started=r->ble_failed=0;
    r->ble_requested=r->ble_scanning=r->ble_halted=false; r->quiesced=false;
    /* One lease owns both scanners. Firmware coexistence remains enabled. */
    if(!wlan_hal_survey_begin(true)) {
        snprintf(r->status,sizeof(r->status),"%s",wlan_hal_survey_status()); return false;
    }
    r->leased=true;
    r->records=heap_caps_malloc(SCAN_BATCH*sizeof(*r->records),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!r->records) r->records=malloc(SCAN_BATCH*sizeof(*r->records));
    if(!r->records) { wardriver_radio_stop(r); strcpy(r->status,"LOW MEMORY"); return false; }
    __atomic_store_n(&callback_count,&r->callbacks,__ATOMIC_RELEASE);
    __atomic_store_n(&owner,r,__ATOMIC_RELEASE);
    progress(r,"STARTING WI-FI");
    if(!wlan_hal_survey_run(setup_wifi,r) || r->error!=ESP_OK) {
        esp_err_t error=r->error;
        wardriver_radio_stop(r); snprintf(r->status,sizeof(r->status),"WIFI SETUP 0x%x",(unsigned)error); return false;
    }
    progress(r,"STARTING BLE");
    if(!wlan_hal_survey_run(setup_ble,r) || r->error!=ESP_OK) {
        /* Keep the working Wi-Fi survey, but expose the BLE failure explicitly. */
        r->ble_halted=true;
        wlan_hal_survey_run(release_ble,r);
    }
    r->next_scan=0; r->scan_started=(uint32_t)(esp_timer_get_time()/1000);
    r->ble_requested_at=r->scan_started;
    strcpy(r->status,r->ble_halted?"BLE FAILED - WIFI RUNNING":"STARTING BOTH SCANNERS"); return true;
}
static void poll_ble(WardriverRadio* r,uint32_t now) {
    if(r->ble_halted) return;
    uint32_t ready=__atomic_load_n(&r->ble_params,__ATOMIC_ACQUIRE);
    uint32_t started=__atomic_load_n(&r->ble_started,__ATOMIC_ACQUIRE);
    if(ready==2 || started==2 || __atomic_load_n(&r->ble_failed,__ATOMIC_ACQUIRE) ||
       (!r->ble_scanning && now-r->ble_requested_at>10000)) {
        r->ble_halted=true; r->ble_scanning=false; return;
    }
    if(ready==1 && !r->ble_requested) {
        r->ble_requested=true; r->ble_requested_at=now;
        if(!wlan_hal_survey_run(scan_ble,r)) r->error=ESP_FAIL;
        if(r->error!=ESP_OK) r->ble_halted=true;
    }
    /* A successful API return only queues the request; wait for GAP completion. */
    if(started==1) r->ble_scanning=true;
}
static void poll_wifi(WardriverRadio* r,uint32_t now) {
    if(r->halted) return;
    if(__atomic_load_n(&r->failed,__ATOMIC_ACQUIRE)) { retry_scan(r,now); return; }
    if(r->scanning) {
        if(__atomic_exchange_n(&r->done,0,__ATOMIC_ACQ_REL)) {
            if(!wlan_hal_survey_run(collect,r)) r->error=ESP_FAIL;
            r->scanning=false; r->next_scan=now+750;
            if(r->error!=ESP_OK) retry_scan(r,now);
            else r->retries=0;
        } else if(now-r->scan_started>15000) retry_scan(r,now);
    } else if(!r->next_scan || (int32_t)(now-r->next_scan)>=0) {
        __atomic_store_n(&r->done,0,__ATOMIC_RELEASE);
        if(!wlan_hal_survey_run(scan,r)) r->error=ESP_FAIL;
        r->scanning=r->error==ESP_OK; r->scan_started=now;
        if(!r->scanning) retry_scan(r,now);
    }
}
void wardriver_radio_poll(WardriverRadio* r,uint32_t now) {
    if(!r->leased || r->quiesced) return;
    poll_ble(r,now);
    poll_wifi(r,now);
    if(r->halted && r->ble_halted) strcpy(r->status,"RADIO ERROR - RESTART");
    else if(r->halted) strcpy(r->status,"WIFI FAILED - BLE RUNNING");
    else if(r->ble_halted) strcpy(r->status,"BLE FAILED - WIFI RUNNING");
    else if(r->retries) snprintf(r->status,sizeof(r->status),"WIFI RETRY %u/3 + BLE",r->retries);
    else strcpy(r->status,r->ble_scanning?"LISTENING":"WIFI + BLE STARTING");
}
void wardriver_radio_quiesce(WardriverRadio* r) {
    if(!r->leased || r->quiesced) return;
    __atomic_store_n(&owner,NULL,__ATOMIC_RELEASE);
    progress(r,"STOPPING WI-FI SCAN");
    wlan_hal_survey_run(stop_wifi_scan,r);
    progress(r,"REMOVING WIFI CALLBACK");
    wlan_hal_survey_run(remove_wifi_handler,r);
    progress(r,"STOPPING BLE SCAN");
    wlan_hal_survey_run(stop_ble_scan,r);
    progress(r,"RELEASING BLE SCANNER");
    wlan_hal_survey_run(release_ble,r);
    progress(r,"DRAINING CALLBACKS");
    while(__atomic_load_n(&r->callbacks,__ATOMIC_ACQUIRE)) furi_delay_ms(1);
    __atomic_store_n(&callback_count,NULL,__ATOMIC_RELEASE);
    r->scanning=r->ble_scanning=false; r->quiesced=true;
    free(r->records); r->records=NULL;
}
void wardriver_radio_stop(WardriverRadio* r) {
    if(!r->leased) return;
    wardriver_radio_quiesce(r);
    wlan_hal_survey_end(); /* Only after FAP callbacks have stopped and drained. */
    r->leased=false;
    strcpy(r->status,!strcmp(wlan_hal_survey_status(),"RADIO RESTORE FAILED")?"RADIO RESTORE FAILED":"STOPPED");
}
void wardriver_radio_free(WardriverRadio* r) { if(r) { wardriver_radio_stop(r); free(r); } }
uint32_t wardriver_radio_dropped(WardriverRadio* r) { return __atomic_load_n(&r->dropped,__ATOMIC_RELAXED); }
const char* wardriver_radio_status(WardriverRadio* r) { return r->status; }
const char* wardriver_radio_wifi_status(WardriverRadio* r) {
    return !r->leased || r->quiesced?"OFF":r->halted?"ERROR":r->retries?"RETRY":"SCAN";
}
const char* wardriver_radio_ble_status(WardriverRadio* r) {
    return !r->leased || r->quiesced?"OFF":r->ble_halted?"ERROR":r->ble_scanning?"SCAN":"START";
}
