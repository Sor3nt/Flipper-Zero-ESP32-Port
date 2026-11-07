#include "wlan_hal.h"

#include <esp_wifi.h>
#include <furi_hal_power.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>
#include <esp_private/wifi.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <esp_sntp.h>
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
// Worker macht esp_wifi/lwIP-Calls; 4096 Words (16 KB) statt 8192 (32 KB),
// um internes RAM für esp_wifi_init zu sparen. Bei Stack-Overflow erhöhen.
#define WLAN_HAL_WORKER_STACK 4096

// ============================================================================
// TIMEOUT CONSTANTS — Fix caller stuck selamanya
// ============================================================================
#define WLAN_CMD_SYNC_TIMEOUT_MS    5000U    // 5 detik untuk cmd umum
#define WLAN_SCAN_CMD_TIMEOUT_MS    10000U   // 10 detik untuk scan
#define WLAN_RUNFN_TIMEOUT_MS       20000U   // 20 detik: run_fn macht teils viel
                                             // (Evil-Portal-Start etc.), großzügig
#define WLAN_SCAN_CANCEL_MAX_RETRY  20U      // 20 × 100ms = 2 detik
#define WLAN_SCAN_NORMAL_MAX_TICKS  500U     // 500 × 10ms = 5 detik

// FIX: queue depth 32 untuk handle burst TX cepat
#define WLAN_CMD_QUEUE_DEPTH        32U

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
    WCMD_SET_AP_MODE,
    WCMD_RUN_FN,
    WCMD_QUIT,
} WlanCmdType;

// RUN_FN handshake state (single atomic, CAS'd by worker and caller). Exactly
// one side transitions it out of Pending, so the worker running fn(arg) and the
// caller abandoning arg (its own stack) can never overlap.
typedef enum {
    WlanRunFnPending = 0, // calloc default: neither side has committed
    WlanRunFnStarted,     // worker is running fn (arg must stay alive)
    WlanRunFnAbandoned,   // caller gave up on timeout (worker must skip)
} WlanRunFnState;

// Heap-alloziertes Completion-Token für synchrone Commands. Refcount = 2
// (Caller + Worker); wer zuletzt released, gibt frei (und befreit ggf. noch
// nicht abgeholte Scan-Records). So überlebt es einen abgebrochenen Caller.
typedef struct {
    volatile bool done;        // Worker fertig
    volatile bool abandoned;   // Caller hat per Timeout aufgegeben (SCAN)
    int runfn_state;           // RUN_FN handshake (WlanRunFnState), CAS-only
    bool result;               // generisches ok/fail des Commands
    wifi_ap_record_t* scan_records; // Scan-Output (Worker-malloc, Caller übernimmt)
    uint16_t scan_count;
    volatile int refs;
} WlanCmdSync;

// Forward-Decl: der Worker (weiter unten) released das Token, die Definition
// steht aber erst nach dem Worker.
static void wlan_sync_release(WlanCmdSync* s);

typedef struct {
    WlanCmdType type;
    // Survey-owned commands run even while a survey owns the radio; every
    // other command is rejected by the worker until the survey ends.
    bool survey_owned;
    union {
        struct {
            // Config BY VALUE (nicht als Pointer auf Caller-Stack): bei einem
            // Sync-Timeout darf der Worker die Config nicht aus totem Stack lesen.
            wifi_scan_config_t config;
            uint16_t max_count;
            // cancel zeigt immer auf stabilen Speicher (static/heap) oder NULL —
            // niemals auf einen Stack der abgebrochen werden kann.
            const volatile bool* cancel;
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
            uint8_t buf[256];
            uint16_t len;
        } send_raw;
        struct {
            WlanHalWorkerFn fn;
            void* arg;
        } run_fn;
        struct {
            bool enable;
        } set_ap_mode;
    };
    // Heap-Completion-Token (NULL = fire-and-forget). Ersetzt die früheren
    // done/result-Pointer-auf-Caller-Stack: der Worker schreibt AUSSCHLIESSLICH
    // in dieses Token, nie in Caller-Speicher. Bei Sync-Timeout markiert der
    // Caller das Token als abandoned und gibt frei; der Worker fasst danach
    // keinen Caller-Stack mehr an → kein Use-after-Stack mehr.
    WlanCmdSync* sync;
} WlanCmd;

static bool s_sntp_started = false;
static bool s_started = false;
// SoftAP interface currently up (raw management-frame TX). Default off: normal
// station use never exposes an AP. Written only by the worker thread.
static volatile bool s_ap_mode = false;
static bool s_light_sleep_inhibited = false;
static FuriMutex* s_connection_sleep_inhibit_mutex = NULL;
static bool s_connection_sleep_inhibited = false;
static bool s_bt_was_on = false;
static bool s_netif_inited = false;
static esp_netif_t* s_netif_sta = NULL;
static esp_netif_t* s_netif_ap = NULL;
static volatile bool s_wifi_connected = false;
static volatile bool s_wifi_auto_reconnect = false;
static volatile bool s_auth_fail_latched = false;

/** true für Disconnect-Reasons, die auf ein falsches Passwort / fehlgeschlagene
 *  Authentifizierung hindeuten. Bewusst OHNE „AP nicht gefunden"/Beacon-Timeout,
 *  damit ein korrektes gespeichertes Passwort bei einem bloßen Ausfall bleibt. */
