#include "../wifi_app.h"
#include "../wifi_hal.h"
#include "../views/netscan_view.h"

#include <esp_log.h>
#include <esp_netif.h>
#include <lwip/etharp.h>
#include <lwip/ip4_addr.h>
#include <lwip/netif.h>
#include <lwip/tcpip.h>
#include <string.h>

#define TAG WIFI_APP_LOG_TAG

static NetscanHost s_hosts[NETSCAN_MAX_HOSTS];
static uint8_t s_host_count = 0;
static uint32_t s_base_host = 0;
static uint32_t s_own_ip = 0;
static int s_scan_idx = 1;
static bool s_scan_done = false;

static bool host_exists(uint32_t ip) {
    for(int i = 0; i < s_host_count; i++) {
        if(s_hosts[i].ip == ip) return true;
    }
    return false;
}

static void host_add(uint32_t ip, const uint8_t* mac) {
    if(s_host_count >= NETSCAN_MAX_HOSTS) return;
    if(host_exists(ip)) return;
    s_hosts[s_host_count].ip = ip;
    memcpy(s_hosts[s_host_count].mac, mac, 6);
    s_host_count++;
}

static void arp_scan_tick_fn(void* ctx) {
    UNUSED(ctx);
    struct netif* nif = netif_default;
    if(!nif) return;

    for(size_t idx = 0; idx < ARP_TABLE_SIZE; idx++) {
        ip4_addr_t* ip_ret = NULL;
        struct netif* nif_ret = NULL;
        struct eth_addr* eth_ret = NULL;
        if(etharp_get_entry(idx, &ip_ret, &nif_ret, &eth_ret) == 1) {
            uint32_t found_ip = ip_ret->addr;
            if(found_ip == 0 || found_ip == s_own_ip) continue;
            if(!host_exists(found_ip)) {
                host_add(found_ip, eth_ret->addr);
            }
        }
    }

    if(s_scan_idx <= 254) {
        for(int i = 0; i < 4 && s_scan_idx <= 254; i++, s_scan_idx++) {
            ip4_addr_t target;
            target.addr = htonl(s_base_host | s_scan_idx);
            etharp_request(nif, &target);
        }
    }

    if(s_scan_idx > 254 && !s_scan_done) {
        s_scan_done = true;
    }
}

void wifi_app_scene_netscan_on_enter(void* context) {
    WifiApp* app = context;

    s_host_count = 0;
    s_scan_idx = 1;
    s_scan_done = false;
    memset(s_hosts, 0, sizeof(s_hosts));

    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info;
    char own_ip_str[16] = "?.?.?.?";

    if(netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        s_own_ip = ip_info.ip.addr;
        s_base_host = ntohl(ip_info.ip.addr & ip_info.netmask.addr);
        snprintf(own_ip_str, sizeof(own_ip_str), IPSTR, IP2STR(&ip_info.ip));
        s_scan_idx = ntohl(ip_info.gw.addr) & 0xFF;
    }

    NetscanViewModel* model = view_get_model(app->view_netscan);
    memset(model, 0, sizeof(NetscanViewModel));
    strncpy(model->own_ip, own_ip_str, sizeof(model->own_ip) - 1);
    model->scanning = true;
    snprintf(model->status, sizeof(model->status), "Scanning...");
    view_commit_model(app->view_netscan, true);

    view_dispatcher_switch_to_view(app->view_dispatcher, WifiAppViewNetscan);
}

bool wifi_app_scene_netscan_on_event(void* context, SceneManagerEvent event) {
    WifiApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        NetscanViewModel* model = view_get_model(app->view_netscan);
        if(event.event == InputKeyUp) {
            if(model->selected > 0) {
                model->selected--;
                if(model->selected < model->window_offset)
                    model->window_offset = model->selected;
            }
            view_commit_model(app->view_netscan, true);
            consumed = true;
        } else if(event.event == InputKeyDown) {
            if(model->count > 0 && model->selected < model->count - 1) {
                model->selected++;
                if(model->selected >= model->window_offset + NETSCAN_ITEMS_ON_SCREEN)
                    model->window_offset = model->selected - NETSCAN_ITEMS_ON_SCREEN + 1;
            }
            view_commit_model(app->view_netscan, true);
            consumed = true;
        } else if(event.event == InputKeyOk) {
            if(model->count > 0 && model->selected < model->count) {
                app->portscan_target_ip = model->hosts[model->selected].ip;
                view_commit_model(app->view_netscan, false);
                scene_manager_next_scene(app->scene_manager, WifiAppScenePortscan);
                consumed = true;
            } else {
                view_commit_model(app->view_netscan, false);
            }
        } else {
            view_commit_model(app->view_netscan, false);
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        tcpip_callback(arp_scan_tick_fn, NULL);

        NetscanViewModel* model = view_get_model(app->view_netscan);
        model->scanning = !s_scan_done;
        model->progress = (s_scan_idx * 100) / 254;
        model->count = s_host_count;
        for(int i = 0; i < s_host_count && i < NETSCAN_MAX_HOSTS; i++) {
            model->hosts[i] = s_hosts[i];
        }
        if(!s_scan_done) {
            snprintf(model->status, sizeof(model->status), "Scanning %d%%...",
                     (s_scan_idx * 100) / 254);
        }
        view_commit_model(app->view_netscan, true);
    }

    return consumed;
}

void wifi_app_scene_netscan_on_exit(void* context) {
    WifiApp* app = context;
    widget_reset(app->widget);
}
