#include "wifi_hal.h"
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <furi.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <btshim.h>

#define TAG "WifiHal"
#define WORKER_STACK_SIZE 4096

static bool s_wifi_started = false;
static bool s_bt_was_on = false;
static volatile bool s_wifi_connected = false;
static volatile bool s_wifi_auto_reconnect = false;
static bool s_netif_initialized = false;
static esp_netif_t* s_netif_sta = NULL;

// Status bar
static ViewPort* s_statusbar_vp = NULL;
static Gui* s_gui = NULL;

// --- Persistent WiFi worker task ---
// All ESP-IDF WiFi API calls must come from the same task (pthread TLS).

typedef enum {
    WCMD_INIT_START,
    WCMD_STOP_DEINIT,
    WCMD_SCAN,
    WCMD_SET_CHANNEL,
    WCMD_SET_PROMISC,
    WCMD_SEND_RAW,
    WCMD_CONNECT,
    WCMD_DISCONNECT,
    WCMD_QUIT,
} WifiCmdType;

typedef struct {
    WifiCmdType type;
    union {
        struct {
            uint8_t channel;
        } set_channel;
        struct {
            bool enable;
            wifi_promiscuous_cb_t cb;
        } set_promisc;
        struct {
            uint8_t buf[64];
            uint16_t len;
        } send_raw;
        struct {
            wifi_scan_config_t* config;
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
    };
    volatile bool* done;
    volatile bool* result;
} WifiCmd;

static QueueHandle_t s_cmd_queue = NULL;
static TaskHandle_t s_worker_task = NULL;
static StackType_t* s_worker_stack = NULL;
static StaticTask_t s_worker_task_buf;

// --- Status bar icon ---

static void wifi_statusbar_draw_cb(Canvas* canvas, void* context) {
    UNUSED(context);
    // WiFi fan icon 8x8
    canvas_draw_dot(canvas, 3, 7);
    canvas_draw_dot(canvas, 4, 7);
    canvas_draw_dot(canvas, 2, 5);
    canvas_draw_dot(canvas, 3, 5);
    canvas_draw_dot(canvas, 4, 5);
    canvas_draw_dot(canvas, 5, 5);
    canvas_draw_dot(canvas, 1, 3);
    canvas_draw_dot(canvas, 2, 3);
    canvas_draw_dot(canvas, 5, 3);
    canvas_draw_dot(canvas, 6, 3);
    canvas_draw_dot(canvas, 0, 1);
    canvas_draw_dot(canvas, 1, 1);
    canvas_draw_dot(canvas, 6, 1);
    canvas_draw_dot(canvas, 7, 1);
}

static void wifi_statusbar_ensure(void) {
    if(s_statusbar_vp) return;
    s_statusbar_vp = view_port_alloc();
    view_port_set_width(s_statusbar_vp, 8);
    view_port_draw_callback_set(s_statusbar_vp, wifi_statusbar_draw_cb, NULL);
    view_port_enabled_set(s_statusbar_vp, false);
    s_gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(s_gui, s_statusbar_vp, GuiLayerStatusBarLeft);
}

static void wifi_statusbar_update(bool connected) {
    // Only update if viewport already created (don't create from event handler)
    if(!s_statusbar_vp) return;
    view_port_enabled_set(s_statusbar_vp, connected);
    view_port_update(s_statusbar_vp);
}

// --- ESP-IDF WiFi event handler ---
// Runs on system event task — must NOT call furi_record_open or alloc GUI resources

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    UNUSED(arg);
    if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* disc = (wifi_event_sta_disconnected_t*)event_data;
        s_wifi_connected = false;
        wifi_statusbar_update(false);
        if(s_wifi_auto_reconnect) {
            ESP_LOGW(TAG, "STA disconnected, reason=%d, reconnecting...", disc->reason);
            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "STA disconnected, reason=%d", disc->reason);
        }
    } else if(event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_connected = true;
        wifi_statusbar_update(true);
    }
}

// --- Worker task ---