static bool wlan_reason_is_auth(uint8_t reason) {
    switch(reason) {
    case WIFI_REASON_AUTH_EXPIRE: // 2
    case WIFI_REASON_MIC_FAILURE: // 14
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: // 15 – klassisch: falsches WPA2-Passwort
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
static bool s_event_handlers_registered = false;
// Serialize set_channel against the deauth/scan apps that lock the channel.
// Reset on every start/stop so a missed unlock can never wedge it for the
// rest of the session.
static volatile bool s_channel_locked = false;

// ----- Exclusive radio survey (WIFITE etc.) -----
// A survey temporarily owns the whole radio: normal Wi-Fi is suspended and
// Bluetooth is stopped, then both are restored on end.
static volatile bool s_survey_active = false;
static bool s_survey_bt_restore = false;   // restart BT on end
static bool s_survey_resume_wifi = false;  // restart normal Wi-Fi on end
static bool s_survey_inhibited = false;    // survey's own light-sleep inhibit
static const char* s_survey_status = "RADIO READY";
static wifi_config_t s_survey_sta_config;
static bool s_survey_sta_config_valid = false;
static bool s_survey_sta_reconnect = false;
static bool s_survey_auth_fail_latched = false;
static esp_err_t s_survey_sta_error = ESP_OK;

static void wlan_set_connection_sleep_inhibited(bool inhibited);

static void survey_save_station(void* unused) {
    UNUSED(unused);
    wifi_mode_t mode = WIFI_MODE_NULL;
    s_survey_sta_config_valid = false;
    s_survey_sta_error = esp_wifi_get_mode(&mode);
    if(s_survey_sta_error != ESP_OK || mode != WIFI_MODE_STA) return;

    s_survey_sta_error = esp_wifi_get_config(WIFI_IF_STA, &s_survey_sta_config);
    s_survey_sta_config_valid = s_survey_sta_error == ESP_OK;
    s_survey_sta_reconnect = s_wifi_connected || s_wifi_auto_reconnect;
    s_survey_auth_fail_latched = s_auth_fail_latched;
}

static void survey_restore_station(void* unused) {
    UNUSED(unused);
    s_survey_sta_error = esp_wifi_set_config(WIFI_IF_STA, &s_survey_sta_config);
    if(s_survey_sta_error != ESP_OK) return;

    s_auth_fail_latched = s_survey_auth_fail_latched;
    s_wifi_auto_reconnect = s_survey_sta_reconnect;
    if(s_survey_sta_reconnect) {
        wlan_set_connection_sleep_inhibited(true);
        s_survey_sta_error = esp_wifi_connect();
        if(s_survey_sta_error != ESP_OK) {
            s_wifi_auto_reconnect = false;
            wlan_set_connection_sleep_inhibited(false);
        }
    }
}

const char* wlan_hal_survey_status(void) {
    return __atomic_load_n(&s_survey_status, __ATOMIC_ACQUIRE);
}
static void survey_status(const char* status) {
    __atomic_store_n(&s_survey_status, status, __ATOMIC_RELEASE);
    ESP_LOGI(TAG, "survey: %s", status);
}
static bool survey_active(void) {
    return __atomic_load_n(&s_survey_active, __ATOMIC_ACQUIRE);
}

static bool survey_resume_normal_wifi(void) {
    if(!s_survey_resume_wifi) return true;
    s_survey_resume_wifi = false;

    bool ok = wlan_hal_start();
    if(ok && s_survey_sta_config_valid) {
        const bool worker_ok = wlan_hal_run_in_worker(survey_restore_station, NULL);
        ok = worker_ok && s_survey_sta_error == ESP_OK;
        if(!ok) {
            ESP_LOGE(TAG, "Failed to restore station config after survey: %s",
                     esp_err_to_name(worker_ok ? s_survey_sta_error : ESP_FAIL));
        }
    }

    memset(&s_survey_sta_config, 0, sizeof(s_survey_sta_config));
    s_survey_sta_config_valid = false;
    s_survey_sta_reconnect = false;
    s_survey_auth_fail_latched = false;
    s_survey_sta_error = ESP_OK;
    return ok;
}
static volatile uint32_t s_own_ip = 0;
static volatile uint32_t s_own_netmask = 0;

static bool wlan_connection_sleep_inhibit_mutex_init(void) {
    static portMUX_TYPE init_lock = portMUX_INITIALIZER_UNLOCKED;
    if(s_connection_sleep_inhibit_mutex) return true;

    FuriMutex* candidate = furi_mutex_alloc(FuriMutexTypeNormal);
    if(!candidate) return false;

    portENTER_CRITICAL(&init_lock);
    if(!s_connection_sleep_inhibit_mutex) {
        s_connection_sleep_inhibit_mutex = candidate;
    }
    FuriMutex* installed = s_connection_sleep_inhibit_mutex;
    portEXIT_CRITICAL(&init_lock);

    if(installed != candidate) furi_mutex_free(candidate);
    return true;
}

/* Wi-Fi event callbacks and worker commands may change this concurrently. */
static void wlan_set_connection_sleep_inhibited(bool inhibited) {
    furi_check(s_connection_sleep_inhibit_mutex);
    furi_check(
        furi_mutex_acquire(s_connection_sleep_inhibit_mutex, FuriWaitForever) == FuriStatusOk);
    if(s_connection_sleep_inhibited != inhibited) {
        if(inhibited) {
            furi_hal_power_light_sleep_inhibit_enter();
        } else {
            furi_hal_power_light_sleep_inhibit_exit();
        }
        s_connection_sleep_inhibited = inhibited;
    }
    furi_check(furi_mutex_release(s_connection_sleep_inhibit_mutex) == FuriStatusOk);
}

static QueueHandle_t s_cmd_queue = NULL;
static TaskHandle_t s_worker_task = NULL;
static StackType_t* s_worker_stack = NULL;
static StaticTask_t s_worker_buf;

static void wlan_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    UNUSED(arg);
    if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        if(s_worker_task) xTaskNotifyGive(s_worker_task);
    } else if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* d = (wifi_event_sta_disconnected_t*)event_data;
        if(d) {
            ESP_LOGW(TAG, "STA disconnected: reason=%u (ssid_len=%u)", (unsigned)d->reason, (unsigned)d->ssid_len);
            if(wlan_reason_is_auth((uint8_t)d->reason)) s_auth_fail_latched = true;
        }
        s_wifi_connected = false;
        s_own_ip = 0;
        s_own_netmask = 0;
        if(!s_wifi_auto_reconnect) {
            wlan_set_connection_sleep_inhibited(false);
        }
        if(s_wifi_auto_reconnect && !survey_active()) {
            esp_wifi_connect();
        }
    } else if(event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        s_own_ip = event->ip_info.ip.addr;
        s_own_netmask = event->ip_info.netmask.addr;
        s_wifi_connected = true;
        wlan_set_connection_sleep_inhibited(true);
        // Sync the RTC via SNTP once we have an IP. Without a correct clock
        // NTLMv2 (SMB) and anything else time-sensitive uses ~1970. UTC is
        // fine for these uses. Harmless if the network has no internet (the
        // SMB code also falls back to the server-supplied timestamp).
        if(!s_sntp_started) {
            esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "pool.ntp.org");
            esp_sntp_init();
            s_sntp_started = true;
            ESP_LOGI(TAG, "SNTP started (pool.ntp.org)");
        }
    }
}

