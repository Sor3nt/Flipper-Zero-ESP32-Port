#include "../wlan_app.h"
#include "../wlan_hal.h"

static const char* mode_label(uint8_t mode) {
    switch(mode) {
    case WlanHalBeaconModeFunny: return "Funny";
    case WlanHalBeaconModeRickroll: return "Rickroll";
    case WlanHalBeaconModeRandom: return "Random";
    case WlanHalBeaconModeCustom: return "Custom";
    default: return "?";
    }
}

static void ssid_spam_run_rebuild(WlanApp* app, uint32_t frames) {
    widget_reset(app->widget);
    widget_add_string_element(
        app->widget, 64, 8, AlignCenter, AlignTop, FontPrimary, "SSID Spam");

    char mode_buf[32];
    snprintf(mode_buf, sizeof(mode_buf), "Mode: %s", mode_label(app->beacon_mode));
    widget_add_string_element(
        app->widget, 64, 24, AlignCenter, AlignTop, FontSecondary, mode_buf);

    char frames_buf[32];
    snprintf(frames_buf, sizeof(frames_buf), "Frames: %lu", (unsigned long)frames);
    widget_add_string_element(
        app->widget, 64, 38, AlignCenter, AlignTop, FontSecondary, frames_buf);

    const char* status =
        wlan_hal_beacon_spam_is_running() ? "Spamming..." : "Stopped";
    widget_add_string_element(
        app->widget, 64, 52, AlignCenter, AlignTop, FontSecondary, status);
}

void wlan_app_scene_ssid_spam_run_on_enter(void* context) {
    WlanApp* app = context;
    wlan_hal_beacon_spam_start(
        (WlanHalBeaconMode)app->beacon_mode,
        app->beacon_custom_ssid);
    ssid_spam_run_rebuild(app, 0);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewWidget);
}

bool wlan_app_scene_ssid_spam_run_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeTick) {
        // Widget alle ~250 ms mit aktuellem Frame-Counter neu zeichnen.
        ssid_spam_run_rebuild(app, wlan_hal_beacon_spam_get_frame_count());
    }
    return consumed;
}

void wlan_app_scene_ssid_spam_run_on_exit(void* context) {
    WlanApp* app = context;
    wlan_hal_beacon_spam_stop();
    widget_reset(app->widget);
}
