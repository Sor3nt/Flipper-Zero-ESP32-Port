#include "../wifi_app.h"
#include "../wifi_hal.h"
#include "../views/airsnitch_view.h"

#include <esp_log.h>
#include <esp_netif.h>
#include <lwip/etharp.h>
#include <lwip/ip4_addr.h>
#include <lwip/netif.h>
#include <lwip/tcpip.h>
#include <string.h>

#define TAG WIFI_APP_LOG_TAG

// ---------------------------------------------------------------------------
// ARP scan state — tick-based using lwIP raw etharp_request (no sockets)
// ---------------------------------------------------------------------------
static AirSnitchHost s_hosts[AIRSNITCH_MAX_HOSTS];
static uint8_t s_host_count = 0;
static uint32_t s_base_host = 0; // base IP in host byte order
static uint32_t s_own_ip = 0;
static int s_scan_idx = 1;       // current IP suffix being scanned (1-254)
static bool s_scan_done = false;

static bool host_exists(uint32_t ip) {
    for(int i = 0; i < s_host_count; i++) {
        if(s_hosts[i].ip == ip) return true;
    }
    return false;
}

static void host_add(uint32_t ip, const uint8_t* mac) {
    if(s_host_count >= AIRSNITCH_MAX_HOSTS) return;
    if(host_exists(ip)) return;
    s_hosts[s_host_count].ip = ip;
    memcpy(s_hosts[s_host_count].mac, mac, 6);
    s_host_count++;
}

/** Callback executed inside the lwIP/TCPIP thread.
 *  First reads ARP cache from PREVIOUS batch, then sends next batch of requests. */
static void arp_scan_tick_fn(void* ctx) {
    UNUSED(ctx);
    struct netif* nif = netif_default;
    if(!nif) {
        ESP_LOGE(TAG, "ARP tick: netif_default is NULL!");
        return;
    }

    // 1. Read ARP cache (results from previous tick's requests)
    //    etharp_get_entry returns 1 on success (STABLE entry), 0 if empty/pending
    int cache_stable = 0;
    for(size_t idx = 0; idx < ARP_TABLE_SIZE; idx++) {
        ip4_addr_t* ip_ret = NULL;
        struct netif* nif_ret = NULL;
        struct eth_addr* eth_ret = NULL;

        if(etharp_get_entry(idx, &ip_ret, &nif_ret, &eth_ret) == 1) {
            cache_stable++;
            uint32_t found_ip = ip_ret->addr;
            if(found_ip == 0 || found_ip == s_own_ip) continue;
            if(!host_exists(found_ip)) {
                host_add(found_ip, eth_ret->addr);
                ESP_LOGI(TAG, "Found: " IPSTR " %02X:%02X:%02X:%02X:%02X:%02X",
                         IP2STR(ip_ret),
                         eth_ret->addr[0], eth_ret->addr[1], eth_ret->addr[2],
                         eth_ret->addr[3], eth_ret->addr[4], eth_ret->addr[5]);
            }
        }
    }

    // 2. Send next batch of ARP requests
    if(s_scan_idx <= 254) {
        int sent = 0;
        for(int i = 0; i < 4 && s_scan_idx <= 254; i++, s_scan_idx++) {
            ip4_addr_t target;
            target.addr = htonl(s_base_host | s_scan_idx);
            err_t err = etharp_request(nif, &target);
            if(err == ERR_OK) {
                sent++;
            } else {
                ESP_LOGW(TAG, "etharp_request(" IPSTR ") failed: %d",
                         IP2STR(&target), err);
            }
        }
        ESP_LOGI(TAG, "ARP tick: sent=%d idx=%d stable=%d hosts=%d nif=%s ip=" IPSTR,
                 sent, s_scan_idx, cache_stable, s_host_count,
                 nif->name[0] ? nif->name : "?",
                 IP2STR(netif_ip4_addr(nif)));
    }

    if(s_scan_idx > 254 && !s_scan_done) {
        s_scan_done = true;
        ESP_LOGI(TAG, "ARP scan complete, %d hosts found (ARP_TABLE_SIZE=%d)",
                 s_host_count, ARP_TABLE_SIZE);
    }
}

