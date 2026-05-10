#include "wlan_hal.h"

#include <esp_wifi.h>
#include <esp_private/wifi.h>
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

#define TAG "WlanHal"
#define WLAN_HAL_WORKER_STACK 8192

typedef enum {
    WCMD_INIT_START,
    WCMD_STOP_DEINIT,
    WCMD_SCAN,
    WCMD_CONNECT,
    WCMD_DISCONNECT,
    WCMD_SEND_ETH_RAW,
    WCMD_SET_CHANNEL,
    WCMD_SET_PROMISC,
    WCMD_SEND_RAW,
    WCMD_RUN_FN,
    WCMD_QUIT,
} WlanCmdType;

typedef struct {
    WlanCmdType type;
    union {
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
            uint8_t* buf;   // heap-alloziert vom Sender, free durch Consumer
            uint16_t len;
        } send_eth;
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
            WlanHalWorkerFn fn;
            void* arg;
        } run_fn;
    };
    volatile bool* done;
    volatile bool* result;
} WlanCmd;

static bool s_started = false;
static bool s_bt_was_on = false;
static bool s_netif_inited = false;
static esp_netif_t* s_netif_sta = NULL;
static volatile bool s_wifi_connected = false;
static volatile bool s_wifi_auto_reconnect = false;
static bool s_event_handlers_registered = false;
static volatile uint32_t s_own_ip = 0;
static volatile uint32_t s_own_netmask = 0;

static QueueHandle_t s_cmd_queue = NULL;
static TaskHandle_t s_worker_task = NULL;
static StackType_t* s_worker_stack = NULL;
static StaticTask_t s_worker_buf;

static void wlan_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    UNUSED(arg);
    if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_connected = false;
        s_own_ip = 0;
        s_own_netmask = 0;
        if(s_wifi_auto_reconnect) {
            esp_wifi_connect();
        }
    } else if(event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        s_own_ip = event->ip_info.ip.addr;
        s_own_netmask = event->ip_info.netmask.addr;
        s_wifi_connected = true;
    }
}