static void wifi_worker_fn(void* arg) {
    UNUSED(arg);
    ESP_LOGI(TAG, "Worker task started");

    WifiCmd cmd;
    while(1) {
        if(xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) continue;

        bool ok = true;
        esp_err_t err;

        switch(cmd.type) {
        case WCMD_INIT_START: {
            // Event loop + netif base init (lightweight, needed for WiFi events)
            if(!s_netif_initialized) {
                esp_netif_init();
                esp_event_loop_create_default();
                s_netif_sta = esp_netif_create_default_wifi_sta();
                esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
                esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);
                s_netif_initialized = true;
            }

            wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
            cfg.static_rx_buf_num = 2;
            cfg.dynamic_rx_buf_num = 4;
            cfg.dynamic_tx_buf_num = 8;

            err = esp_wifi_init(&cfg);
            if(err != ESP_OK) {
                ESP_LOGE(TAG, "wifi init: %s", esp_err_to_name(err));
                ok = false;
                break;
            }
            esp_wifi_set_storage(WIFI_STORAGE_RAM);
            esp_wifi_set_mode(WIFI_MODE_STA);
            err = esp_wifi_start();
            if(err != ESP_OK) {
                ESP_LOGE(TAG, "wifi start: %s", esp_err_to_name(err));
                esp_wifi_deinit();
                ok = false;
            }
            break;
        }
        case WCMD_STOP_DEINIT:
            s_wifi_auto_reconnect = false;
            esp_wifi_disconnect();
            s_wifi_connected = false;
            wifi_statusbar_update(false);
            esp_wifi_set_promiscuous(false);
            esp_wifi_stop();
            esp_wifi_deinit();
            break;

        case WCMD_SCAN: {
            err = esp_wifi_scan_start(cmd.scan.config, true);
            if(err != ESP_OK) {
                ESP_LOGE(TAG, "scan: %s", esp_err_to_name(err));
                *cmd.scan.out_count = 0;
                ok = false;
                break;
            }
            uint16_t count = 0;
            esp_wifi_scan_get_ap_num(&count);
            if(count > cmd.scan.max_count) count = cmd.scan.max_count;
            *cmd.scan.out_records = malloc(count * sizeof(wifi_ap_record_t));
            esp_wifi_scan_get_ap_records(&count, *cmd.scan.out_records);
            *cmd.scan.out_count = count;
            break;
        }
        case WCMD_SET_CHANNEL:
            esp_wifi_set_channel(cmd.set_channel.channel, WIFI_SECOND_CHAN_NONE);
            break;

        case WCMD_SET_PROMISC:
            if(cmd.set_promisc.enable && cmd.set_promisc.cb) {
                esp_wifi_set_promiscuous_rx_cb(cmd.set_promisc.cb);
            }
            esp_wifi_set_promiscuous(cmd.set_promisc.enable);
            if(!cmd.set_promisc.enable) {
                esp_wifi_set_promiscuous_rx_cb(NULL);
            }
            break;

        case WCMD_SEND_RAW: {
            esp_err_t tx_err = esp_wifi_80211_tx(WIFI_IF_STA, cmd.send_raw.buf, cmd.send_raw.len, false);
            if(tx_err != ESP_OK) {
                ESP_LOGE(TAG, "80211_tx: %s", esp_err_to_name(tx_err));
            }
            break;
        }

        case WCMD_CONNECT: {
            // Clean disconnect before new connection attempt
            s_wifi_auto_reconnect = false;
            esp_wifi_disconnect();
            s_wifi_connected = false;
            vTaskDelay(pdMS_TO_TICKS(100));

            wifi_config_t wifi_cfg = {0};
            strncpy((char*)wifi_cfg.sta.ssid, cmd.connect.ssid, 32);
            if(cmd.connect.password[0]) {
                strncpy((char*)wifi_cfg.sta.password, cmd.connect.password, 64);
                wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK;
            } else {
                wifi_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
            }
            // Target specific AP by BSSID + channel (avoids auth timeout on multi-AP networks)
            if(cmd.connect.bssid_set) {
                wifi_cfg.sta.bssid_set = true;
                memcpy(wifi_cfg.sta.bssid, cmd.connect.bssid, 6);
            }
            if(cmd.connect.channel) {
                wifi_cfg.sta.channel = cmd.connect.channel;
            }
            // Disable PMF — some open/captive-portal APs reject auth with PMF enabled
            wifi_cfg.sta.pmf_cfg.capable = false;
            wifi_cfg.sta.pmf_cfg.required = false;
            s_wifi_auto_reconnect = true;
            esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
            err = esp_wifi_connect();
            if(err != ESP_OK) {
                ESP_LOGE(TAG, "connect: %s", esp_err_to_name(err));
                ok = false;
            }
            break;
        }
        case WCMD_DISCONNECT:
            s_wifi_auto_reconnect = false;
            esp_wifi_disconnect();
            s_wifi_connected = false;
            wifi_statusbar_update(false);
            break;

        case WCMD_QUIT:
            ESP_LOGI(TAG, "Worker quitting");
            if(cmd.done) *cmd.done = true;
            vTaskSuspend(NULL);
            return;
        }

        if(cmd.result) *cmd.result = ok;
        if(cmd.done) *cmd.done = true;
    }
}

static void wifi_send_cmd_sync(WifiCmd* cmd) {
    volatile bool done = false;
    cmd->done = &done;
    xQueueSend(s_cmd_queue, cmd, portMAX_DELAY);
    while(!done) {
        furi_delay_ms(10);
    }
}