// ---------------------------------------------------------------------------
// Scene handlers
// ---------------------------------------------------------------------------

void wifi_app_scene_airsn_scan_on_enter(void* context) {
    WifiApp* app = context;

    s_host_count = 0;
    s_scan_idx = 1;
    s_scan_done = false;
    memset(s_hosts, 0, sizeof(s_hosts));

    // Get IP info
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info;
    if(netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        s_own_ip = ip_info.ip.addr;
        s_base_host = ntohl(ip_info.ip.addr & ip_info.netmask.addr);

        ESP_LOGI(TAG, "ARP scan: own=" IPSTR " gw=" IPSTR " mask=" IPSTR " base_host=0x%08lX",
                 IP2STR(&ip_info.ip), IP2STR(&ip_info.gw), IP2STR(&ip_info.netmask),
                 (unsigned long)s_base_host);

        // Start with gateway IP (must respond)
        s_scan_idx = ntohl(ip_info.gw.addr) & 0xFF;
        ESP_LOGI(TAG, "Starting scan at gateway suffix: %d", s_scan_idx);
    } else {
        ESP_LOGE(TAG, "Failed to get IP info");
        scene_manager_previous_scene(app->scene_manager);
        return;
    }

    // Init view
    AirSnitchViewModel* model = view_get_model(app->view_airsnitch);
    memset(model, 0, sizeof(AirSnitchViewModel));
    model->scanning = true;
    snprintf(model->status, sizeof(model->status), "Scanning...");
    view_commit_model(app->view_airsnitch, true);

    view_dispatcher_switch_to_view(app->view_dispatcher, WifiAppViewAirSnitch);
}

bool wifi_app_scene_airsn_scan_on_event(void* context, SceneManagerEvent event) {
    WifiApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == InputKeyUp) {
            AirSnitchViewModel* model = view_get_model(app->view_airsnitch);
            if(model->selected > 0) {
                model->selected--;
                if(model->selected < model->window_offset) {
                    model->window_offset = model->selected;
                }
            }
            view_commit_model(app->view_airsnitch, true);
            consumed = true;
        } else if(event.event == InputKeyDown) {
            AirSnitchViewModel* model = view_get_model(app->view_airsnitch);
            if(model->count > 0 && model->selected < model->count - 1) {
                model->selected++;
                if(model->selected >= model->window_offset + AIRSNITCH_ITEMS_ON_SCREEN) {
                    model->window_offset = model->selected - AIRSNITCH_ITEMS_ON_SCREEN + 1;
                }
            }
            view_commit_model(app->view_airsnitch, true);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        // Send ARP requests + read cache in lwIP TCPIP thread
        tcpip_callback(arp_scan_tick_fn, NULL);

        // Update view
        AirSnitchViewModel* model = view_get_model(app->view_airsnitch);
        model->scanning = !s_scan_done;
        model->progress = (s_scan_idx * 100) / 254;
        model->count = s_host_count;

        for(int i = 0; i < s_host_count && i < AIRSNITCH_MAX_HOSTS; i++) {
            model->hosts[i] = s_hosts[i];
        }

        if(!s_scan_done) {
            snprintf(model->status, sizeof(model->status), "Scanning %d%%...",
                     (s_scan_idx * 100) / 254);
        }

        view_commit_model(app->view_airsnitch, true);
    }

    return consumed;
}

extern bool g_airsnitch_did_connect;

void wifi_app_scene_airsn_scan_on_exit(void* context) {
    UNUSED(context);
    if(g_airsnitch_did_connect) {
        wifi_hal_disconnect();
        g_airsnitch_did_connect = false;
    }
}
