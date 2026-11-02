#include "wlan_live_scanner.h"
#include "wlan_oui.h"
#include <wlan_hal.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_attr.h>
#include <string.h>
#include <stdlib.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>
#include <freertos/semphr.h>
#include <esp_heap_caps.h>

#define TAG "LiveScan"

static TaskHandle_t        s_task = NULL;
static volatile bool       s_running = false;
static volatile bool       s_pause = false;
static volatile bool       s_stop = false;
static volatile bool       s_hopping = true;
static volatile uint8_t    s_current_channel = 1;

static EXT_RAM_BSS_ATTR LiveApEntry s_aps[LIVE_SCAN_MAX_APS];
static volatile uint16_t   s_ap_count = 0;

static SemaphoreHandle_t   s_mtx = NULL;
static volatile LiveSortMode   s_sort = LiveSortRssi;
static volatile LiveFilterMode s_filter = LiveFilterAll;

// OUI-Tabelle (SD-basiert, bestehende API). Lifecycle komplett im Hop-Task:
// geladen vor promiscuous-enable, freigegeben nach promiscuous-disable — damit
// sniffer_cb (WiFi-Task) nie auf einen ungültigen Pointer greift.
static WlanOuiTable*       s_oui = NULL;

static inline uint32_t now_ms(void) {
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}
static inline void ls_lock(void)   { if(s_mtx) xSemaphoreTake(s_mtx, portMAX_DELAY); }
static inline void ls_unlock(void) { if(s_mtx) xSemaphoreGive(s_mtx); }

// ============================================================================
// RSN / WPA3 / PMF parser
// ============================================================================
static void parse_security(const uint8_t* ies, int len,
                            bool* has_rsn, bool* has_wpa3, bool* has_pmf,
                            uint8_t* authmode) {
    *has_rsn = false;
    *has_wpa3 = false;
    *has_pmf = false;

    int pos = 0;
    while(pos + 2 <= len) {
        uint8_t id = ies[pos];
        uint8_t ilen = ies[pos + 1];
        if(pos + 2 + ilen > len) break;
        const uint8_t* p = &ies[pos + 2];

        if(id == 48 && ilen >= 18) {   // RSN IE
            *has_rsn = true;
            if(*authmode < 3) *authmode = 3;

            int rp = 2;
            if(rp + 4 > ilen) { pos += 2 + ilen; continue; }
            rp += 4;   // group cipher suite

            if(rp + 2 > ilen) { pos += 2 + ilen; continue; }
            uint16_t pcount = p[rp] | (p[rp + 1] << 8);
            rp += 2 + pcount * 4;

            if(rp + 2 > ilen) { pos += 2 + ilen; continue; }
            uint16_t acount = p[rp] | (p[rp + 1] << 8);
            rp += 2;

            for(uint16_t i = 0; i < acount && rp + 4 <= ilen; i++, rp += 4) {
                // AKM 00-0F-AC-08 = SAE (WPA3)
                if(p[rp] == 0x00 && p[rp+1] == 0x0F &&
                   p[rp+2] == 0xAC && p[rp+3] == 0x08) {
                    *has_wpa3 = true;
                    *authmode = 4;
                }
            }

            if(rp + 2 <= ilen) {
                uint16_t caps = p[rp] | (p[rp + 1] << 8);
                if(caps & 0x0040) *has_pmf = true;   // MFPR
                if(caps & 0x0080) *has_pmf = true;   // MFPC
            }
        }
        if(id == 127 && ilen >= 3) {   // Extended Capabilities
            if(p[2] & 0x40) *has_pmf = true;
            if(p[2] & 0x80) *has_pmf = true;
        }
        pos += 2 + ilen;
    }
}

// ============================================================================
// AP store (immer unter ls_lock aufgerufen)
// ============================================================================
static int find_ap_nolock(const uint8_t* bssid) {
    for(uint16_t i = 0; i < s_ap_count; i++) {
        if(s_aps[i].valid && memcmp(s_aps[i].bssid, bssid, 6) == 0) return i;
    }
    return -1;
}

