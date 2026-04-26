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
#include <string.h>
#include <stdlib.h>
#include <time.h>

#define TAG "WifiHal"
#define WORKER_STACK_SIZE 4096

static bool s_wifi_started = false;
static bool s_bt_was_on = false;
static volatile bool s_wifi_connected = false;
static volatile bool s_wifi_auto_reconnect = false;
static bool s_netif_initialized = false;
static esp_netif_t* s_netif_sta = NULL;

static ViewPort* s_statusbar_vp = NULL;
static Gui* s_gui = NULL;

typedef enum {
    WCMD_INIT_START,
    WCMD_STOP_DEINIT,
    WCMD_SCAN,
    WCMD_SET_CHANNEL,
    WCMD_SET_PROMISC,
    WCMD_SEND_RAW,
    WCMD_CONNECT,
    WCMD_DISCONNECT,
    WCMD_BEACON_SPAM_START,
    WCMD_BEACON_SPAM_STOP,
    WCMD_RUN_FN,
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
        struct {
            WifiHalBeaconMode mode;
            char base_ssid[33];
        } beacon_start;
        struct {
            WifiHalWorkerFn fn;
            void* arg;
        } run_fn;
    };
    volatile bool* done;
    volatile bool* result;
} WifiCmd;

static QueueHandle_t s_cmd_queue = NULL;
static TaskHandle_t s_worker_task = NULL;
static StackType_t* s_worker_stack = NULL;
static StaticTask_t s_worker_task_buf;

static volatile bool s_beacon_active = false;
static uint32_t s_beacon_frames = 0;
static TaskHandle_t s_beacon_task = NULL;

// Static buffer for passing parameters to beacon task (9 uint32_t slots)
static uint32_t s_beacon_params[9];

static const uint8_t beacon_packet_template[] = {
    0x80, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x83, 0x51, 0xf7, 0x8f, 0x0f, 0x00,
    0x00, 0x00, 0xe8, 0x03, 0x31, 0x00, 0x00, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x01, 0x08,
    0x82, 0x84, 0x8b, 0x96, 0x24, 0x30, 0x48, 0x6c, 0x03, 0x01,
    0x01, 0x30, 0x18, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x02, 0x02,
    0x00, 0x00, 0x0f, 0xac, 0x04, 0x00, 0x0f, 0xac, 0x04, 0x01,
    0x00, 0x00, 0x0f, 0xac, 0x02, 0x00, 0x00
};

static const char* funny_ssids[] = {
    "Mom Use This One", "Abraham Linksys", "Benjamin FrankLAN",
    "Martin Router King", "John Wilkes Bluetooth", "Pretty Fly for a Wi-Fi",
    "Bill Wi the Science Fi", "I Believe Wi Can Fi", "Tell My Wi-Fi Love Her",
    "No More Mister Wi-Fi", "LAN Solo", "The LAN Before Time",
    "Silence of the LANs", "House LANister", "Winternet Is Coming",
    "FBI Surveillance Van 4", "Area 51 Test Site", "Never Gonna Give You Up",
    "Loading...", "VIRUS.EXE", "Free Public Wi-Fi", "404 Wi-Fi Unavailable",
    NULL
};

static const char* rickroll_ssids[] = {
    "01 Never gonna give you up",
    "02 Never gonna let you down",
    "03 Never gonna run around",
    "04 And desert you",
    "05 Never gonna make you cry",
    "06 Never gonna say goodbye",
    "07 Never gonna tell a lie",
    "08 And hurt you",
    "09 We’ve known each other for so long",
    "10 Your heart’s been aching, you know it",
    "11 Inside we both know what’s going on",
    "12 We understand the game",
    "13 Gonna stick together forever",

    NULL
};