// Minimal SoftAP used only so WIFI_IF_AP is up for esp_wifi_80211_tx (raw
// management frames). Hidden SSID + slow beacon + single client: it is not
// meant to serve clients, and it is brought up only for raw-TX features.
static void wlan_apply_softap_config(void) {
    wifi_config_t ap_cfg = {0};
    const char* dummy_ssid = "x";
    strncpy((char*)ap_cfg.ap.ssid, dummy_ssid, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = strlen(dummy_ssid);
    ap_cfg.ap.ssid_hidden = 1; // do not advertise the SSID in beacons
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    ap_cfg.ap.max_connection = 1; // 0 is out of IDF's valid range
    ap_cfg.ap.beacon_interval = 60000;
    ap_cfg.ap.pmf_cfg.required = false;
    const esp_err_t e = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if(e != ESP_OK) ESP_LOGW(TAG, "AP config: %s", esp_err_to_name(e));
}

static void wlan_worker_fn(void* arg) {
    UNUSED(arg);
    ESP_LOGI(TAG, "Worker started");

    WlanCmd cmd;
    while(1) {
        if(xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) continue;

        // Never run another app's scan/connect/transmit while a survey owns
        // the radio. Complete the token as failed so the caller unblocks.
        if(survey_active() && !cmd.survey_owned) {
            if(cmd.type == WCMD_SEND_ETH_RAW) free(cmd.send_eth.buf);
            if(cmd.sync) {
                cmd.sync->result = false;
                __atomic_store_n(&cmd.sync->done, true, __ATOMIC_RELEASE);
                wlan_sync_release(cmd.sync);
            }
            continue;
        }

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
                // FIX: create AP netif — wajib untuk APSTA mode
                s_netif_ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
                if(!s_netif_ap) {
                    s_netif_ap = esp_netif_create_default_wifi_ap();
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

            // Normal Wi-Fi use is station-only: no SoftAP is exposed. The AP
            // interface is brought up for raw management-frame TX (deauth /
            // beacon / ...) only when a survey owns the radio (wifite), or on
            // demand via wlan_hal_set_ap_mode() (see WCMD_SET_AP_MODE).
            if(cmd.survey_owned) {
                esp_wifi_set_mode(WIFI_MODE_APSTA);
                wlan_apply_softap_config();
                s_ap_mode = true;
            } else {
                esp_wifi_set_mode(WIFI_MODE_STA);
                s_ap_mode = false;
            }

            err = esp_wifi_start();
            if(err != ESP_OK) {
                ESP_LOGE(TAG, "wifi_start: %s", esp_err_to_name(err));
                esp_wifi_deinit();
                ok = false;
                break;
            }

            if(s_ap_mode) {
                // Let the AP interface settle before the first raw TX.
                vTaskDelay(pdMS_TO_TICKS(100));
            }

            ESP_LOGI(TAG, "WiFi started (%s)", s_ap_mode ? "APSTA" : "STA");
            break;

        case WCMD_STOP_DEINIT:
            s_wifi_auto_reconnect = false;
            wlan_set_connection_sleep_inhibited(false);
            esp_wifi_disconnect();
            s_wifi_connected = false;
            s_own_ip = 0;
            s_own_netmask = 0;
            esp_wifi_set_promiscuous(false);
            esp_wifi_stop();
            esp_wifi_deinit();
            s_ap_mode = false;
            break;

        case WCMD_CONNECT: {
            // A normal station connect must never run with the SoftAP exposed.
            if(s_ap_mode) {
                esp_wifi_set_mode(WIFI_MODE_STA);
                s_ap_mode = false;
            }
            s_wifi_auto_reconnect = false;
            wlan_set_connection_sleep_inhibited(false);
            esp_wifi_disconnect();
            s_wifi_connected = false;
            s_own_ip = 0;
            s_own_netmask = 0;
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
            wlan_set_connection_sleep_inhibited(true);
            esp_wifi_set_config(WIFI_IF_STA, &wcfg);
            err = esp_wifi_connect();
            if(err != ESP_OK) {
                ESP_LOGE(TAG, "connect: %s", esp_err_to_name(err));
                s_wifi_auto_reconnect = false;
                wlan_set_connection_sleep_inhibited(false);
                ok = false;
            }
            break;
        }

        case WCMD_DISCONNECT:
            s_wifi_auto_reconnect = false;
            wlan_set_connection_sleep_inhibited(false);
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
            // FIX KRITIS: pakai WIFI_IF_AP (bukan STA) + en_sys_seq=false
            // untuk mengikuti strategi Bruce. TX path AP lebih permisif,
            // dan sequence dari template kita dipakai (bukan di-override driver).
            esp_err_t tx_err = ESP_FAIL;
            for(uint8_t attempt = 0; attempt < 8; attempt++) {
                tx_err = esp_wifi_80211_tx(
                    WIFI_IF_AP, cmd.send_raw.buf, cmd.send_raw.len, false);
                if(tx_err != ESP_ERR_NO_MEM) break;
                vTaskDelay(1);
            }
            if(tx_err != ESP_OK) {
                ESP_LOGD(TAG, "80211_tx(AP): %s", esp_err_to_name(tx_err));
            }
            break;
        }

        case WCMD_SCAN: {
            // Caller hat schon aufgegeben, bevor wir überhaupt starten
            // (Queue war gestaut) → Scan überspringen.
            if(cmd.sync && __atomic_load_n(&cmd.sync->abandoned, __ATOMIC_ACQUIRE)) {
                ok = false;
                break;
            }
            if(cmd.scan.cancel &&
               __atomic_load_n(cmd.scan.cancel, __ATOMIC_ACQUIRE)) {
                ok = false;
                break;
            }

            // FIX: drain notification + delay untuk tutup race window
            // antara scan lama dan scan baru
            while(ulTaskNotifyTake(pdTRUE, 0) > 0) {
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            while(ulTaskNotifyTake(pdTRUE, 0) > 0) {
            }

            err = esp_wifi_scan_start(&cmd.scan.config, false);
            if(err != ESP_OK) {
                ESP_LOGE(TAG, "scan: %s", esp_err_to_name(err));
                ok = false;
                break;
            }

            bool scan_done = false;
            bool cancelled = false;
            uint32_t normal_ticks = 0;

            while(!scan_done) {
                if(cmd.scan.cancel &&
                   __atomic_load_n(cmd.scan.cancel, __ATOMIC_ACQUIRE)) {
                    cancelled = true;
                    err = esp_wifi_scan_stop();
                    bool stop_succeeded = err == ESP_OK;
                    uint32_t stop_retries = 0;
                    if(!stop_succeeded) {
                        ESP_LOGW(TAG, "scan cancel stop: %s", esp_err_to_name(err));
                    }

                    // FIX: BOUNDED retry max 2 detik
                    while(!scan_done && stop_retries < WLAN_SCAN_CANCEL_MAX_RETRY) {
                        scan_done = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100)) > 0;
                        stop_retries++;
                        if(scan_done || stop_succeeded) continue;

                        err = esp_wifi_scan_stop();
                        stop_succeeded = err == ESP_OK;
                        if(stop_succeeded) {
                            ESP_LOGI(TAG, "scan cancel stop on retry");
                        } else if(stop_retries % 5 == 0) {
                            ESP_LOGW(TAG, "scan cancel retry #%lu: %s",
                                     (unsigned long)stop_retries, esp_err_to_name(err));
                        }
                    }

                    if(!scan_done) {
                        ESP_LOGE(TAG, "scan cancel timeout — force exit");
                    }
                    break;
                }

                scan_done = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10)) > 0;

                // FIX: scan normal timeout 5 detik
                if(!scan_done && ++normal_ticks > WLAN_SCAN_NORMAL_MAX_TICKS) {
                    ESP_LOGE(TAG, "scan timeout (>5s) — forcing stop");
                    esp_wifi_scan_stop();
                    vTaskDelay(pdMS_TO_TICKS(50));
                    scan_done = true;
                }
            }

            if(cancelled) {
                // FIX: force clear scan state 2× setelah cancel
                esp_wifi_scan_stop();
                vTaskDelay(pdMS_TO_TICKS(30));
                esp_wifi_scan_stop();
                break;
            }

            uint16_t count = 0;
            wifi_ap_record_t* records = NULL;
            err = esp_wifi_scan_get_ap_num(&count);
            if(err != ESP_OK) {
                ESP_LOGW(TAG, "scan result count: %s", esp_err_to_name(err));
                ok = false;
                break;
            }
            if(count > cmd.scan.max_count) count = cmd.scan.max_count;
            if(count > 0) {
                records = malloc(count * sizeof(wifi_ap_record_t));
                if(records) {
                    err = esp_wifi_scan_get_ap_records(&count, records);
                    if(err != ESP_OK) {
                        ESP_LOGW(TAG, "scan records: %s", esp_err_to_name(err));
                        free(records);
                        records = NULL;
                        count = 0;
                        ok = false;
                    }
                } else {
                    count = 0;
                    ok = false;
                }
            }
            // In das Token stellen; die gemeinsame Completion setzt done. Der
            // Caller holt die Records nach done ab (oder das Token-Release gibt
            // sie frei, falls der Caller abgebrochen hat → kein Leak, kein UAF).
            if(cmd.sync) {
                cmd.sync->scan_records = records;
                cmd.sync->scan_count = count;
            } else if(records) {
                free(records); // Scan ist immer sync; defensiv.
            }
            break;
        }

        case WCMD_SET_AP_MODE:
            // Dynamic mode switch on the worker thread (serialized with all
            // other esp_wifi calls). Enables/disables the raw-TX SoftAP without
            // a full radio restart.
            if(cmd.set_ap_mode.enable) {
                esp_wifi_set_mode(WIFI_MODE_APSTA);
                wlan_apply_softap_config();
                s_ap_mode = true;
            } else {
                esp_wifi_set_mode(WIFI_MODE_STA);
                s_ap_mode = false;
            }
            break;

        case WCMD_RUN_FN: {
            // Claim the run before touching run_fn.arg: CAS Pending→Started.
            // If it fails, the caller already timed out and CAS'd Abandoned, so
            // arg points at a dead caller stack — skip the call. Exactly one of
            // worker/caller wins this CAS, so the two can never overlap (this
            // closes the use-after-stack window the old started/abandoned pair
            // left between its separate load and store).
            if(cmd.sync) {
                int expected = WlanRunFnPending;
                if(!__atomic_compare_exchange_n(
                       &cmd.sync->runfn_state, &expected, WlanRunFnStarted, false,
                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                    break; // caller abandoned → arg is dead
                }
            }
            if(cmd.run_fn.fn) cmd.run_fn.fn(cmd.run_fn.arg);
            break;
        }

        case WCMD_QUIT:
            ESP_LOGI(TAG, "Worker quitting");
            if(cmd.sync) {
                __atomic_store_n(&cmd.sync->done, true, __ATOMIC_RELEASE);
                wlan_sync_release(cmd.sync);
            }
            vTaskDelete(NULL);
            return;
        }

        // Gemeinsame Completion: NUR ins Heap-Token schreiben (nie Caller-Stack).
        // result zuerst, dann done mit Release-Semantik, dann unseren Ref lösen.
        if(cmd.sync) {
            cmd.sync->result = ok;
            __atomic_store_n(&cmd.sync->done, true, __ATOMIC_RELEASE);
            wlan_sync_release(cmd.sync);
        }
    }
}

