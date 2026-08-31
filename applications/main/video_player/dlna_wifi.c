#include "dlna_wifi.h"

#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <furi.h>
#include <btshim.h>
#include <string.h>
#include <stdlib.h>

#define TAG "DlnaWifi"
/* Worker issues esp_wifi/lwIP calls. 8192 words (32 KB): the SSDP multicast
 * send path with IP_MULTICAST_IF set is far deeper than a plain UDP send and
 * overflowed the old 16 KB stack (vApplicationStackOverflowHook). The same task
 * also runs the blocking device-description HTTP fetches + XML parse. */
#define DLNA_WIFI_WORKER_STACK 8192

typedef enum {
    DCMD_INIT_START,
    DCMD_STOP_DEINIT,
    DCMD_SCAN,
    DCMD_CONNECT,
    DCMD_DISCONNECT,
    DCMD_RUN_FN,
    DCMD_QUIT,
} DlnaCmdType;

typedef struct {
    DlnaCmdType type;
    union {
        struct {
            wifi_ap_record_t** out_records;
            uint16_t* out_count;
            uint16_t max_count;
        } scan;
        struct {
            char ssid[33];
            char password[65];
            uint8_t bssid[6];
            uint8_t channel;
            bool bssid_set;
        } connect;
        struct {
            DlnaWifiWorkerFn fn;
            void* arg;
        } run_fn;
    };
    volatile bool* done;
    volatile bool* result;
} DlnaCmd;

static bool s_started = false;
static bool s_bt_was_on = false;
static bool s_netif_inited = false;
static bool s_event_handlers_registered = false;
static esp_netif_t* s_netif_sta = NULL;
static volatile bool s_wifi_connected = false;
static volatile bool s_wifi_auto_reconnect = false;
static volatile bool s_auth_fail_latched = false;
static volatile uint32_t s_own_ip = 0;

static QueueHandle_t s_cmd_queue = NULL;
static TaskHandle_t s_worker_task = NULL;
static StackType_t* s_worker_stack = NULL;
/* The TCB and stack MUST live in internal DRAM: FreeRTOS asserts on the TCB
 * memory (xPortCheckValidTCBMem), and this app is a FAP whose BSS is placed in
 * PSRAM — so a `static StaticTask_t` here would fail that check. Allocate both
 * from internal RAM instead. */
static StaticTask_t* s_worker_buf = NULL;

/* Disconnect reasons that indicate a bad password / failed auth (deliberately
 * without "AP not found"/beacon-timeout so a mere outage keeps the creds). */
static bool dlna_reason_is_auth(uint8_t reason) {
    switch(reason) {
    case WIFI_REASON_AUTH_EXPIRE: // 2
    case WIFI_REASON_MIC_FAILURE: // 14
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: // 15
    case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT: // 16
    case WIFI_REASON_IE_IN_4WAY_DIFFERS: // 17
    case WIFI_REASON_AUTH_FAIL: // 202
    case WIFI_REASON_HANDSHAKE_TIMEOUT: // 204
    case WIFI_REASON_CONNECTION_FAIL: // 205
        return true;
    default:
        return false;
    }
}

static void dlna_event_handler(
    void* arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void* event_data) {
    UNUSED(arg);
    if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* d = (wifi_event_sta_disconnected_t*)event_data;
        if(d) {
            ESP_LOGW(TAG, "STA disconnected: reason=%u", (unsigned)d->reason);
            if(dlna_reason_is_auth((uint8_t)d->reason)) s_auth_fail_latched = true;
        }
        s_wifi_connected = false;
        s_own_ip = 0;
        if(s_wifi_auto_reconnect) {
            esp_wifi_connect();
        }
    } else if(event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        s_own_ip = event->ip_info.ip.addr;
        s_wifi_connected = true;
        ESP_LOGI(TAG, "got IP");
    }
}