static void add_or_update(const uint8_t* bssid, uint8_t ch, int8_t rssi,
                          const char* ssid, const uint8_t* ies, int ies_len,
                          uint16_t caps) {
    int idx = find_ap_nolock(bssid);
    uint32_t t = now_ms();

    if(idx >= 0) {
        LiveApEntry* ap = &s_aps[idx];
        ap->rssi = rssi;
        if(rssi > ap->rssi_max) ap->rssi_max = rssi;
        if(rssi < ap->rssi_min) ap->rssi_min = rssi;
        ap->last_seen_ms = t;
        ap->beacon_count++;
        if(ssid && ssid[0] && !ap->ssid[0]) {
            strncpy(ap->ssid, ssid, 32);
            ap->ssid[32] = 0;
        }
        return;
    }

    if(s_ap_count >= LIVE_SCAN_MAX_APS) return;
    idx = s_ap_count;
    LiveApEntry* ap = &s_aps[idx];
    memset(ap, 0, sizeof(*ap));

    memcpy(ap->bssid, bssid, 6);
    ap->channel = ch;
    ap->rssi = rssi;
    ap->rssi_max = rssi;
    ap->rssi_min = rssi;
    ap->first_seen_ms = t;
    ap->last_seen_ms = t;
    ap->beacon_count = 1;
    ap->capabilities = caps;

    if(ssid) {
        strncpy(ap->ssid, ssid, 32);
        ap->ssid[32] = 0;
    }

    ap->authmode = (caps & 0x0010) ? 1 : 0;   // Privacy-Bit → WEP/privacy
    if(ies && ies_len > 0) {
        parse_security(ies, ies_len, &ap->has_rsn, &ap->has_wpa3,
                       &ap->has_pmf, &ap->authmode);
    }

    const char* vendor = s_oui ? wlan_oui_lookup(s_oui, bssid) : NULL;
    if(vendor) {
        strncpy(ap->vendor, vendor, sizeof(ap->vendor) - 1);
        ap->vendor[sizeof(ap->vendor) - 1] = 0;
    } else {
        ap->vendor[0] = 0;
    }

    ap->valid = true;
    s_ap_count++;
}

// ============================================================================
// Sniffer callback (WiFi-Task-Kontext)
// ============================================================================
static void sniffer_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if(!s_running || s_pause) return;
    if(type != WIFI_PKT_MGMT) return;

    const wifi_promiscuous_pkt_t* pkt = buf;
    int sig_len = pkt->rx_ctrl.sig_len;
    if(sig_len < 28) return;
    int len = sig_len - 4;   // FCS
    if(len < 36) return;

    const uint8_t* p = pkt->payload;
    uint16_t fc = p[0] | (p[1] << 8);
    uint8_t ftype = (fc >> 2) & 0x03;
    uint8_t fsub = (fc >> 4) & 0x0F;
    if(ftype != 0) return;
    if(fsub != 0x08 && fsub != 0x05) return;   // Beacon / Probe-Response

    const uint8_t* bssid = &p[16];
    if(bssid[0] & 0x01) return;   // Multicast/Broadcast-BSSID ignorieren

    uint8_t ch = pkt->rx_ctrl.channel;
    if(ch < 1 || ch > 13) return;

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

    uint16_t caps = p[34] | (p[35] << 8);

    ls_lock();
    add_or_update(bssid, ch, pkt->rx_ctrl.rssi, ssid, ies, ies_len, caps);
    ls_unlock();
}

