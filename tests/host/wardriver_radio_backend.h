/* Test-only SDK boundary for compiling the complete production radio module.
 * It checks lifecycle/arguments and injects events; it does not model RF. */
#include "wardriver_wifi.h"
#include "wardriver_detect.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"radio FAIL %d: %s\n",__LINE__,#x); abort(); } } while(0)
typedef int esp_err_t;
typedef const char* esp_event_base_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE 0x103
#define WIFI_EVENT "wifi"
#define WIFI_EVENT_SCAN_DONE 1
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_SPIRAM 2
#define MALLOC_CAP_8BIT 4
#define WIFI_PROMIS_FILTER_MASK_MGMT 1
#define WIFI_SCAN_TYPE_PASSIVE 1
typedef int wifi_promiscuous_pkt_type_t;
#define WIFI_PKT_MGMT 1
typedef struct { unsigned status; } wifi_event_sta_scan_done_t;
typedef struct { struct { int8_t rssi; uint8_t channel; uint16_t sig_len; } rx_ctrl; uint8_t payload[2500]; } wifi_promiscuous_pkt_t;
typedef struct { unsigned filter_mask; } wifi_promiscuous_filter_t;
typedef struct { bool show_hidden; int scan_type; struct { unsigned passive; } scan_time; } wifi_scan_config_t;
typedef struct { uint8_t bssid[6],ssid[33],primary,authmode,pairwise_cipher,group_cipher; int8_t rssi; } wifi_ap_record_t;
enum { ESP_BT_CONTROLLER_STATUS_IDLE,ESP_BT_CONTROLLER_STATUS_INITED,ESP_BT_CONTROLLER_STATUS_ENABLED };
enum { ESP_BLUEDROID_STATUS_UNINITIALIZED,ESP_BLUEDROID_STATUS_INITIALIZED,ESP_BLUEDROID_STATUS_ENABLED };
enum { BLE_ADDR_TYPE_PUBLIC,BLE_ADDR_TYPE_RANDOM,BLE_ADDR_TYPE_RPA_RANDOM };
enum { ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT,ESP_GAP_BLE_SCAN_START_COMPLETE_EVT,ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT,ESP_GAP_BLE_SCAN_RESULT_EVT };
#define ESP_GAP_SEARCH_INQ_RES_EVT 1
#define ESP_BT_STATUS_SUCCESS 0
#define ESP_BT_MODE_BLE 1
#define BLE_SCAN_TYPE_PASSIVE 0
#define BLE_SCAN_FILTER_ALLOW_ALL 0
#define BLE_SCAN_DUPLICATE_DISABLE 0
typedef int esp_bt_controller_status_t;
typedef int esp_gap_ble_cb_event_t;
typedef struct { unsigned ble_max_act; } esp_bt_controller_config_t;
#define BT_CONTROLLER_INIT_CONFIG_DEFAULT() { .ble_max_act=6 }
typedef struct { int scan_type,own_addr_type,scan_filter_policy,scan_duplicate; unsigned scan_interval,scan_window; } esp_ble_scan_params_t;
typedef union {
    struct { int status; } scan_param_cmpl,scan_start_cmpl;
    struct { int search_evt,ble_addr_type; int8_t rssi; uint8_t bda[6],ble_adv[62]; size_t adv_data_len; } scan_rst;
} esp_ble_gap_cb_param_t;
struct FuriMessageQueue { WardriverObservation rows[32]; unsigned count; };
static bool leased,registered,promisc,scan_error,collect_error,ble_init_error,worker_failure;
static unsigned scans,ble_scans,collections,cancellations,clears,restores,stages;
static int controller,host;
static uint32_t clock_ms;
static void (*wifi_handler)(void*,esp_event_base_t,int32_t,void*);
static void (*ble_handler)(esp_gap_ble_cb_event_t,esp_ble_gap_cb_param_t*);
static void (*frame_handler)(void*,wifi_promiscuous_pkt_type_t);
void* heap_caps_malloc(size_t n,unsigned caps) { (void)caps; return malloc(n); }
void* heap_caps_calloc(size_t n,size_t sz,unsigned caps) { (void)caps; return calloc(n,sz); }
int64_t esp_timer_get_time(void) { return (int64_t)clock_ms*1000; }
void furi_delay_ms(uint32_t ms) { clock_ms+=ms; }
FuriStatus furi_message_queue_put(FuriMessageQueue* q,const void* data,uint32_t wait) {
    CHECK(wait==0);
    if(q->count==32) return (FuriStatus)1;
    q->rows[q->count++]=*(const WardriverObservation*)data; return FuriStatusOk;
}
static bool wlan_hal_survey_begin(bool wifi) { CHECK(wifi); if(leased) return false; leased=true; return true; }
static bool wlan_hal_survey_run(void (*fn)(void*),void* context) {
    CHECK(leased); if(worker_failure) return false; fn(context); return true;
}
static void wlan_hal_survey_end(void) {
    CHECK(!registered && !promisc && !controller && !host && !frame_handler);
    leased=false; ++restores;
}
static const char* wlan_hal_survey_status(void) { return "RADIO RESTORED"; }
static esp_err_t esp_event_handler_register(esp_event_base_t b,int id,void (*fn)(void*,esp_event_base_t,int32_t,void*),void* arg) {
    CHECK(!strcmp(b,WIFI_EVENT) && id==WIFI_EVENT_SCAN_DONE && !arg);
    wifi_handler=fn; registered=true; return ESP_OK;
}
static esp_err_t esp_event_handler_unregister(esp_event_base_t b,int id,void (*fn)(void*,esp_event_base_t,int32_t,void*)) {
    CHECK(!strcmp(b,WIFI_EVENT) && id==WIFI_EVENT_SCAN_DONE && wifi_handler==fn);
    registered=false; wifi_handler=NULL; return ESP_OK;
}
static esp_err_t esp_wifi_set_promiscuous_filter(const wifi_promiscuous_filter_t* filter) { CHECK(filter->filter_mask==WIFI_PROMIS_FILTER_MASK_MGMT); return ESP_OK; }
static esp_err_t esp_wifi_set_promiscuous_rx_cb(void (*fn)(void*,wifi_promiscuous_pkt_type_t)) { frame_handler=fn; return ESP_OK; }
static esp_err_t esp_wifi_set_promiscuous(bool on) { promisc=on; return ESP_OK; }
static esp_err_t esp_wifi_scan_start(const wifi_scan_config_t* config,bool block) {
    CHECK(config->scan_type==WIFI_SCAN_TYPE_PASSIVE && !block && config->show_hidden);
    ++scans; return scan_error?ESP_FAIL:ESP_OK;
}
static esp_err_t esp_wifi_scan_stop(void) { ++cancellations; return ESP_OK; }
static esp_err_t esp_wifi_clear_ap_list(void) { ++clears; return ESP_OK; }
static esp_err_t esp_wifi_scan_get_ap_num(uint16_t* n) { *n=1; return collect_error?ESP_FAIL:ESP_OK; }
static esp_err_t esp_wifi_scan_get_ap_records(uint16_t* count,wifi_ap_record_t* records) {
    CHECK(*count==1); ++collections;
    *records=(wifi_ap_record_t){.bssid={0,1,2,3,4,5},.ssid="Sample AP",.primary=6,.rssi=-63,.authmode=3}; return ESP_OK;
}
static int esp_bt_controller_get_status(void) { return controller; }
static int esp_bluedroid_get_status(void) { return host; }
static esp_err_t esp_bt_controller_init(const esp_bt_controller_config_t* c) {
    CHECK(!controller && c->ble_max_act==1);
    if(ble_init_error) return ESP_FAIL;
    controller=ESP_BT_CONTROLLER_STATUS_INITED; return ESP_OK;
}
static esp_err_t esp_bt_controller_enable(int mode) { CHECK(mode==ESP_BT_MODE_BLE && controller==1); controller=2; return ESP_OK; }
static esp_err_t esp_bt_controller_disable(void) { CHECK(controller==2 && !host); controller=1; return ESP_OK; }
static esp_err_t esp_bt_controller_deinit(void) { CHECK(controller==1); controller=0; return ESP_OK; }
static esp_err_t esp_bluedroid_init(void) { CHECK(controller==2 && !host); host=1; return ESP_OK; }
static esp_err_t esp_bluedroid_enable(void) { CHECK(host==1); host=2; return ESP_OK; }
static esp_err_t esp_bluedroid_disable(void) { CHECK(host==2); host=1; return ESP_OK; }
static esp_err_t esp_bluedroid_deinit(void) { CHECK(host==1); host=0; ble_handler=NULL; return ESP_OK; }
static esp_err_t esp_ble_gap_register_callback(void (*fn)(esp_gap_ble_cb_event_t,esp_ble_gap_cb_param_t*)) { CHECK(fn); ble_handler=fn; return ESP_OK; }
static esp_err_t esp_ble_gap_set_scan_params(const esp_ble_scan_params_t* p) {
    CHECK(p->scan_type==BLE_SCAN_TYPE_PASSIVE && p->scan_window<=p->scan_interval);
    /* Completions are delivered explicitly by tests, including reordered cases. */
    return ESP_OK;
}
static esp_err_t esp_ble_gap_start_scanning(unsigned duration) { CHECK(duration==0 && host==2); ++ble_scans; return ESP_OK; }
static esp_err_t esp_ble_gap_stop_scanning(void) { return ESP_OK; }