static void dlna_worker_fn(void* arg) {
    UNUSED(arg);
    ESP_LOGI(TAG, "Worker started");

    DlnaCmd cmd;
    while(1) {
        if(xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) continue;

        bool ok = true;
        esp_err_t err;

        switch(cmd.type) {
        case DCMD_INIT_START:
            if(!s_netif_inited) {
                esp_netif_init();
                esp_event_loop_create_default();
                s_netif_sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
                if(!s_netif_sta) {
                    s_netif_sta = esp_netif_create_default_wifi_sta();
                }
                s_netif_inited = true;
            }
            if(!s_event_handlers_registered) {
                esp_event_handler_register(
                    WIFI_EVENT, ESP_EVENT_ANY_ID, &dlna_event_handler, NULL);
                esp_event_handler_register(
                    IP_EVENT, IP_EVENT_STA_GOT_IP, &dlna_event_handler, NULL);
                s_event_handlers_registered = true;
            }

            wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
            cfg.static_rx_buf_num = 2;
            cfg.dynamic_rx_buf_num = 4;
            cfg.dynamic_tx_buf_num = 8;

            err = esp_wifi_init(&cfg);
            if(err != ESP_OK) {
                ESP_LOGE(TAG, "wifi_init: %s", esp_err_to_name(err));
                ok = false;
                break;
            }
            esp_wifi_set_storage(WIFI_STORAGE_RAM);
            esp_wifi_set_mode(WIFI_MODE_STA);
            err = esp_wifi_start();
            if(err != ESP_OK) {
                ESP_LOGE(TAG, "wifi_start: %s", esp_err_to_name(err));
                esp_wifi_deinit();
                ok = false;
            }
            break;

        case DCMD_STOP_DEINIT:
            s_wifi_auto_reconnect = false;
            esp_wifi_disconnect();
            s_wifi_connected = false;
            s_own_ip = 0;
            esp_wifi_stop();
            esp_wifi_deinit();
            /* Deregister the event handlers — dlna_event_handler lives in the
             * FAP text segment, so a lingering registration would call into dead
             * memory after the app unloads (StoreProhibited on next launch). */
            if(s_event_handlers_registered) {
                esp_event_handler_unregister(
                    WIFI_EVENT, ESP_EVENT_ANY_ID, &dlna_event_handler);
                esp_event_handler_unregister(
                    IP_EVENT, IP_EVENT_STA_GOT_IP, &dlna_event_handler);
                s_event_handlers_registered = false;
            }
            break;

        case DCMD_CONNECT: {
            s_wifi_auto_reconnect = false;
            esp_wifi_disconnect();
            s_wifi_connected = false;
            s_own_ip = 0;
            s_auth_fail_latched = false;
            vTaskDelay(pdMS_TO_TICKS(100));

            wifi_config_t wcfg = {0};
            strncpy((char*)wcfg.sta.ssid, cmd.connect.ssid, 32);
            if(cmd.connect.password[0]) {
                strncpy((char*)wcfg.sta.password, cmd.connect.password, 64);
                wcfg.sta.threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK;
            } else {
                wcfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
            }
            if(cmd.connect.bssid_set) {
                wcfg.sta.bssid_set = true;
                memcpy(wcfg.sta.bssid, cmd.connect.bssid, 6);
            }
            if(cmd.connect.channel) {
                wcfg.sta.channel = cmd.connect.channel;
            }
            wcfg.sta.pmf_cfg.capable = false;
            wcfg.sta.pmf_cfg.required = false;
            s_wifi_auto_reconnect = true;
            esp_wifi_set_config(WIFI_IF_STA, &wcfg);
            err = esp_wifi_connect();
            if(err != ESP_OK) {
                ESP_LOGE(TAG, "connect: %s", esp_err_to_name(err));
                ok = false;
            }
            break;
        }

        case DCMD_DISCONNECT:
            s_wifi_auto_reconnect = false;
            esp_wifi_disconnect();
            s_wifi_connected = false;
            s_own_ip = 0;
            break;

        case DCMD_SCAN: {
            wifi_scan_config_t scfg = {0};
            err = esp_wifi_scan_start(&scfg, true);
            if(err != ESP_OK) {
                ESP_LOGE(TAG, "scan: %s", esp_err_to_name(err));
                *cmd.scan.out_count = 0;
                *cmd.scan.out_records = NULL;
                ok = false;
                break;
            }
            uint16_t count = 0;
            esp_wifi_scan_get_ap_num(&count);
            if(count > cmd.scan.max_count) count = cmd.scan.max_count;
            if(count > 0) {
                *cmd.scan.out_records = malloc(count * sizeof(wifi_ap_record_t));
                if(*cmd.scan.out_records) {
                    esp_wifi_scan_get_ap_records(&count, *cmd.scan.out_records);
                } else {
                    count = 0;
                }
            } else {
                *cmd.scan.out_records = NULL;
            }
            *cmd.scan.out_count = count;
            break;
        }

        case DCMD_RUN_FN:
            if(cmd.run_fn.fn) cmd.run_fn.fn(cmd.run_fn.arg);
            break;

        case DCMD_QUIT:
            ESP_LOGI(TAG, "Worker quitting");
            if(cmd.done) *cmd.done = true;
            vTaskDelete(NULL);
            return;
        }

        if(cmd.result) *cmd.result = ok;
        if(cmd.done) *cmd.done = true;
    }
}