// ============================================================================
// Hop task — Radio-Bringup + Channel-Hopping laufen hier (nie im UI-Thread).
// ============================================================================
static void hop_task(void* arg) {
    (void)arg;
    ESP_LOGI(TAG, "Hop task started");

    if(!wlan_hal_is_started()) {
        if(!wlan_hal_start()) {
            ESP_LOGE(TAG, "wlan_hal_start failed");
            s_running = false;
            s_task = NULL;
            vTaskDeleteWithCaps(NULL);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if(wlan_hal_is_connected()) wlan_hal_disconnect();

    s_oui = wlan_oui_load();   // NULL wenn SD-Datei fehlt → vendor bleibt leer

    wlan_hal_set_promiscuous_filter(WIFI_PROMIS_FILTER_MASK_MGMT);
    wlan_hal_set_promiscuous(true, sniffer_cb);

    while(!s_stop) {
        if(s_running && !s_pause && s_hopping) {
            for(uint8_t ch = 1; ch <= 13 && !s_stop; ch++) {
                if(!s_running || s_pause || !s_hopping) break;
                s_current_channel = ch;
                wlan_hal_set_channel(ch);
                vTaskDelay(pdMS_TO_TICKS(LIVE_SCAN_HOP_MS));
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    wlan_hal_set_promiscuous(false, NULL);
    if(s_oui) { wlan_oui_free(s_oui); s_oui = NULL; }

    ESP_LOGI(TAG, "Hop task exit");
    s_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

// ============================================================================
// PUBLIC
// ============================================================================
bool wlan_live_scan_start(void) {
    if(s_running) return true;
    if(!s_mtx) s_mtx = xSemaphoreCreateMutex();
    if(!s_mtx) return false;

    ls_lock();
    memset(s_aps, 0, sizeof(s_aps));
    s_ap_count = 0;
    ls_unlock();

    s_stop = false;
    s_pause = false;
    s_hopping = true;
    s_running = true;

    // Worker-Stack ins PSRAM (xTaskCreatePinnedToCoreWithCaps): bei residentem
    // BT-Stack kann der interne Heap zu knapp für den Task-Stack sein → ENOMEM.
    // Nur der kleine TCB bleibt intern. Der RX-Callback (sniffer_cb) läuft im
    // WiFi-Task, nicht auf diesem Stack; set_channel/promiscuous sind cache-sicher
    // (WiFi IRAM OP). Self-Delete MUSS über vTaskDeleteWithCaps laufen.
    if(xTaskCreatePinnedToCoreWithCaps(hop_task, "LiveHop",
                                4096, NULL, 8, &s_task, 1, MALLOC_CAP_SPIRAM) != pdPASS) {
        s_running = false;
        s_task = NULL;
        return false;
    }
    ESP_LOGI(TAG, "Started");
    return true;
}

void wlan_live_scan_stop(void) {
    if(!s_running && !s_task) return;
    s_running = false;
    s_stop = true;

    for(int i = 0; i < 300 && s_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // Sicherheitshalber — Task hat das normalerweise schon erledigt.
    wlan_hal_set_promiscuous(false, NULL);
    ESP_LOGI(TAG, "Stopped");
}

bool wlan_live_scan_is_running(void) { return s_running; }

void wlan_live_scan_clear(void) {
    ls_lock();
    memset(s_aps, 0, sizeof(s_aps));
    s_ap_count = 0;
    ls_unlock();
}

void wlan_live_scan_pause(void)  { s_pause = true; }
void wlan_live_scan_resume(void) { s_pause = false; }
uint16_t wlan_live_scan_get_count(void) { return s_ap_count; }

static bool matches_filter(const LiveApEntry* ap, LiveFilterMode f) {
    switch(f) {
    case LiveFilterAll:  return true;
    case LiveFilterOpen: return ap->authmode == 0;
    case LiveFilterWpa2: return ap->has_rsn && !ap->has_wpa3;
    case LiveFilterWpa3: return ap->has_wpa3;
    default:             return true;
    }
}

static int cmp_rssi(const void* a, const void* b) {
    return (int)((const LiveApEntry*)b)->rssi - (int)((const LiveApEntry*)a)->rssi;
}
static int cmp_channel(const void* a, const void* b) {
    return (int)((const LiveApEntry*)a)->channel - (int)((const LiveApEntry*)b)->channel;
}
static int cmp_ssid(const void* a, const void* b) {
    return strncmp(((const LiveApEntry*)a)->ssid, ((const LiveApEntry*)b)->ssid, 32);
}
static int cmp_first(const void* a, const void* b) {
    uint32_t fa = ((const LiveApEntry*)a)->first_seen_ms;
    uint32_t fb = ((const LiveApEntry*)b)->first_seen_ms;
    if(fa < fb) return -1;
    if(fa > fb) return 1;
    return 0;
}

uint16_t wlan_live_scan_snapshot(LiveApEntry* out, uint16_t max) {
    if(!out || max == 0) return 0;

    ls_lock();
    uint16_t n = 0;
    for(uint16_t i = 0; i < s_ap_count && n < max; i++) {
        if(!s_aps[i].valid) continue;
        if(!matches_filter(&s_aps[i], s_filter)) continue;
        out[n++] = s_aps[i];
    }
    ls_unlock();

    switch(s_sort) {
    case LiveSortRssi:      qsort(out, n, sizeof(LiveApEntry), cmp_rssi); break;
    case LiveSortChannel:   qsort(out, n, sizeof(LiveApEntry), cmp_channel); break;
    case LiveSortSsid:      qsort(out, n, sizeof(LiveApEntry), cmp_ssid); break;
    case LiveSortFirstSeen: qsort(out, n, sizeof(LiveApEntry), cmp_first); break;
    }
    return n;
}

void wlan_live_scan_set_sort(LiveSortMode m)     { s_sort = m; }
void wlan_live_scan_set_filter(LiveFilterMode f) { s_filter = f; }
LiveSortMode   wlan_live_scan_get_sort(void)     { return s_sort; }
LiveFilterMode wlan_live_scan_get_filter(void)   { return s_filter; }

void wlan_live_scan_set_hop(bool enabled) {
    s_hopping = enabled;
    if(!enabled) wlan_hal_set_channel(s_current_channel);
}
bool wlan_live_scan_is_hopping(void) { return s_hopping; }
uint8_t wlan_live_scan_get_channel(void) { return s_current_channel; }
void wlan_live_scan_set_channel(uint8_t ch) {
    if(ch < 1 || ch > 13) return;
    s_current_channel = ch;
    s_hopping = false;
    wlan_hal_set_channel(ch);
}