static uint8_t channels[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
static int channel_idx = 0;

static void wifi_statusbar_draw_cb(Canvas* canvas, void* context) {
    UNUSED(context);
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
    if(!s_statusbar_vp) return;
    view_port_enabled_set(s_statusbar_vp, connected);
    view_port_update(s_statusbar_vp);
}

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

static void prepare_beacon_packet(uint8_t* packet, const uint8_t* mac, const char* ssid, uint8_t channel) {
    memcpy(packet, beacon_packet_template, sizeof(beacon_packet_template));
    memcpy(&packet[10], mac, 6);
    memcpy(&packet[16], mac, 6);
    uint8_t ssid_len = strlen(ssid);
    if(ssid_len > 32) ssid_len = 32;
    packet[37] = ssid_len;
    memcpy(&packet[38], ssid, ssid_len);
    packet[82] = channel;
}

static void beacon_spam_task(void* param) {
    uint32_t* p = (uint32_t*)param;
    WifiHalBeaconMode mode = (WifiHalBeaconMode)p[0];
    char base_ssid[33] = {0};
    if(mode == WifiHalBeaconModeCustom) {
        memcpy(base_ssid, &p[1], 32);
    }
    
    ESP_LOGI(TAG, "Beacon task started with mode=%d", mode);
    
    const char** ssid_list = NULL;
    char random_ssid[64];
    uint8_t mac[6];
    uint8_t packet[sizeof(beacon_packet_template)];
    int ssid_index = 0;
    int counter = 1;
    uint8_t channel = 1;
    
    srand(time(NULL));
    
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    esp_wifi_set_promiscuous(true);
    
    while(s_beacon_active) {
        // Get next SSID
        const char* current_ssid = NULL;
        switch(mode) {
            case WifiHalBeaconModeFunny:
                if(!ssid_list) ssid_list = funny_ssids;
                current_ssid = ssid_list[ssid_index];
                ssid_index++;
                if(ssid_list[ssid_index] == NULL) ssid_index = 0;
                break;
            case WifiHalBeaconModeRickroll:
                if(!ssid_list) ssid_list = rickroll_ssids;
                current_ssid = ssid_list[ssid_index];
                ssid_index++;
                if(ssid_list[ssid_index] == NULL) ssid_index = 0;
                break;
            case WifiHalBeaconModeRandom:
                snprintf(random_ssid, sizeof(random_ssid), "SSID_%d", rand() % 9999);
                current_ssid = random_ssid;
                break;
            case WifiHalBeaconModeCustom:
                if(base_ssid[0] != '\0') {
                    snprintf(random_ssid, sizeof(random_ssid), "%s%d", base_ssid, counter++);
                    if(counter > 9999) counter = 1;
                } else {
                    snprintf(random_ssid, sizeof(random_ssid), "SSID_%d", rand() % 9999);
                }
                current_ssid = random_ssid;
                break;
            default:
                current_ssid = "ESP32 Flipper";
                break;
        }
        
        // Generate random MAC
        for(int i = 0; i < 6; i++) mac[i] = rand() % 256;
        mac[0] = (mac[0] & 0xFE) | 0x02;
        
        // Send on current channel
        prepare_beacon_packet(packet, mac, current_ssid, channel);
        esp_wifi_80211_tx(WIFI_IF_STA, packet, sizeof(beacon_packet_template), false);
        
        s_beacon_frames++;
        
        // Change channel every 5 packets for better spread
        if(s_beacon_frames % 5 == 0) {
            channel++;
            if(channel > 11) channel = 1;
            esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
        }
        
        // Small delay 
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    esp_wifi_set_promiscuous(false);
    s_beacon_task = NULL;
    vTaskDelete(NULL);
}

static void wifi_worker_fn(void* arg) {
    UNUSED(arg);
    ESP_LOGI(TAG, "Worker task started");

    WifiCmd cmd;
    while(1) {
        if(xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) continue;

        bool ok = true;
        esp_err_t err;

        switch(cmd.type) {
        case WCMD_INIT_START:
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
            
        case WCMD_STOP_DEINIT:
            if(s_beacon_active) {
                s_beacon_active = false;
                while(s_beacon_task) vTaskDelay(pdMS_TO_TICKS(10));
            }
            s_wifi_auto_reconnect = false;
            esp_wifi_disconnect();
            s_wifi_connected = false;
            wifi_statusbar_update(false);
            esp_wifi_set_promiscuous(false);
            esp_wifi_stop();
            esp_wifi_deinit();
            break;

        case WCMD_SCAN:
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

        case WCMD_SEND_RAW:
            esp_err_t tx_err = esp_wifi_80211_tx(WIFI_IF_STA, cmd.send_raw.buf, cmd.send_raw.len, false);
            if(tx_err != ESP_OK) {
                ESP_LOGE(TAG, "80211_tx: %s", esp_err_to_name(tx_err));
            }
            break;

        case WCMD_CONNECT:
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
            if(cmd.connect.bssid_set) {
                wifi_cfg.sta.bssid_set = true;
                memcpy(wifi_cfg.sta.bssid, cmd.connect.bssid, 6);
            }
            if(cmd.connect.channel) {
                wifi_cfg.sta.channel = cmd.connect.channel;
            }
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
            
        case WCMD_DISCONNECT:
            s_wifi_auto_reconnect = false;
            esp_wifi_disconnect();
            s_wifi_connected = false;
            wifi_statusbar_update(false);
            break;

        case WCMD_BEACON_SPAM_START:
            if(s_beacon_active) {
                ok = false;
                break;
            }
            s_beacon_active = true;
            s_beacon_frames = 0;
            
            s_beacon_params[0] = (uint32_t)cmd.beacon_start.mode;
            if(cmd.beacon_start.mode == WifiHalBeaconModeCustom) {
                memcpy(&s_beacon_params[1], cmd.beacon_start.base_ssid, 32);
            }
            
            xTaskCreate(beacon_spam_task, "BeaconSpam", 4096, s_beacon_params, 5, &s_beacon_task);
            break;
            
        case WCMD_BEACON_SPAM_STOP:
            s_beacon_active = false;
            while(s_beacon_task) vTaskDelay(pdMS_TO_TICKS(10));
            break;

        case WCMD_RUN_FN:
            if(cmd.run_fn.fn) {
                cmd.run_fn.fn(cmd.run_fn.arg);
            }
            break;

        case WCMD_QUIT:
            ESP_LOGI(TAG, "Worker quitting");
            if(cmd.done) *cmd.done = true;
            vTaskDelete(NULL);
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
}

bool wifi_hal_run_in_worker(WifiHalWorkerFn fn, void* arg) {
    if(!fn) return false;
    if(!wifi_ensure_worker()) return false;
    WifiCmd cmd = {.type = WCMD_RUN_FN, .run_fn = {.fn = fn, .arg = arg}};
    wifi_send_cmd_sync(&cmd);
    return true;
}

bool wifi_hal_start(void) {
    if(s_wifi_started) return true;

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
        TaskHandle_t old = s_worker_task;
        WifiCmd quit = {.type = WCMD_QUIT};
        wifi_send_cmd_sync(&quit);
        s_worker_task = NULL;
        /* Wait until idle task has fully processed vTaskDelete before
         * freeing the stack. Accessing it earlier crashes the next
         * context switch (TCB still references the freed stack). */
        while(eTaskGetState(old) != eDeleted) {
            vTaskDelay(1);
        }
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
    ESP_LOGI(TAG, "Keeping WiFi connection alive after app exit");
}

void wifi_hal_beacon_spam_start(WifiHalBeaconMode mode, const char* base_ssid) {
    ESP_LOGI(TAG, "beacon_spam_start called, mode=%d, wifi_started=%d", mode, s_wifi_started);
    
    if(!s_wifi_started) {
        ESP_LOGE(TAG, "WiFi not started, cannot spam");
        return;
    }
    
    WifiCmd cmd = {.type = WCMD_BEACON_SPAM_START};
    cmd.beacon_start.mode = mode;
    if(base_ssid) {
        strncpy(cmd.beacon_start.base_ssid, base_ssid, 32);
    }
    
    wifi_send_cmd_sync(&cmd);
}

void wifi_hal_beacon_spam_stop(void) {
    if(!s_wifi_started) return;
    WifiCmd cmd = {.type = WCMD_BEACON_SPAM_STOP};
    wifi_send_cmd_sync(&cmd);
}

bool wifi_hal_beacon_spam_is_running(void) {
    return s_beacon_active;
}

uint32_t wifi_hal_beacon_spam_get_frame_count(void) {
    return s_beacon_frames;
}