static bool dlna_ensure_worker(void) {
    if(s_worker_task) return true;

    s_cmd_queue = xQueueCreate(4, sizeof(DlnaCmd));
    if(!s_cmd_queue) return false;

    s_worker_stack = heap_caps_malloc(
        DLNA_WIFI_WORKER_STACK * sizeof(StackType_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_worker_buf = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if(!s_worker_stack || !s_worker_buf) {
        free(s_worker_stack);
        s_worker_stack = NULL;
        free(s_worker_buf);
        s_worker_buf = NULL;
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
        ESP_LOGE(TAG, "Cannot alloc worker stack/TCB");
        return false;
    }

    s_worker_task = xTaskCreateStaticPinnedToCore(
        dlna_worker_fn, "DlnaWifi", DLNA_WIFI_WORKER_STACK,
        NULL, 5, s_worker_stack, s_worker_buf, 0);
    return s_worker_task != NULL;
}

static void dlna_send_cmd_sync(DlnaCmd* cmd) {
    volatile bool done = false;
    cmd->done = &done;
    xQueueSend(s_cmd_queue, cmd, portMAX_DELAY);
    while(!done) {
        furi_delay_ms(10);
    }
}

bool dlna_wifi_start(void) {
    if(s_started) return true;

    Bt* bt = furi_record_open(RECORD_BT);
    s_bt_was_on = bt_is_enabled(bt);
    if(s_bt_was_on) {
        bt_stop_stack(bt);
    }
    furi_record_close(RECORD_BT);

    if(!dlna_ensure_worker()) return false;

    volatile bool result = false;
    DlnaCmd cmd = {.type = DCMD_INIT_START, .result = &result};
    dlna_send_cmd_sync(&cmd);

    if(result) {
        s_started = true;
        ESP_LOGI(TAG, "WiFi started");
    }
    return result;
}

void dlna_wifi_stop(void) {
    if(s_started) {
        DlnaCmd cmd = {.type = DCMD_STOP_DEINIT};
        dlna_send_cmd_sync(&cmd);
        s_started = false;
        ESP_LOGI(TAG, "WiFi stopped");
    }

    /* Terminate the worker task. It runs dlna_worker_fn() from the FAP text
     * segment, so it MUST NOT outlive the app — a leftover task crashes on the
     * next launch when the FAP is reloaded (StoreProhibited, 0xa5a5a5a5). */
    if(s_worker_task) {
        volatile bool done = false;
        DlnaCmd cmd = {.type = DCMD_QUIT, .done = &done};
        xQueueSend(s_cmd_queue, &cmd, portMAX_DELAY);
        while(!done) furi_delay_ms(5);
        furi_delay_ms(20); /* let the scheduler reclaim the deleted task */
        s_worker_task = NULL;
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
        free(s_worker_stack);
        s_worker_stack = NULL;
        free(s_worker_buf);
        s_worker_buf = NULL;
    }

    if(s_bt_was_on) {
        Bt* bt = furi_record_open(RECORD_BT);
        bt_start_stack(bt);
        furi_record_close(RECORD_BT);
        s_bt_was_on = false;
    }

    /* Reset init flag so a fresh app instance re-initialises cleanly. */
    s_netif_inited = false;
}

bool dlna_wifi_is_started(void) {
    return s_started;
}

void dlna_wifi_scan(wifi_ap_record_t** out_records, uint16_t* out_count, uint16_t max_count) {
    *out_records = NULL;
    *out_count = 0;
    if(!s_started) return;

    DlnaCmd cmd = {
        .type = DCMD_SCAN,
        .scan = {.out_records = out_records, .out_count = out_count, .max_count = max_count},
    };
    dlna_send_cmd_sync(&cmd);
}

bool dlna_wifi_connect(
    const char* ssid,
    const char* password,
    const uint8_t* bssid,
    uint8_t channel) {
    if(!s_started || !ssid) return false;

    DlnaCmd cmd = {.type = DCMD_CONNECT};
    memset(&cmd.connect, 0, sizeof(cmd.connect));
    strncpy(cmd.connect.ssid, ssid, 32);
    if(password && password[0]) {
        strncpy(cmd.connect.password, password, 64);
    }
    if(bssid) {
        memcpy(cmd.connect.bssid, bssid, 6);
        cmd.connect.bssid_set = true;
    }
    cmd.connect.channel = channel;
    dlna_send_cmd_sync(&cmd);
    return true;
}

void dlna_wifi_disconnect(void) {
    if(!s_started) return;
    DlnaCmd cmd = {.type = DCMD_DISCONNECT};
    dlna_send_cmd_sync(&cmd);
}

bool dlna_wifi_is_connected(void) {
    return s_wifi_connected;
}

bool dlna_wifi_last_fail_is_auth(void) {
    return s_auth_fail_latched;
}

uint32_t dlna_wifi_get_own_ip(void) {
    return s_own_ip;
}

bool dlna_wifi_run_in_worker(DlnaWifiWorkerFn fn, void* arg) {
    if(!s_worker_task) return false;
    DlnaCmd cmd = {.type = DCMD_RUN_FN, .run_fn = {.fn = fn, .arg = arg}};
    dlna_send_cmd_sync(&cmd);
    return true;
}