static bool wlan_ensure_worker(void) {
    if(s_worker_task) return true;

    s_cmd_queue = xQueueCreate(WLAN_CMD_QUEUE_DEPTH, sizeof(WlanCmd));
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

bool wlan_hal_ensure_worker(void) {
    return wlan_ensure_worker();
}

// Token-Ref lösen; der letzte Releaser gibt frei (und befreit noch nicht
// abgeholte Scan-Records → kein Leak). Threadsafe via atomarem Refcount.
static void wlan_sync_release(WlanCmdSync* s) {
    if(!s) return;
    if(__atomic_sub_fetch(&s->refs, 1, __ATOMIC_ACQ_REL) == 0) {
        if(s->scan_records) free(s->scan_records);
        free(s);
    }
}

// Führt ein Command synchron aus. Return true = innerhalb timeout fertig.
// out_result/out_records/out_count (je optional) werden NUR bei Erfolg und
// NOCH im lebenden Caller-Frame aus dem Token kopiert — der Worker fasst
// Caller-Speicher nie an. Bei Timeout wird das Token als abandoned markiert
// und sauber per Refcount freigegeben (kein Use-after-Stack, kein Leak).
static bool wlan_cmd_exec(
    WlanCmd* cmd,
    uint32_t timeout_ms,
    bool* out_result,
    wifi_ap_record_t** out_records,
    uint16_t* out_count) {
    if(out_records) *out_records = NULL;
    if(out_count) *out_count = 0;
    if(out_result) *out_result = false;

    WlanCmdSync* s = calloc(1, sizeof(WlanCmdSync));
    if(!s) {
        ESP_LOGE(TAG, "cmd_exec: token OOM (cmd=%d)", (int)cmd->type);
        return false;
    }
    s->refs = 2; // Caller + Worker
    cmd->sync = s;

    if(xQueueSend(s_cmd_queue, cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "cmd_exec: queue full (cmd=%d)", (int)cmd->type);
        // Worker bekommt das Command nie → beide Refs gehören dem Caller.
        wlan_sync_release(s);
        wlan_sync_release(s);
        return false;
    }

    TickType_t start = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    bool completed = false;
    while(1) {
        if(__atomic_load_n(&s->done, __ATOMIC_ACQUIRE)) {
            completed = true;
            break;
        }
        if((xTaskGetTickCount() - start) > timeout_ticks) {
            // For RUN_FN, claim abandonment atomically: CAS Pending→Abandoned.
            // If it fails, the worker already CAS'd Started and is dereferencing
            // arg (often this caller's stack) — keep waiting for `done` rather
            // than returning into a frame the worker is about to use.
            if(cmd->type == WCMD_RUN_FN) {
                int expected = WlanRunFnPending;
                if(!__atomic_compare_exchange_n(
                       &s->runfn_state, &expected, WlanRunFnAbandoned, false,
                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue;
                }
            } else {
                __atomic_store_n(&s->abandoned, true, __ATOMIC_RELEASE);
            }
            ESP_LOGW(TAG, "cmd_exec: timeout %lums (cmd=%d)",
                     (unsigned long)timeout_ms, (int)cmd->type);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if(completed) {
        if(out_result) *out_result = s->result;
        if(out_records) {
            *out_records = s->scan_records;
            s->scan_records = NULL; // Ownership übernommen → Release gibt's nicht frei
        }
        if(out_count) *out_count = s->scan_count;
    }

    wlan_sync_release(s); // Caller-Ref
    return completed;
}

// Wrapper default — pakai timeout standar 5 detik, Ergebnis ignoriert.
static void wlan_send_cmd_sync(WlanCmd* cmd) {
    wlan_cmd_exec(cmd, WLAN_CMD_SYNC_TIMEOUT_MS, NULL, NULL, NULL);
}

// Survey-owned radio bring-up/-down. Keeps its own light-sleep inhibit and
// never touches Bluetooth (the survey begin/end does that once).
static bool survey_radio_start(void) {
    if(s_started) return true;
    if(!wlan_ensure_worker()) return false;
    if(!s_survey_inhibited) {
        furi_hal_power_light_sleep_inhibit_enter();
        s_survey_inhibited = true;
    }
    bool result = false;
    WlanCmd cmd = {.type = WCMD_INIT_START, .survey_owned = true};
    wlan_cmd_exec(&cmd, WLAN_CMD_SYNC_TIMEOUT_MS, &result, NULL, NULL);
    if(result) {
        s_started = true;
    } else if(s_survey_inhibited) {
        s_survey_inhibited = false;
        furi_hal_power_light_sleep_inhibit_exit();
    }
    return result;
}
static void survey_radio_stop(void) {
    if(s_started) {
        WlanCmd cmd = {.type = WCMD_STOP_DEINIT, .survey_owned = true};
        wlan_cmd_exec(&cmd, WLAN_CMD_SYNC_TIMEOUT_MS, NULL, NULL, NULL);
        s_started = false;
    }
    if(s_survey_inhibited) {
        s_survey_inhibited = false;
        furi_hal_power_light_sleep_inhibit_exit();
    }
    s_channel_locked = false;
}

bool wlan_hal_survey_begin(bool wifi) {
    if(survey_active() || wlan_hal_beacon_spam_is_running() ||
       wlan_hal_evil_portal_is_running()) {
        survey_status("OTHER RADIO APP ACTIVE");
        return false;
    }
    memset(&s_survey_sta_config, 0, sizeof(s_survey_sta_config));
    s_survey_sta_config_valid = false;
    s_survey_sta_reconnect = false;
    s_survey_auth_fail_latched = false;
    s_survey_sta_error = ESP_OK;

    // Suspend an existing normal Wi-Fi session first (before claiming the
    // radio, so wlan_hal_stop's survey guard does not reject it).
    s_survey_resume_wifi = s_started;
    if(s_started) {
        survey_status("SAVING WI-FI STATE");
        if(!wlan_hal_run_in_worker(survey_save_station, NULL) ||
           !s_survey_sta_config_valid) {
            s_survey_resume_wifi = false;
            survey_status("WI-FI STATE SAVE FAILED");
            return false;
        }
        survey_status("SUSPENDING WI-FI");
        wlan_hal_stop();
    }
    // Stop Bluetooth so the survey has the full radio.
    Bt* bt = furi_record_open(RECORD_BT);
    s_survey_bt_restore = bt_is_enabled(bt);
    if(s_survey_bt_restore) bt_stop_stack(bt);
    furi_record_close(RECORD_BT);

    __atomic_store_n(&s_survey_active, true, __ATOMIC_RELEASE);

    bool ok = true;
    if(wifi) {
        survey_status("STARTING EXCLUSIVE WI-FI");
        ok = survey_radio_start();
    }
    if(!ok) {
        __atomic_store_n(&s_survey_active, false, __ATOMIC_RELEASE);
        if(s_survey_bt_restore) {
            bt = furi_record_open(RECORD_BT);
            bt_start_stack(bt);
            furi_record_close(RECORD_BT);
            s_survey_bt_restore = false;
        }
        const bool restored = survey_resume_normal_wifi();
        survey_status(restored ? "WI-FI INIT FAILED" : "RADIO RESTORE FAILED");
        return false;
    }
    survey_status("EXCLUSIVE RADIO READY");
    return true;
}

bool wlan_hal_survey_run(WlanHalWorkerFn fn, void* arg) {
    if(!fn || !survey_active() || !s_cmd_queue) return false;
    WlanCmd cmd = {.type = WCMD_RUN_FN, .survey_owned = true,
                   .run_fn = {.fn = fn, .arg = arg}};
    // Returns true only if the worker actually ran fn to completion.
    return wlan_cmd_exec(&cmd, WLAN_RUNFN_TIMEOUT_MS, NULL, NULL, NULL);
}

void wlan_hal_survey_end(void) {
    if(!survey_active()) return;
    survey_status("STOPPING SURVEY RADIO");
    survey_radio_stop();
    if(s_survey_bt_restore) {
        Bt* bt = furi_record_open(RECORD_BT);
        bt_start_stack(bt);
        furi_record_close(RECORD_BT);
        s_survey_bt_restore = false;
    }
    // Release ownership before resuming normal Wi-Fi so its commands pass.
    __atomic_store_n(&s_survey_active, false, __ATOMIC_RELEASE);
    const bool restored = survey_resume_normal_wifi();
    survey_status(restored ? "RADIO RESTORED" : "RADIO RESTORE FAILED");
}

static void wlan_start_failed_cleanup(void) {
    if(s_light_sleep_inhibited) {
        s_light_sleep_inhibited = false;
        furi_hal_power_light_sleep_inhibit_exit();
    }
    if(s_bt_was_on) {
        Bt* bt = furi_record_open(RECORD_BT);
        bt_start_stack(bt);
        furi_record_close(RECORD_BT);
        s_bt_was_on = false;
    }
}

bool wlan_hal_start(void) {
    if(survey_active()) return false;
    if(s_started) return true;

    if(!wlan_connection_sleep_inhibit_mutex_init()) {
        ESP_LOGE(TAG, "Failed to allocate Wi-Fi sleep-inhibit mutex");
        return false;
    }

    /* FIX KRITIS: Light sleep inhibit harus tetap aktif SELAMA WiFi aktif,
     * bukan hanya saat init. Jika di-exit setelah init selesai, PM (Power
     * Management) akan wake up berkali-kali saat WiFi di background, memicu
     * phy_track_pll_init() berulang → timer leak → ESP_ERR_NO_MEM crash. */
    furi_hal_power_light_sleep_inhibit_enter();
    s_light_sleep_inhibited = true;

    Bt* bt = furi_record_open(RECORD_BT);
    s_bt_was_on = bt_is_enabled(bt);
    if(s_bt_was_on) {
        bt_stop_stack(bt);
    }
    furi_record_close(RECORD_BT);

    if(!wlan_ensure_worker()) {
        wlan_start_failed_cleanup();
        return false;
    }

    bool result = false;
    WlanCmd cmd = {.type = WCMD_INIT_START};
    wlan_cmd_exec(&cmd, WLAN_CMD_SYNC_TIMEOUT_MS, &result, NULL, NULL);

    if(result) {
        s_started = true;
        s_channel_locked = false;
        ESP_LOGI(TAG, "WiFi started (light sleep inhibited)");
        /* FIX: JANGAN exit inhibit di sini — biarkan aktif sampai wlan_hal_stop() */
    } else {
        /* Init failed: restore power management and the Bluetooth stack. */
        wlan_start_failed_cleanup();
    }
    return result;
}

void wlan_hal_stop(void) {
    if(survey_active()) return;

    s_channel_locked = false;
    if(s_started) {
        WlanCmd cmd = {.type = WCMD_STOP_DEINIT};
        wlan_send_cmd_sync(&cmd);
        s_started = false;
        ESP_LOGI(TAG, "WiFi stopped");
    }

    /* FIX: Exit light sleep inhibit hanya di stop — biarkan WiFi sepenuhnya
     * selesai dulu (termasuk timer cleanup di driver) sebelum restore PM. */
    if(s_light_sleep_inhibited) {
        s_light_sleep_inhibited = false;
        furi_hal_power_light_sleep_inhibit_exit();
    }

    if(s_bt_was_on) {
        Bt* bt = furi_record_open(RECORD_BT);
        bt_start_stack(bt);
        furi_record_close(RECORD_BT);
        s_bt_was_on = false;
    }
}

void wlan_hal_set_bt_restore(bool restore) {
    if(survey_active()) return;
    s_bt_was_on = restore;
}

bool wlan_hal_is_started(void) {
    return s_started;
}

bool wlan_hal_connect(const char* ssid, const char* password, const uint8_t* bssid, uint8_t channel) {
    if(survey_active() || !s_started || !ssid) return false;

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

    bool result = false;
    wlan_cmd_exec(&cmd, WLAN_CMD_SYNC_TIMEOUT_MS, &result, NULL, NULL);
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

typedef struct {
    wifi_ap_record_t* out;
    bool result;
} WlanGetApArg;

/* Läuft im wlan-Worker-Task: esp_wifi_sta_get_ap_info() ist ioctl-basiert und
 * nutzt pthread-TLS des WiFi-Treibers — ein Aufruf aus einem FuriThread (z.B.
 * der WiFi-App) crasht (LoadProhibited). Daher via wlan_hal_run_in_worker. */
static void wlan_get_ap_worker(void* p) {
    WlanGetApArg* a = p;
    a->result = (esp_wifi_sta_get_ap_info(a->out) == ESP_OK);
}

bool wlan_hal_get_connected_ap(wifi_ap_record_t* out) {
    if(!s_started || !s_wifi_connected || !out) return false;
    WlanGetApArg arg = {.out = out, .result = false};
    if(!wlan_hal_run_in_worker(wlan_get_ap_worker, &arg)) return false;
    return arg.result;
}

bool wlan_hal_last_fail_is_auth(void) {
    return s_auth_fail_latched;
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
        ESP_LOGW(TAG, "send_eth_raw: not started");
        return false;
    }
    if(!data || len < 14 || len > 1600) return false;
    uint8_t* buf = malloc(len);
    if(!buf) {
        ESP_LOGE(TAG, "send_eth_raw: malloc(%u) failed", (unsigned)len);
        return false;
    }
    memcpy(buf, data, len);
    WlanCmd cmd = {.type = WCMD_SEND_ETH_RAW, .sync = NULL};
    cmd.send_eth.buf = buf;
    cmd.send_eth.len = len;
    if(xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE) {
        static uint32_t qfull_count = 0;
        static uint32_t qfull_last_log = 0;
        qfull_count++;
        if(qfull_count - qfull_last_log >= 20) {
            qfull_last_log = qfull_count;
            ESP_LOGW(TAG, "send_eth_raw: queue full (count=%lu)", (unsigned long)qfull_count);
        }
        free(buf);
        return false;
    }
    return true;
}

// Channel lock — cegah channel hop selama attack
void wlan_hal_set_channel(uint8_t channel) {
    if(s_channel_locked) {
        ESP_LOGW(TAG, "set_channel(%u) BLOCKED", channel);
        return;
    }
    if(!s_started || channel < 1 || channel > 14) return;
    WlanCmd cmd = {.type = WCMD_SET_CHANNEL, .set_channel = {.channel = channel}};
    wlan_send_cmd_sync(&cmd);
}

void wlan_hal_set_promiscuous(bool enable, wifi_promiscuous_cb_t cb) {
    if(!s_started) return;
    WlanCmd cmd = {.type = WCMD_SET_PROMISC, .set_promisc = {.enable = enable, .cb = cb}};
    wlan_send_cmd_sync(&cmd);
}

// Bring the raw-TX SoftAP up or down. No-op while a survey owns the radio (it
// already runs APSTA). Called by the raw-TX helpers below the first time they
// need WIFI_IF_AP, so no feature scene has to manage the mode itself.
void wlan_hal_set_ap_mode(bool enable) {
    if(survey_active()) return;
    if(!s_started) {
        if(!enable) return;
        if(!wlan_hal_start()) return; // comes up station-only
    }
    if(s_ap_mode == enable) return;
    WlanCmd cmd = {.type = WCMD_SET_AP_MODE, .set_ap_mode = {.enable = enable}};
    wlan_send_cmd_sync(&cmd);
}

bool wlan_hal_send_raw(const uint8_t* data, uint16_t len) {
    if(!s_started || !s_cmd_queue || !data || len > 256) return false;
    if(!survey_active() && !s_ap_mode) wlan_hal_set_ap_mode(true);
    WlanCmd cmd = {.type = WCMD_SEND_RAW, .sync = NULL};
    memcpy(cmd.send_raw.buf, data, len);
    cmd.send_raw.len = len;
    return xQueueSend(s_cmd_queue, &cmd, 0) == pdTRUE;
}

bool wlan_hal_raw_tx_retry(const uint8_t* data, uint16_t len) {
    /* Serialize raw TX with scan, channel, and promiscuous operations. */
    if(!s_started || !s_cmd_queue || !data || len > 256) return false;
    if(!survey_active() && !s_ap_mode) wlan_hal_set_ap_mode(true);

    WlanCmd cmd = {.type = WCMD_SEND_RAW, .sync = NULL};
    memcpy(cmd.send_raw.buf, data, len);
    cmd.send_raw.len = len;
    // FIX: retry 8× pada queue full (Bruce-style)
    for(int attempt = 0; attempt < 8; attempt++) {
        if(xQueueSend(s_cmd_queue, &cmd, 0) == pdTRUE) return true;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return false;
}

// ============================================================================
// SMART DEAUTH SUPPORT — 5 FUNGSI BARU
// ============================================================================

// Direct TX — bypass queue, cegah context switch overhead
bool wlan_hal_raw_tx_direct(const uint8_t* data, uint16_t len) {
    // Guard: anders als die meisten wlan_hal_*-APIs läuft dies NICHT über den
    // wlan-Worker, sondern ruft esp_wifi_80211_tx direkt im Caller-Thread auf.
    // esp_wifi_*-ioctls aus einem FuriThread crashen hart (LoadProhibited) —
    // diese API darf nur aus einem echten xTaskCreate-Task kommen. Auf einem
    // FuriThread liefert furi_thread_get_current() != NULL → sauber abbrechen
    // statt crashen (z.B. versehentlicher Aufruf aus einem GUI-View-Callback).
    if(furi_thread_get_current()) {
        ESP_LOGE(
            TAG, "raw_tx_direct: refused (FuriThread) — call only from an xTaskCreate task");
        return false;
    }
    if(!s_started || !data || len > 256) return false;
    if(!survey_active() && !s_ap_mode) wlan_hal_set_ap_mode(true);

    esp_err_t err = esp_wifi_80211_tx(WIFI_IF_AP, data, len, false);
    if(err == ESP_OK) return true;

    // TX ring full → retry cepat tanpa delay
    if(err == ESP_ERR_NO_MEM) {
        for(int i = 0; i < 4; i++) {
            taskYIELD();
            err = esp_wifi_80211_tx(WIFI_IF_AP, data, len, false);
            if(err == ESP_OK) return true;
        }
    }
    return false;
}

void wlan_hal_channel_lock(void) {
    s_channel_locked = true;
    ESP_LOGI(TAG, "Channel LOCKED");
}

void wlan_hal_channel_unlock(void) {
    s_channel_locked = false;
    ESP_LOGI(TAG, "Channel UNLOCKED");
}

bool wlan_hal_channel_is_locked(void) {
    return s_channel_locked;
}

// Promiscuous filter — MGMT only / DATA only
void wlan_hal_set_promiscuous_filter(uint32_t filter_mask) {
    // Guard: ruft esp_wifi_set_promiscuous_filter direkt im Caller-Thread auf
    // (nicht über den wlan-Worker). esp_wifi_*-ioctls aus einem FuriThread
    // crashen hart (LoadProhibited) — nur aus einem echten xTaskCreate-Task
    // aufrufen. furi_thread_get_current() != NULL ⇒ FuriThread ⇒ abbrechen.
    if(furi_thread_get_current()) {
        ESP_LOGE(
            TAG,
            "set_promiscuous_filter: refused (FuriThread) — call only from an xTaskCreate task");
        return;
    }
    if(!s_started) return;
    wifi_promiscuous_filter_t filter = {.filter_mask = filter_mask};
    esp_wifi_set_promiscuous_filter(&filter);
}

// Max TX power
void wlan_hal_set_max_tx_power(int8_t power) {
    // Guard: ruft esp_wifi_set_max_tx_power direkt im Caller-Thread auf (nicht
    // über den wlan-Worker). esp_wifi_*-ioctls aus einem FuriThread crashen
    // hart (LoadProhibited) — nur aus einem echten xTaskCreate-Task aufrufen.
    // furi_thread_get_current() != NULL ⇒ FuriThread ⇒ abbrechen.
    if(furi_thread_get_current()) {
        ESP_LOGE(
            TAG, "set_max_tx_power: refused (FuriThread) — call only from an xTaskCreate task");
        return;
    }
    if(!s_started) return;
    esp_wifi_set_max_tx_power(power);
}

bool wlan_hal_run_in_worker(WlanHalWorkerFn fn, void* arg) {
    if(!fn || survey_active()) return false;
    // Lazy-init the worker queue + task. Evil Portal is entered directly from
    // the menu without going through wlan_hal_start (no STA scan), so the
    // worker may not exist yet. Safe to call repeatedly; no-ops if already up.
    if(!wlan_ensure_worker()) return false;
    WlanCmd cmd = {.type = WCMD_RUN_FN, .run_fn = {.fn = fn, .arg = arg}};
    // true only if the worker actually ran fn (not queue-full / not timed
    // out before it started) — callers gate on this.
    return wlan_cmd_exec(&cmd, WLAN_RUNFN_TIMEOUT_MS, NULL, NULL, NULL);
}

void wlan_hal_scan(wifi_ap_record_t** out_records, uint16_t* out_count, uint16_t max_count) {
    wlan_hal_scan_cancellable(out_records, out_count, max_count, NULL);
}

void wlan_hal_scan_cancellable(
    wifi_ap_record_t** out_records,
    uint16_t* out_count,
    uint16_t max_count,
    const volatile bool* cancel) {
    if(!out_records || !out_count) return;
    *out_records = NULL;
    *out_count = 0;
    if(!s_started) {
        return;
    }
    furi_hal_power_light_sleep_inhibit_enter();
    NotificationApp* notification = furi_record_open(RECORD_NOTIFICATION);
    notification_message(notification, &sequence_wifi_scan);
    furi_record_close(RECORD_NOTIFICATION);

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = {
            .active = {
                .min = 80,
                .max = 120,
            },
        },
    };
    WlanCmd cmd = {
        .type = WCMD_SCAN,
        .scan = {
            .config = scan_config, // BY VALUE kopiert — kein Stack-Pointer mehr
            .max_count = max_count,
            .cancel = cancel,
        },
    };

    // FIX: pakai timeout variant khusus scan (10s) — kalau worker stuck
    // karena scan-cancel loop atau driver hang, caller tidak akan stuck
    // selamanya. out_records/out_count werden im lebenden Frame aus dem Token
    // kopiert; bei Timeout bleiben sie NULL/0 (statt Use-after-Stack).
    wlan_cmd_exec(&cmd, WLAN_SCAN_CMD_TIMEOUT_MS, NULL, out_records, out_count);

    notification = furi_record_open(RECORD_NOTIFICATION);
    notification_message(notification, &sequence_wifi_scan);
    furi_record_close(RECORD_NOTIFICATION);
    furi_hal_power_light_sleep_inhibit_exit();
}

// ---------------------------------------------------------------------------
// Beacon-Spam: dedizierter xTaskCreate-Task (parallel zum Worker), nutzt
// esp_wifi_80211_tx() direkt. Frame-Counter + Stop-Flag sind volatile.
// ---------------------------------------------------------------------------

// Max. Beacon-Framelänge: 24 (MAC-Header) + 12 (Fixed-Params) + 34 (SSID-IE
// max) + 10 (Rates-IE) + 3 (DS-Param) = 83; auf 128 aufgerundet.
#define BEACON_MAX_LEN 128

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

// Baut einen korrekt gepackten Open-Network-Beacon. Gibt die tatsächliche
// Framelänge zurück. WICHTIG: SSID-IE-Länge MUSS zur SSID-Bytezahl passen und
// die folgenden IEs müssen unmittelbar anschließen — sonst ist der Beacon
// malformed und Clients zeigen die SSID nicht an (alter Template-Code war hier
// fehlerhaft: feste 30-Byte-SSID bei len=32, Channel in RSN-Länge geschrieben).
static uint16_t build_beacon_packet(uint8_t* p, const uint8_t* mac,
                                    const char* ssid, uint8_t channel) {
    uint16_t n = 0;

    // --- 802.11 MAC-Header (24 Byte) ---
    p[n++] = 0x80; p[n++] = 0x00;          // Frame Control: Beacon
    p[n++] = 0x00; p[n++] = 0x00;          // Duration
    for(int i = 0; i < 6; i++) p[n++] = 0xFF; // DA = Broadcast
    for(int i = 0; i < 6; i++) p[n++] = mac[i]; // SA
    for(int i = 0; i < 6; i++) p[n++] = mac[i]; // BSSID
    p[n++] = 0x00; p[n++] = 0x00;          // Sequence Control (HW füllt)

    // --- Fixed Parameters (12 Byte) ---
    for(int i = 0; i < 8; i++) p[n++] = 0x00; // Timestamp
    p[n++] = 0x64; p[n++] = 0x00;          // Beacon Interval = 100 TU
    p[n++] = 0x01; p[n++] = 0x04;          // Capability: ESS (offen, kein Privacy)

    // --- SSID-IE (tag 0) ---
    uint8_t sl = (uint8_t)strlen(ssid);
    if(sl > 32) sl = 32;
    p[n++] = 0x00; p[n++] = sl;
    for(uint8_t i = 0; i < sl; i++) p[n++] = (uint8_t)ssid[i];

    // --- Supported Rates-IE (tag 1) ---
    p[n++] = 0x01; p[n++] = 0x08;
    p[n++] = 0x82; p[n++] = 0x84; p[n++] = 0x8b; p[n++] = 0x96;
    p[n++] = 0x24; p[n++] = 0x30; p[n++] = 0x48; p[n++] = 0x6c;

    // --- DS-Parameter-IE (tag 3): aktueller Channel ---
    p[n++] = 0x03; p[n++] = 0x01; p[n++] = channel;

    return n;
}

static void beacon_spam_task(void* param) {
    (void)param;
    const char** ssid_list = NULL;
    char gen_ssid[64];
    uint8_t mac[6];
    uint8_t packet[BEACON_MAX_LEN];
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

        uint16_t frame_len = build_beacon_packet(packet, mac, current_ssid, channel);
        if(esp_wifi_80211_tx(WIFI_IF_AP, packet, frame_len, false) == ESP_OK) {
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
    wlan_hal_set_ap_mode(true); // beacon frames go out on WIFI_IF_AP
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