static void wlan_worker_fn(void* arg) {
    UNUSED(arg);
    ESP_LOGI(TAG, "Worker started");

    WlanCmd cmd;
    while(1) {
        if(xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) continue;

        bool ok = true;
        esp_err_t err;

        switch(cmd.type) {
        case WCMD_INIT_START:
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
                    WIFI_EVENT, ESP_EVENT_ANY_ID, &wlan_event_handler, NULL);
                esp_event_handler_register(
                    IP_EVENT, IP_EVENT_STA_GOT_IP, &wlan_event_handler, NULL);
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

        case WCMD_STOP_DEINIT:
            s_wifi_auto_reconnect = false;
            esp_wifi_disconnect();
            s_wifi_connected = false;
            s_own_ip = 0;
            s_own_netmask = 0;
            esp_wifi_set_promiscuous(false);
            esp_wifi_stop();
            esp_wifi_deinit();
            break;

        case WCMD_CONNECT: {
            s_wifi_auto_reconnect = false;
            esp_wifi_disconnect();
            s_wifi_connected = false;
            s_own_ip = 0;
            s_own_netmask = 0;
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

        case WCMD_DISCONNECT:
            s_wifi_auto_reconnect = false;
            esp_wifi_disconnect();
            s_wifi_connected = false;
            s_own_ip = 0;
            s_own_netmask = 0;
            break;

        case WCMD_SEND_ETH_RAW: {
            int eth_err = esp_wifi_internal_tx(WIFI_IF_STA, cmd.send_eth.buf, cmd.send_eth.len);
            if(eth_err != 0) {
                static uint32_t eth_err_count = 0;
                static uint32_t eth_err_last_log = 0;
                eth_err_count++;
                if(eth_err_count - eth_err_last_log >= 20) {
                    eth_err_last_log = eth_err_count;
                    ESP_LOGW(TAG, "internal_tx err=%d (count=%lu)",
                        eth_err, (unsigned long)eth_err_count);
                }
            }
            free(cmd.send_eth.buf);
            break;
        }

        case WCMD_SET_CHANNEL:
            esp_wifi_set_channel(cmd.set_channel.channel, WIFI_SECOND_CHAN_NONE);
            break;

        case WCMD_SET_PROMISC:
            // CB immer setzen (auch auf NULL), sonst bleibt ein zuvor
            // installierter RX-Callback aus einem anderen Subsystem aktiv.
            esp_wifi_set_promiscuous_rx_cb(cmd.set_promisc.enable ? cmd.set_promisc.cb : NULL);
            esp_wifi_set_promiscuous(cmd.set_promisc.enable);
            break;

        case WCMD_SEND_RAW: {
            // en_sys_seq=true: System füllt die Sequence-Number selbst → keine
            // duplizierten Frames mit fixem Seq aus dem Template.
            esp_err_t tx_err = esp_wifi_80211_tx(
                WIFI_IF_STA, cmd.send_raw.buf, cmd.send_raw.len, true);
            if(tx_err != ESP_OK) {
                ESP_LOGD(TAG, "80211_tx: %s", esp_err_to_name(tx_err));
            }
            break;
        }

        case WCMD_SCAN: {
            err = esp_wifi_scan_start(cmd.scan.config, true);
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

        case WCMD_RUN_FN:
            if(cmd.run_fn.fn) cmd.run_fn.fn(cmd.run_fn.arg);
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

static bool wlan_ensure_worker(void) {
    if(s_worker_task) return true;

    s_cmd_queue = xQueueCreate(4, sizeof(WlanCmd));
    if(!s_cmd_queue) return false;

    s_worker_stack = heap_caps_malloc(
        WLAN_HAL_WORKER_STACK * sizeof(StackType_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if(!s_worker_stack) {
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
        ESP_LOGE(TAG, "Cannot alloc worker stack");
        return false;
    }

    s_worker_task = xTaskCreateStaticPinnedToCore(
        wlan_worker_fn, "WlanWorker", WLAN_HAL_WORKER_STACK,
        NULL, 5, s_worker_stack, &s_worker_buf, 0);
    return s_worker_task != NULL;
}

static void wlan_send_cmd_sync(WlanCmd* cmd) {
    volatile bool done = false;
    cmd->done = &done;
    xQueueSend(s_cmd_queue, cmd, portMAX_DELAY);
    while(!done) {
        furi_delay_ms(10);
    }
}

bool wlan_hal_start(void) {
    if(s_started) return true;

    Bt* bt = furi_record_open(RECORD_BT);
    s_bt_was_on = bt_is_enabled(bt);
    if(s_bt_was_on) {
        bt_stop_stack(bt);
    }
    furi_record_close(RECORD_BT);

    if(!wlan_ensure_worker()) return false;

    volatile bool result = false;
    WlanCmd cmd = {.type = WCMD_INIT_START, .result = &result};
    wlan_send_cmd_sync(&cmd);

    if(result) {
        s_started = true;
        ESP_LOGI(TAG, "WiFi started");
    }
    return result;
}

void wlan_hal_stop(void) {
    if(s_started) {
        WlanCmd cmd = {.type = WCMD_STOP_DEINIT};
        wlan_send_cmd_sync(&cmd);
        s_started = false;
        ESP_LOGI(TAG, "WiFi stopped");
    }

    if(s_bt_was_on) {
        Bt* bt = furi_record_open(RECORD_BT);
        bt_start_stack(bt);
        furi_record_close(RECORD_BT);
        s_bt_was_on = false;
    }
}

bool wlan_hal_is_started(void) {
    return s_started;
}

bool wlan_hal_connect(const char* ssid, const char* password, const uint8_t* bssid, uint8_t channel) {
    if(!s_started || !ssid) return false;

    WlanCmd cmd = {.type = WCMD_CONNECT};
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
    wlan_send_cmd_sync(&cmd);
    return result;
}

void wlan_hal_disconnect(void) {
    if(!s_started) return;
    WlanCmd cmd = {.type = WCMD_DISCONNECT};
    wlan_send_cmd_sync(&cmd);
}

bool wlan_hal_is_connected(void) {
    return s_wifi_connected;
}

uint32_t wlan_hal_get_own_ip(void) {
    return s_own_ip;
}

uint32_t wlan_hal_get_netmask(void) {
    return s_own_netmask;
}

bool wlan_hal_get_own_mac(uint8_t out[6]) {
    if(!s_started) return false;
    return esp_wifi_get_mac(WIFI_IF_STA, out) == ESP_OK;
}

uint32_t wlan_hal_get_gw_ip(void) {
    if(!s_netif_sta) return 0;
    esp_netif_ip_info_t info;
    if(esp_netif_get_ip_info(s_netif_sta, &info) != ESP_OK) return 0;
    return info.gw.addr;
}

bool wlan_hal_send_eth_raw(const uint8_t* data, uint16_t len) {
    if(!s_started || !s_cmd_queue) {
        ESP_LOGW(TAG, "send_eth_raw: not started (s=%d q=%p)", s_started, s_cmd_queue);
        return false;
    }
    if(!data || len < 14 || len > 1600) return false;
    uint8_t* buf = malloc(len);
    if(!buf) {
        ESP_LOGE(TAG, "send_eth_raw: malloc(%u) failed", (unsigned)len);
        return false;
    }
    memcpy(buf, data, len);
    WlanCmd cmd = {.type = WCMD_SEND_ETH_RAW, .done = NULL, .result = NULL};
    cmd.send_eth.buf = buf;
    cmd.send_eth.len = len;
    if(xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE) {
        static uint32_t qfull_count = 0;
        static uint32_t qfull_last_log = 0;
        qfull_count++;
        if(qfull_count - qfull_last_log >= 20) {
            qfull_last_log = qfull_count;
            ESP_LOGW(TAG, "send_eth_raw: cmd queue full (count=%lu)",
                (unsigned long)qfull_count);
        }
        free(buf);
        return false;
    }
    return true;
}

void wlan_hal_set_channel(uint8_t channel) {
    if(!s_started || channel < 1 || channel > 14) return;
    WlanCmd cmd = {.type = WCMD_SET_CHANNEL, .set_channel = {.channel = channel}};
    wlan_send_cmd_sync(&cmd);
}

void wlan_hal_set_promiscuous(bool enable, wifi_promiscuous_cb_t cb) {
    if(!s_started) return;
    WlanCmd cmd = {.type = WCMD_SET_PROMISC, .set_promisc = {.enable = enable, .cb = cb}};
    wlan_send_cmd_sync(&cmd);
}

bool wlan_hal_send_raw(const uint8_t* data, uint16_t len) {
    if(!s_started || !s_cmd_queue || len > 64) return false;
    WlanCmd cmd = {.type = WCMD_SEND_RAW, .done = NULL, .result = NULL};
    memcpy(cmd.send_raw.buf, data, len);
    cmd.send_raw.len = len;
    return xQueueSend(s_cmd_queue, &cmd, 0) == pdTRUE;
}

bool wlan_hal_run_in_worker(WlanHalWorkerFn fn, void* arg) {
    if(!fn || !s_cmd_queue) return false;
    WlanCmd cmd = {.type = WCMD_RUN_FN, .run_fn = {.fn = fn, .arg = arg}};
    wlan_send_cmd_sync(&cmd);
    return true;
}

void wlan_hal_scan(wifi_ap_record_t** out_records, uint16_t* out_count, uint16_t max_count) {
    if(!s_started) {
        if(out_records) *out_records = NULL;
        *out_count = 0;
        return;
    }
    wifi_scan_config_t scan_config = {
        .ssid = NULL, .bssid = NULL, .channel = 0,
        .show_hidden = true, .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    WlanCmd cmd = {
        .type = WCMD_SCAN,
        .scan = {
            .config = &scan_config,
            .out_records = out_records,
            .out_count = out_count,
            .max_count = max_count,
        },
    };
    wlan_send_cmd_sync(&cmd);
}

// ---------------------------------------------------------------------------
// Beacon-Spam: dedizierter xTaskCreate-Task (parallel zum Worker), nutzt
// esp_wifi_80211_tx() direkt. Frame-Counter + Stop-Flag sind volatile.
// ---------------------------------------------------------------------------

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
    0x00, 0x00, 0x0f, 0xac, 0x02, 0x00, 0x00,
};

static const char* beacon_funny_ssids[] = {
    "Mom Use This One", "Abraham Linksys", "Benjamin FrankLAN",
    "Martin Router King", "John Wilkes Bluetooth", "Pretty Fly for a Wi-Fi",
    "Bill Wi the Science Fi", "I Believe Wi Can Fi", "Tell My Wi-Fi Love Her",
    "No More Mister Wi-Fi", "LAN Solo", "The LAN Before Time",
    "Silence of the LANs", "House LANister", "Winternet Is Coming",
    "FBI Surveillance Van 4", "Area 51 Test Site", "Never Gonna Give You Up",
    "Loading...", "VIRUS.EXE", "Free Public Wi-Fi", "404 Wi-Fi Unavailable",
    NULL,
};

static const char* beacon_rickroll_ssids[] = {
    "01 Never gonna give you up",
    "02 Never gonna let you down",
    "03 Never gonna run around",
    "04 And desert you",
    "05 Never gonna make you cry",
    "06 Never gonna say goodbye",
    "07 Never gonna tell a lie",
    "08 And hurt you",
    NULL,
};

static volatile bool s_beacon_active = false;
static volatile uint32_t s_beacon_frames = 0;
static TaskHandle_t s_beacon_task = NULL;
static WlanHalBeaconMode s_beacon_mode = WlanHalBeaconModeFunny;
static char s_beacon_base_ssid[33] = {0};

static void prepare_beacon_packet(uint8_t* packet, const uint8_t* mac,
                                  const char* ssid, uint8_t channel) {
    memcpy(packet, beacon_packet_template, sizeof(beacon_packet_template));
    memcpy(&packet[10], mac, 6);
    memcpy(&packet[16], mac, 6);
    uint8_t ssid_len = (uint8_t)strlen(ssid);
    if(ssid_len > 32) ssid_len = 32;
    packet[37] = ssid_len;
    memcpy(&packet[38], ssid, ssid_len);
    packet[82] = channel;
}

static void beacon_spam_task(void* param) {
    (void)param;
    const char** ssid_list = NULL;
    char gen_ssid[64];
    uint8_t mac[6];
    uint8_t packet[sizeof(beacon_packet_template)];
    int ssid_index = 0;
    int counter = 1;
    uint8_t channel = 1;

    srand((unsigned)esp_log_timestamp());

    // Promiscuous (cb=NULL) erlaubt 80211_tx auf STA-Interface.
    esp_wifi_set_promiscuous(true);

    while(s_beacon_active) {
        const char* current_ssid = NULL;
        switch(s_beacon_mode) {
        case WlanHalBeaconModeFunny:
            ssid_list = beacon_funny_ssids;
            current_ssid = ssid_list[ssid_index++];
            if(ssid_list[ssid_index] == NULL) ssid_index = 0;
            break;
        case WlanHalBeaconModeRickroll:
            ssid_list = beacon_rickroll_ssids;
            current_ssid = ssid_list[ssid_index++];
            if(ssid_list[ssid_index] == NULL) ssid_index = 0;
            break;
        case WlanHalBeaconModeRandom:
            snprintf(gen_ssid, sizeof(gen_ssid), "SSID_%d", rand() % 9999);
            current_ssid = gen_ssid;
            break;
        case WlanHalBeaconModeCustom:
            if(s_beacon_base_ssid[0]) {
                snprintf(gen_ssid, sizeof(gen_ssid), "%s%d",
                    s_beacon_base_ssid, counter++);
                if(counter > 9999) counter = 1;
            } else {
                snprintf(gen_ssid, sizeof(gen_ssid), "SSID_%d", rand() % 9999);
            }
            current_ssid = gen_ssid;
            break;
        }
        if(!current_ssid) current_ssid = "Flipper";

        // Random Locally-Administered Unicast MAC.
        for(int i = 0; i < 6; i++) mac[i] = rand() & 0xFF;
        mac[0] = (mac[0] & 0xFE) | 0x02;

        prepare_beacon_packet(packet, mac, current_ssid, channel);
        if(esp_wifi_80211_tx(WIFI_IF_STA, packet,
               sizeof(beacon_packet_template), false) == ESP_OK) {
            s_beacon_frames++;
        }

        if((s_beacon_frames % 5) == 0) {
            channel++;
            if(channel > 11) channel = 1;
            esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    esp_wifi_set_promiscuous(false);
    s_beacon_task = NULL;
    vTaskDelete(NULL);
}

void wlan_hal_beacon_spam_start(WlanHalBeaconMode mode, const char* base_ssid) {
    if(s_beacon_active || s_beacon_task) return;
    if(!s_started) {
        if(!wlan_hal_start()) return;
    }
    if(s_wifi_connected) wlan_hal_disconnect();

    s_beacon_mode = mode;
    memset(s_beacon_base_ssid, 0, sizeof(s_beacon_base_ssid));
    if(mode == WlanHalBeaconModeCustom && base_ssid) {
        strncpy(s_beacon_base_ssid, base_ssid, sizeof(s_beacon_base_ssid) - 1);
    }
    s_beacon_frames = 0;
    s_beacon_active = true;
    BaseType_t rc = xTaskCreate(beacon_spam_task, "BeaconSpam",
        4096, NULL, 5, &s_beacon_task);
    if(rc != pdPASS) {
        s_beacon_active = false;
        s_beacon_task = NULL;
        ESP_LOGE(TAG, "beacon_spam: xTaskCreate failed");
    }
}

void wlan_hal_beacon_spam_stop(void) {
    if(!s_beacon_active && !s_beacon_task) return;
    s_beacon_active = false;
    // Task beendet sich selbst (vTaskDelete im Task). Auf Beendigung warten.
    while(s_beacon_task) vTaskDelay(pdMS_TO_TICKS(10));
}

bool wlan_hal_beacon_spam_is_running(void) {
    return s_beacon_active;
}

uint32_t wlan_hal_beacon_spam_get_frame_count(void) {
    return s_beacon_frames;
}
