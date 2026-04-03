#include "../wifi_app.h"
#include "../wifi_hal.h"
#include "../wifi_pcap.h"
#include "../views/sniffer_view.h"

#include <esp_wifi.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <storage/storage.h>
#include <string.h>

#define TAG WIFI_APP_LOG_TAG
#define SNIFFER_PKT_POOL_SIZE 32
#define SNIFFER_PKT_MAX_LEN 256

typedef struct {
    uint16_t len;
    uint16_t orig_len;
    uint32_t timestamp_us;
    uint8_t data[SNIFFER_PKT_MAX_LEN];
} SnifferPacket;

static SnifferPacket s_pkt_pool[SNIFFER_PKT_POOL_SIZE];
static volatile uint32_t s_write_idx = 0;
static volatile uint32_t s_read_idx = 0;
static File* s_pcap_file = NULL;
static Storage* s_storage = NULL;
static uint32_t s_start_time = 0;

// Promiscuous callback — runs in WiFi task context, must not block
static void sniffer_rx_callback(void* buf, wifi_promiscuous_pkt_type_t type) {
    UNUSED(type);
    const wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;

    uint32_t next = (s_write_idx + 1) % SNIFFER_PKT_POOL_SIZE;
    if(next == s_read_idx) return; // Pool full, drop packet

    SnifferPacket* slot = &s_pkt_pool[s_write_idx];
    uint16_t copy_len = pkt->rx_ctrl.sig_len;
    if(copy_len > SNIFFER_PKT_MAX_LEN) copy_len = SNIFFER_PKT_MAX_LEN;

    slot->len = copy_len;
    slot->orig_len = pkt->rx_ctrl.sig_len;
    slot->timestamp_us = pkt->rx_ctrl.timestamp;
    memcpy(slot->data, pkt->payload, copy_len);

    s_write_idx = next;
}

// Drain packet pool and write to PCAP — called from tick
static void sniffer_drain_packets(WifiApp* app) {
    while(s_read_idx != s_write_idx) {
        SnifferPacket* pkt = &s_pkt_pool[s_read_idx];

        if(s_pcap_file) {
            wifi_pcap_write_packet(s_pcap_file, pkt->timestamp_us, pkt->data, pkt->len);
        }

        app->sniffer_pkt_count++;
        app->sniffer_bytes += pkt->len;

        s_read_idx = (s_read_idx + 1) % SNIFFER_PKT_POOL_SIZE;
    }
}

void wifi_app_scene_sniffer_on_enter(void* context) {
    WifiApp* app = context;

    app->sniffer_running = false;
    app->sniffer_pkt_count = 0;
    app->sniffer_bytes = 0;
    app->sniffer_channel = 1;
    s_write_idx = 0;
    s_read_idx = 0;

    // Start WiFi
    if(!wifi_hal_is_started()) {
        wifi_hal_start();
    }

    // Open PCAP file
    s_storage = furi_record_open(RECORD_STORAGE);
    char path[64];
    // Find next available file number
    for(int i = 0; i < 999; i++) {
        snprintf(path, sizeof(path), "/ext/wifi/capture_%03d.pcap", i);
        if(!storage_common_stat(s_storage, path, NULL)) {
            continue; // file exists, try next
        }
        break; // file doesn't exist, use this
    }
    s_pcap_file = wifi_pcap_open(s_storage, path);

    // Init view
    SnifferViewModel* model = view_get_model(app->view_sniffer);
    model->packets = 0;
    model->bytes = 0;
    model->channel = app->sniffer_channel;
    model->elapsed_sec = 0;
    model->running = false;
    view_commit_model(app->view_sniffer, true);

    view_dispatcher_switch_to_view(app->view_dispatcher, WifiAppViewSniffer);
}

bool wifi_app_scene_sniffer_on_event(void* context, SceneManagerEvent event) {
    WifiApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == InputKeyOk) {
            // Toggle sniffer
            if(!app->sniffer_running) {
                app->sniffer_running = true;
                s_start_time = (uint32_t)(esp_timer_get_time() / 1000000);
                wifi_hal_set_channel(app->sniffer_channel);
                wifi_hal_set_promiscuous(true, sniffer_rx_callback);
                ESP_LOGI(TAG, "Sniffer started on ch%d", app->sniffer_channel);
            } else {
                app->sniffer_running = false;
                wifi_hal_set_promiscuous(false, NULL);
                sniffer_drain_packets(app);
                ESP_LOGI(TAG, "Sniffer stopped");
            }
            consumed = true;
        } else if(event.event == InputKeyUp) {
            app->sniffer_channel++;
            if(app->sniffer_channel > 13) app->sniffer_channel = 1;
            if(app->sniffer_running) {
                wifi_hal_set_promiscuous(false, NULL);
                wifi_hal_set_channel(app->sniffer_channel);
                furi_delay_ms(20);
                wifi_hal_set_promiscuous(true, sniffer_rx_callback);
            }
            consumed = true;
        } else if(event.event == InputKeyDown) {
            if(app->sniffer_channel > 1)
                app->sniffer_channel--;
            else
                app->sniffer_channel = 13;
            if(app->sniffer_running) {
                wifi_hal_set_promiscuous(false, NULL);
                wifi_hal_set_channel(app->sniffer_channel);
                furi_delay_ms(20);
                wifi_hal_set_promiscuous(true, sniffer_rx_callback);
            }
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        if(app->sniffer_running) {
            sniffer_drain_packets(app);
        }
        SnifferViewModel* model = view_get_model(app->view_sniffer);
        model->packets = app->sniffer_pkt_count;
        model->bytes = app->sniffer_bytes;
        model->channel = app->sniffer_channel;
        model->running = app->sniffer_running;
        if(app->sniffer_running) {
            model->elapsed_sec = (uint32_t)(esp_timer_get_time() / 1000000) - s_start_time;
        }
        view_commit_model(app->view_sniffer, true);
    }

    return consumed;
}

void wifi_app_scene_sniffer_on_exit(void* context) {
    WifiApp* app = context;

    if(app->sniffer_running) {
        app->sniffer_running = false;
        wifi_hal_set_promiscuous(false, NULL);
        sniffer_drain_packets(app);
    }

    if(s_pcap_file) {
        wifi_pcap_close(s_pcap_file);
        s_pcap_file = NULL;
    }
    if(s_storage) {
        furi_record_close(RECORD_STORAGE);
        s_storage = NULL;
    }
}
