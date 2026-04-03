#include "../esp_now_app.h"

#include <esp_wifi.h>
#include <esp_now.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <esp_log.h>

#define TAG "EspNowSniff"

static EspNowApp* g_app = NULL;

static void esp_now_recv_cb(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    if(!g_app || !info || !data || len <= 0) return;

    if(furi_mutex_acquire(g_app->mutex, 0) != FuriStatusOk) return;

    if(g_app->packet_count < g_app->packet_capacity) {
        EspNowPacket* pkt = &g_app->packets[g_app->packet_count];
        memcpy(pkt->mac, info->src_addr, 6);
        size_t copy_len = (size_t)len > ESP_NOW_PKT_MAX_DATA ? ESP_NOW_PKT_MAX_DATA : (size_t)len;
        memcpy(pkt->data, data, copy_len);
        pkt->data_len = (uint8_t)copy_len;
        pkt->timestamp = xTaskGetTickCount();
        g_app->packet_count++;
    }

    furi_mutex_release(g_app->mutex);
}

static void esp_now_sniff_start(EspNowApp* app) {
    if(app->sniffing) return;

    ESP_LOGI(TAG, "Starting WiFi + ESP-NOW");

    esp_netif_init();
    esp_event_loop_create_default();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();

    esp_now_init();

    g_app = app;
    esp_now_register_recv_cb(esp_now_recv_cb);

    app->sniffing = true;
    ESP_LOGI(TAG, "ESP-NOW sniffing active");
}

static void esp_now_sniff_stop(EspNowApp* app) {
    if(!app->sniffing) return;

    ESP_LOGI(TAG, "Stopping ESP-NOW + WiFi");

    esp_now_unregister_recv_cb();
    g_app = NULL;

    esp_now_deinit();
    esp_wifi_stop();
    esp_wifi_deinit();
    esp_event_loop_delete_default();

    app->sniffing = false;
}

static void esp_now_scene_sniff_submenu_callback(void* context, uint32_t index) {
    EspNowApp* app = context;
    app->selected_index = index;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void esp_now_app_scene_sniff_on_enter(void* context) {
    EspNowApp* app = context;

    // Reset packet storage
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->packet_count = 0;
    app->last_displayed_count = 0;
    furi_mutex_release(app->mutex);

    submenu_set_header(app->submenu, "ESP-Now Sniffer");
    view_dispatcher_switch_to_view(app->view_dispatcher, EspNowViewSubmenu);

    esp_now_sniff_start(app);
}

bool esp_now_app_scene_sniff_on_event(void* context, SceneManagerEvent event) {
    EspNowApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        // Packet selected
        scene_manager_next_scene(app->scene_manager, EspNowAppScenePacketInfo);
        consumed = true;
    } else if(event.type == SceneManagerEventTypeTick) {
        // Check for new packets and update submenu
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        size_t count = app->packet_count;

        if(count > app->last_displayed_count) {
            for(size_t i = app->last_displayed_count; i < count; i++) {
                EspNowPacket* pkt = &app->packets[i];
                furi_string_printf(
                    app->text_buf,
                    "%02X:%02X:%02X:%02X:%02X:%02X (%d B)",
                    pkt->mac[0],
                    pkt->mac[1],
                    pkt->mac[2],
                    pkt->mac[3],
                    pkt->mac[4],
                    pkt->mac[5],
                    pkt->data_len);
                submenu_add_item(
                    app->submenu,
                    furi_string_get_cstr(app->text_buf),
                    i,
                    esp_now_scene_sniff_submenu_callback,
                    app);
            }
            app->last_displayed_count = count;
        }

        furi_mutex_release(app->mutex);
    }

    return consumed;
}

void esp_now_app_scene_sniff_on_exit(void* context) {
    EspNowApp* app = context;
    esp_now_sniff_stop(app);
    submenu_reset(app->submenu);
}
