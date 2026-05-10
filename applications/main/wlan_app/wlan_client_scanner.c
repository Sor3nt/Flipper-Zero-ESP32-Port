#include "wlan_client_scanner.h"
#include "wlan_app.h"
#include "wlan_hal.h"

#include <esp_wifi.h>
#include <string.h>

#define CS_RING_SIZE 32

typedef struct {
    uint8_t mac[6];
    int8_t rssi;
} CsEvent;

static volatile uint32_t s_write_idx = 0;
static volatile uint32_t s_read_idx = 0;
static CsEvent s_ring[CS_RING_SIZE];
static uint8_t s_filter_bssid[6];
static bool s_filter_active = false;

static bool cs_is_invalid_sta(const uint8_t* m) {
    if(m[0] & 0x01) return true; // Multicast/Broadcast
    bool all_zero = true;
    for(int i = 0; i < 6; i++) {
        if(m[i] != 0) {
            all_zero = false;
            break;
        }
    }
    return all_zero;
}

static void cs_promisc_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if(type != WIFI_PKT_DATA) return;
    const wifi_promiscuous_pkt_t* pkt = buf;
    const uint8_t* p = pkt->payload;
    int len = pkt->rx_ctrl.sig_len;
    if(len < 24) return;

    uint16_t fc = p[0] | (p[1] << 8);
    uint8_t frame_type = (fc & 0x0C) >> 2;
    if(frame_type != 2) return; // nur Data-Frames

    uint8_t to_ds = (fc & 0x0100) >> 8;
    uint8_t from_ds = (fc & 0x0200) >> 9;

    const uint8_t* addr1 = &p[4];
    const uint8_t* addr2 = &p[10];
    const uint8_t* addr3 = &p[16];

    const uint8_t* bssid;
    const uint8_t* sta;
    if(to_ds && !from_ds) {
        bssid = addr1;
        sta = addr2;
    } else if(!to_ds && from_ds) {
        bssid = addr2;
        sta = addr1;
    } else if(!to_ds && !from_ds) {
        bssid = addr3;
        sta = addr2;
    } else {
        return; // WDS / 4-address — ignorieren
    }

    if(s_filter_active && memcmp(bssid, s_filter_bssid, 6) != 0) return;
    if(cs_is_invalid_sta(sta)) return;
    if(memcmp(sta, bssid, 6) == 0) return; // STA == AP

    uint32_t next = (s_write_idx + 1) % CS_RING_SIZE;
    if(next == s_read_idx) return; // Buffer voll → Drop
    memcpy(s_ring[s_write_idx].mac, sta, 6);
    s_ring[s_write_idx].rssi = pkt->rx_ctrl.rssi;
    s_write_idx = next;
}

void wlan_client_scanner_start(const uint8_t* bssid_filter, uint8_t channel) {
    s_write_idx = 0;
    s_read_idx = 0;
    if(bssid_filter) {
        memcpy(s_filter_bssid, bssid_filter, 6);
        s_filter_active = true;
    } else {
        s_filter_active = false;
    }
    if(!wlan_hal_is_started()) wlan_hal_start();
    if(wlan_hal_is_connected()) wlan_hal_disconnect();
    if(channel >= 1 && channel <= 14) wlan_hal_set_channel(channel);
    wlan_hal_set_promiscuous(true, cs_promisc_cb);
}

void wlan_client_scanner_stop(void) {
    wlan_hal_set_promiscuous(false, NULL);
    s_write_idx = 0;
    s_read_idx = 0;
    s_filter_active = false;
}

bool wlan_client_scanner_drain(WlanApp* app) {
    bool changed = false;
    while(s_read_idx != s_write_idx) {
        CsEvent ev = s_ring[s_read_idx];
        s_read_idx = (s_read_idx + 1) % CS_RING_SIZE;

        int found = -1;
        for(uint8_t i = 0; i < app->deauth_client_count; i++) {
            if(memcmp(app->deauth_clients[i].mac, ev.mac, 6) == 0) {
                found = i;
                break;
            }
        }
        if(found >= 0) {
            // EWMA mit alpha=0.3 → glättet RSSI-Schwankungen.
            int new_rssi = (app->deauth_clients[found].rssi * 7 + ev.rssi * 3) / 10;
            if(new_rssi != app->deauth_clients[found].rssi) {
                app->deauth_clients[found].rssi = (int8_t)new_rssi;
                changed = true;
            }
        } else if(app->deauth_client_count < WLAN_APP_MAX_DEAUTH_CLIENTS) {
            WlanDeauthClient* c = &app->deauth_clients[app->deauth_client_count];
            memcpy(c->mac, ev.mac, 6);
            c->rssi = ev.rssi;
            c->cut = false;
            app->deauth_client_count++;
            changed = true;
        }
    }
    return changed;
}