static bool wifi_ensure_worker(void) {
    if(s_worker_task) return true;

    s_cmd_queue = xQueueCreate(4, sizeof(WifiCmd));
    if(!s_cmd_queue) return false;

    s_worker_stack = heap_caps_malloc(
        WORKER_STACK_SIZE * sizeof(StackType_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if(!s_worker_stack) {
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
        ESP_LOGE(TAG, "Cannot alloc worker stack");
        return false;
    }

    s_worker_task = xTaskCreateStaticPinnedToCore(
        wifi_worker_fn, "WifiWorker", WORKER_STACK_SIZE,
        NULL, 5, s_worker_stack, &s_worker_task_buf, 0);

    return s_worker_task != NULL;
}

void wifi_hal_preinit(void) {
    // No-op
}

bool wifi_hal_start(void) {
    if(s_wifi_started) return true;

    // Stop BLE advertising (WiFi + BLE coexist but can't both use radio simultaneously)
    Bt* bt = furi_record_open(RECORD_BT);
    s_bt_was_on = bt_is_enabled(bt);
    if(s_bt_was_on) {
        bt_stop_stack(bt);
    }
    furi_record_close(RECORD_BT);

    ESP_LOGI(TAG, "Starting WiFi (internal heap: %lu)",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    if(!wifi_ensure_worker()) return false;

    volatile bool result = false;
    WifiCmd cmd = {.type = WCMD_INIT_START, .result = &result};
    wifi_send_cmd_sync(&cmd);

    if(result) {
        s_wifi_started = true;
        ESP_LOGI(TAG, "WiFi started (internal heap: %lu)",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    return result;
}

void wifi_hal_stop(void) {
    if(s_wifi_started) {
        WifiCmd cmd = {.type = WCMD_STOP_DEINIT};
        wifi_send_cmd_sync(&cmd);
        s_wifi_started = false;
        ESP_LOGI(TAG, "WiFi stopped");
    }

    // Keep worker alive — WiFi pthread TLS requires same task for re-init.
    // Worker will be reused on next wifi_hal_start().

    // Restart BLE after WiFi (stripe-based display rendering freed ~95KB SRAM)
    if(s_bt_was_on) {
        Bt* bt = furi_record_open(RECORD_BT);
        bt_start_stack(bt);
        furi_record_close(RECORD_BT);
        s_bt_was_on = false;
    }
}

void wifi_hal_scan(wifi_ap_record_t** out_records, uint16_t* out_count, uint16_t max_count) {
    if(!s_wifi_started) {
        *out_count = 0;
        return;
    }
    wifi_scan_config_t scan_config = {
        .ssid = NULL, .bssid = NULL, .channel = 0,
        .show_hidden = true, .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    WifiCmd cmd = {
        .type = WCMD_SCAN,
        .scan = {
            .config = &scan_config,
            .out_records = out_records,
            .out_count = out_count,
            .max_count = max_count,
        },
    };
    wifi_send_cmd_sync(&cmd);
}

void wifi_hal_set_channel(uint8_t channel) {
    if(!s_wifi_started || channel < 1 || channel > 14) return;
    WifiCmd cmd = {.type = WCMD_SET_CHANNEL, .set_channel = {.channel = channel}};
    wifi_send_cmd_sync(&cmd);
}

void wifi_hal_set_promiscuous(bool enable, wifi_promiscuous_cb_t cb) {
    if(!s_wifi_started) return;
    WifiCmd cmd = {.type = WCMD_SET_PROMISC, .set_promisc = {.enable = enable, .cb = cb}};
    wifi_send_cmd_sync(&cmd);
}

bool wifi_hal_send_raw(const uint8_t* data, uint16_t len) {
    if(!s_wifi_started || !s_cmd_queue || len > 64) return false;
    WifiCmd cmd = {.type = WCMD_SEND_RAW, .done = NULL, .result = NULL};
    memcpy(cmd.send_raw.buf, data, len);
    cmd.send_raw.len = len;
    return xQueueSend(s_cmd_queue, &cmd, 0) == pdTRUE;
}

bool wifi_hal_connect(const char* ssid, const char* password, const uint8_t* bssid, uint8_t channel) {
    if(!s_wifi_started || !ssid) return false;

    // Create statusbar on first connect attempt (app task context, safe for GUI)
    wifi_statusbar_ensure();

    WifiCmd cmd = {.type = WCMD_CONNECT};
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

    volatile bool result = false;
    cmd.result = &result;
    wifi_send_cmd_sync(&cmd);
    return result;
}

void wifi_hal_disconnect(void) {
    if(!s_wifi_started) return;
    WifiCmd cmd = {.type = WCMD_DISCONNECT};
    wifi_send_cmd_sync(&cmd);
}

bool wifi_hal_is_connected(void) {
    return s_wifi_connected;
}

bool wifi_hal_is_started(void) {
    return s_wifi_started;
}

void wifi_hal_cleanup(void) {
    wifi_hal_stop();
    if(s_worker_task) {
        WifiCmd quit = {.type = WCMD_QUIT};
        wifi_send_cmd_sync(&quit);
        s_worker_task = NULL;
    }
    if(s_worker_stack) {
        heap_caps_free(s_worker_stack);
        s_worker_stack = NULL;
    }
    if(s_cmd_queue) {
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
    }
}

void wifi_hal_cleanup_keep_connection(void) {
    if(!s_wifi_connected) {
        wifi_hal_cleanup();
        return;
    }
    // Keep worker, queue, and WiFi alive — just release app references
    ESP_LOGI(TAG, "Keeping WiFi connection alive after app exit");
}
