/**
 * @file notification_settings_app.c
 * @brief ESP32 notification settings — adapted from STM32 unleashed-firmware.
 *
 * STM32-only features removed: LCD contrast, LCD inversion, LED brightness,
 * RGB backlight. Everything else kept as close to the original as possible.
 */
#include <furi.h>
#include <furi_hal_rtc.h>
#include <furi_hal_light.h>
#include <notification/notification_app.h>
#include <notification/notification_messages.h>
#include <gui/modules/variable_item_list.h>
#include <gui/view_dispatcher.h>
#include <lib/toolbox/value_index.h>

typedef struct {
    NotificationApp* notification;
    ViewDispatcher* view_dispatcher;
    VariableItemList* variable_item_list;
    /* Direct refs so the night_shift lock callback doesn't depend on a
     * hardcoded index — the menu shape changes per board (LED items gated
     * on BOARD_HAS_RGB_LED) and per future feature additions. */
    VariableItem* night_shift_start_item;
    VariableItem* night_shift_end_item;
} NotificationAppSettings;

static const NotificationSequence sequence_note_c = {
    &message_note_c5,
    &message_delay_100,
    &message_sound_off,
    NULL,
};

#define BACKLIGHT_COUNT 17
const char* const backlight_text[BACKLIGHT_COUNT] = {
    "20%", "25%", "30%", "35%", "40%", "45%", "50%",
    "55%", "60%", "65%", "70%", "75%", "80%", "85%", "90%", "95%", "100%",
};
const float backlight_value[BACKLIGHT_COUNT] = {
    0.20f, 0.25f, 0.30f, 0.35f, 0.40f, 0.45f, 0.50f,
    0.55f, 0.60f, 0.65f, 0.70f, 0.75f, 0.80f, 0.85f, 0.90f, 0.95f, 1.00f,
};

#define VOLUME_COUNT 21
const char* const volume_text[VOLUME_COUNT] = {
    "0%",  "5%",  "10%", "15%", "20%", "25%", "30%", "35%", "40%", "45%",  "50%",
    "55%", "60%", "65%", "70%", "75%", "80%", "85%", "90%", "95%", "100%",
};
const float volume_value[VOLUME_COUNT] = {
    0.00f, 0.05f, 0.10f, 0.15f, 0.20f, 0.25f, 0.30f, 0.35f, 0.40f, 0.45f, 0.50f,
    0.55f, 0.60f, 0.65f, 0.70f, 0.75f, 0.80f, 0.85f, 0.90f, 0.95f, 1.00f,
};

#define DELAY_COUNT 12
const char* const delay_text[DELAY_COUNT] = {
    "Always ON",
    "2s",
    "5s",
    "10s",
    "15s",
    "30s",
    "60s",
    "90s",
    "120s",
    "5min",
    "10min",
    "30min",
};
const uint32_t delay_value[DELAY_COUNT] =
    {0, 2000, 5000, 10000, 15000, 30000, 60000, 90000, 120000, 300000, 600000, 1800000};


// --- NIGHT SHIFT ---
#define NIGHT_SHIFT_COUNT 7
const char* const night_shift_text[NIGHT_SHIFT_COUNT] =
    {"OFF", "-10%", "-20%", "-30%", "-40%", "-50%", "-60%"};
const float night_shift_value[NIGHT_SHIFT_COUNT] = {
    1.0f, 0.9f, 0.8f, 0.7f, 0.6f, 0.5f, 0.4f,
};

#define NIGHT_SHIFT_START_COUNT 14
const char* const night_shift_start_text[NIGHT_SHIFT_START_COUNT] = {
    "17:00", "17:30", "18:00", "18:30", "19:00", "19:30", "20:00",
    "20:30", "21:00", "21:30", "22:00", "22:30", "23:00", "23:30",
};
const uint32_t night_shift_start_value[NIGHT_SHIFT_START_COUNT] = {
    1020, 1050, 1080, 1110, 1140, 1170, 1200,
    1230, 1260, 1290, 1320, 1350, 1380, 1410,
};

#define NIGHT_SHIFT_END_COUNT 14
const char* const night_shift_end_text[NIGHT_SHIFT_END_COUNT] = {
    "05:00", "05:30", "06:00", "06:30", "07:00", "07:30", "08:00",
    "08:30", "09:00", "09:30", "10:00", "10:30", "11:00", "11:30",
};
const uint32_t night_shift_end_value[NIGHT_SHIFT_END_COUNT] = {
    300, 330, 360, 390, 410, 440, 470,
    500, 530, 560, 590, 620, 650, 680,
};
// --- NIGHT SHIFT END ---

static void backlight_changed(VariableItem* item) {
    NotificationAppSettings* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);

    variable_item_set_current_value_text(item, backlight_text[index]);
    app->notification->settings.display_brightness = backlight_value[index];

    notification_message(app->notification, &sequence_display_backlight_force_on);
}

static void screen_changed(VariableItem* item) {
    NotificationAppSettings* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);

    variable_item_set_current_value_text(item, delay_text[index]);
    app->notification->settings.display_off_delay_ms = delay_value[index];

    if((delay_value[index] == 0) & (furi_timer_is_running(app->notification->display_timer))) {
        furi_timer_stop(app->notification->display_timer);
    }
    notification_message(app->notification, &sequence_display_backlight_on);
}

static void volume_changed(VariableItem* item) {
    NotificationAppSettings* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);

    variable_item_set_current_value_text(item, volume_text[index]);
    app->notification->settings.speaker_volume = volume_value[index];
    notification_message(app->notification, &sequence_note_c);
}

// --- NIGHT SHIFT ---

static void night_shift_changed(VariableItem* item) {
    NotificationAppSettings* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);

    variable_item_set_current_value_text(item, night_shift_text[index]);
    app->notification->settings.night_shift = night_shift_value[index];

    /* Lock/unlock Start + End via the stored pointers — index-free, so the
     * lock works regardless of how the menu is shuffled by board-gating
     * (LED items only appear when BOARD_HAS_RGB_LED). */
    if(app->night_shift_start_item) {
        variable_item_set_locked(app->night_shift_start_item, index == 0, "Night Shift\nOFF!");
    }
    if(app->night_shift_end_item) {
        variable_item_set_locked(app->night_shift_end_item, index == 0, "Night Shift\nOFF!");
    }

    /* Demo the night shift brightness briefly */
    app->notification->current_night_shift = night_shift_value[index];
    notification_message(app->notification, &sequence_display_backlight_force_on);

    if(night_shift_value[index] != 1) {
        night_shift_timer_start(app->notification);
        if(furi_timer_is_running(app->notification->night_shift_demo_timer)) {
            furi_timer_stop(app->notification->night_shift_demo_timer);
        }
        furi_timer_start(app->notification->night_shift_demo_timer, furi_ms_to_ticks(1200));
    } else {
        night_shift_timer_stop(app->notification);
        if(furi_timer_is_running(app->notification->night_shift_demo_timer))
            furi_timer_stop(app->notification->night_shift_demo_timer);
    }

    notification_message_save_settings(app->notification);
}

static void night_shift_start_changed(VariableItem* item) {
    NotificationAppSettings* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);

    variable_item_set_current_value_text(item, night_shift_start_text[index]);
    app->notification->settings.night_shift_start = night_shift_start_value[index];

    notification_message_save_settings(app->notification);
}

static void night_shift_end_changed(VariableItem* item) {
    NotificationAppSettings* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);

    variable_item_set_current_value_text(item, night_shift_end_text[index]);
    app->notification->settings.night_shift_end = night_shift_end_value[index];

    notification_message_save_settings(app->notification);
}

// --- NIGHT SHIFT END ---

// --- LED COLOR (T-Embed Plus WS2812 ring) ---
#define LED_COLOR_COUNT 10
const char* const led_color_text[LED_COLOR_COUNT] = {
    "Off",
    "Red",
    "Green",
    "Blue",
    "Cyan",
    "Magenta",
    "Yellow",
    "White",
    "Orange",
    "Purple",
};
/* Each value is packed 0x00RRGGBB at full saturation per active channel.
 * The actual hardware output = (channel * settings.led_brightness), so the
 * brightness slider is the single dial controlling current draw and
 * perceived intensity. Purple uses 0x80 R + 0xFF B for a perceptually
 * correct violet that doesn't look like plain blue. */
const uint32_t led_color_value[LED_COLOR_COUNT] = {
    0x00000000, /* Off */
    0x00FF0000, /* Red */
    0x0000FF00, /* Green */
    0x000000FF, /* Blue */
    0x0000FFFF, /* Cyan */
    0x00FF1DCE, /* Magenta — #FF1DCE */
    0x00FFFF00, /* Yellow — #FFFF00 */
    0x00FFFFFF, /* White */
    0x00FFA500, /* Orange — #FFA500 */
    0x008F00FF, /* Purple — #8F00FF */
};

static void led_color_changed(VariableItem* item) {
    NotificationAppSettings* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, led_color_text[index]);
    app->notification->settings.led_color = led_color_value[index];
    notification_apply_led_color(app->notification);     /* live preview */
    notification_message_save_settings(app->notification); /* persist */
}

/* LED brightness has its own table with much finer low-end resolution because
 * WS2812 LEDs are very bright at low duty cycles. The notification.c apply
 * function cubes this value (cubic gamma correction) to match perceptual
 * brightness on the T-Embed Plus, so user-set 1%–10% all clamp to off,
 * 20% drives ~0.8%, 30% drives ~2.7%, 50% drives 12.5%, 100% stays 100%. */
#define LED_BRIGHTNESS_COUNT 20
const char* const led_brightness_text[LED_BRIGHTNESS_COUNT] = {
    "1%", "2%", "5%", "10%", "15%", "20%", "25%", "30%", "35%", "40%",
    "45%", "50%", "60%", "70%", "75%", "80%", "85%", "90%", "95%", "100%",
};
const float led_brightness_value[LED_BRIGHTNESS_COUNT] = {
    0.01f, 0.02f, 0.05f, 0.10f, 0.15f, 0.20f, 0.25f, 0.30f, 0.35f, 0.40f,
    0.45f, 0.50f, 0.60f, 0.70f, 0.75f, 0.80f, 0.85f, 0.90f, 0.95f, 1.00f,
};

static void led_brightness_changed(VariableItem* item) {
    NotificationAppSettings* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, led_brightness_text[index]);
    app->notification->settings.led_brightness = led_brightness_value[index];
    notification_apply_led_color(app->notification);     /* live preview */
    notification_message_save_settings(app->notification); /* persist */
}

static void led_timeout_changed(VariableItem* item) {
    NotificationAppSettings* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    /* Reuse the same DELAY_COUNT table as the backlight timeout. */
    variable_item_set_current_value_text(item, delay_text[index]);
    app->notification->settings.led_off_delay_ms = delay_value[index];
    notification_apply_led_color(app->notification);     /* re-arms timer with new value */
    notification_message_save_settings(app->notification); /* persist */
}

#define LED_EFFECT_COUNT 9
const char* const led_effect_text[LED_EFFECT_COUNT] = {
    "Solid",     /* 0 */
    "Off",       /* 1 */
    "Breathing", /* 2 */
    "Chase",     /* 3 */
    "Rainbow",   /* 4 */
    "Strobe",    /* 5 */
    "Cylon",     /* 6 */
    "Sparkle",   /* 7 */
    "Wipe",      /* 8 */
};

static void led_effect_changed(VariableItem* item) {
    NotificationAppSettings* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, led_effect_text[index]);
    app->notification->settings.led_effect = index;
    notification_apply_led_color(app->notification);     /* live preview */
    notification_message_save_settings(app->notification); /* persist */
}

#define LED_SPEED_COUNT 5
const char* const led_speed_text[LED_SPEED_COUNT] = {
    "Slowest", "Slow", "Normal", "Fast", "Fastest",
};
const uint32_t led_speed_value[LED_SPEED_COUNT] = {
    100, 60, 33, 16, 8, /* milliseconds per animation tick */
};

static void led_speed_changed(VariableItem* item) {
    NotificationAppSettings* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, led_speed_text[index]);
    app->notification->settings.led_speed_tick_ms = led_speed_value[index];
    notification_apply_led_color(app->notification);     /* restart effect at new speed */
    notification_message_save_settings(app->notification); /* persist */
}
// --- LED COLOR END ---

// --- UI COLOR (LCD bichromatic palette) ---
/* The UI color presets — order matches ui_color_value[] in notification.c.
 * Last entry "Spectrum" is a sentinel; selecting it kicks off the periodic
 * hue-cycle timer in the notification service. Used by both
 * "UI Background" (fg_color = field around UI elements, default Orange) and
 * "UI Foreground" (bg_color = drawn UI elements, default Black). */
#define UI_COLOR_COUNT 11
const char* const ui_color_text[UI_COLOR_COUNT] = {
    "Black", "Orange", "Red", "Green", "Blue", "Cyan",
    "Magenta", "Yellow", "White", "Purple", "Spectrum",
};

static void ui_bg_color_changed(VariableItem* item) {
    NotificationAppSettings* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, ui_color_text[index]);
    app->notification->settings.ui_color_index = index;
    notification_apply_ui_color(app->notification);     /* live preview */
    notification_message_save_settings(app->notification); /* persist */
}

static void ui_fg_color_changed(VariableItem* item) {
    NotificationAppSettings* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, ui_color_text[index]);
    app->notification->settings.ui_fg_color_index = index;
    notification_apply_ui_color(app->notification);     /* live preview */
    notification_message_save_settings(app->notification); /* persist */
}
// --- UI COLOR END ---

static uint32_t notification_app_settings_exit(void* context) {
    UNUSED(context);
    return VIEW_NONE;
}

static NotificationAppSettings* alloc_settings(void) {
    NotificationAppSettings* app = malloc(sizeof(NotificationAppSettings));
    app->night_shift_start_item = NULL;
    app->night_shift_end_item = NULL;
    app->notification = furi_record_open(RECORD_NOTIFICATION);

    app->variable_item_list = variable_item_list_alloc();
    View* view = variable_item_list_get_view(app->variable_item_list);
    view_set_previous_callback(view, notification_app_settings_exit);

    VariableItem* item;
    uint8_t value_index;

    /* LCD Backlight brightness */
    item = variable_item_list_add(
        app->variable_item_list, "LCD Backlight", BACKLIGHT_COUNT, backlight_changed, app);
    value_index = value_index_float(
        app->notification->settings.display_brightness, backlight_value, BACKLIGHT_COUNT);
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, backlight_text[value_index]);

    /* UI Background tint — fills the field around drawn UI elements
     * (default Orange). Last preset is "Spectrum" → cycles HSV via timer. */
    item = variable_item_list_add(
        app->variable_item_list, "UI Background", UI_COLOR_COUNT, ui_bg_color_changed, app);
    value_index = app->notification->settings.ui_color_index < UI_COLOR_COUNT
                      ? app->notification->settings.ui_color_index : 1;
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, ui_color_text[value_index]);

    /* UI Foreground tint — fills the drawn UI elements themselves: text,
     * icons, borders (default Black). When both UI colors are Spectrum, the
     * foreground hue is offset 180° from the background for legibility. */
    item = variable_item_list_add(
        app->variable_item_list, "UI Foreground", UI_COLOR_COUNT, ui_fg_color_changed, app);
    value_index = app->notification->settings.ui_fg_color_index < UI_COLOR_COUNT
                      ? app->notification->settings.ui_fg_color_index : 0;
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, ui_color_text[value_index]);

    /* WS2812 ring controls — only added when the board actually has a ring
     * (furi_hal_light_pixel_count() > 0). Boards without a ring (Waveshare
     * C6 1.9, C6 1.47) get a clean menu with no inert sliders. */
    if(furi_hal_light_pixel_count() > 0) {
    /* RGB LED ring color (T-Embed Plus WS2812). Pick the preset that matches
     * the saved settings.led_color, falling back to "Off" if no match. */
    item = variable_item_list_add(
        app->variable_item_list, "LED Color", LED_COLOR_COUNT, led_color_changed, app);
    value_index = 0;
    for(uint8_t i = 0; i < LED_COLOR_COUNT; i++) {
        if(app->notification->settings.led_color == led_color_value[i]) {
            value_index = i;
            break;
        }
    }
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, led_color_text[value_index]);

    /* LED ring brightness — uses a fine-grained 1-100% table; the apply
     * function applies gamma correction so user-perceived brightness scales
     * naturally with the slider. */
    item = variable_item_list_add(
        app->variable_item_list, "LED Brightness", LED_BRIGHTNESS_COUNT, led_brightness_changed, app);
    value_index = value_index_float(
        app->notification->settings.led_brightness, led_brightness_value, LED_BRIGHTNESS_COUNT);
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, led_brightness_text[value_index]);

    /* LED idle-off timeout (Always ON / 2s / 5s / ... / 30min, same scale as backlight) */
    item = variable_item_list_add(
        app->variable_item_list, "LED Timeout", DELAY_COUNT, led_timeout_changed, app);
    value_index = value_index_uint32(
        app->notification->settings.led_off_delay_ms, delay_value, DELAY_COUNT);
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, delay_text[value_index]);

    /* LED animation effect */
    item = variable_item_list_add(
        app->variable_item_list, "LED Effect", LED_EFFECT_COUNT, led_effect_changed, app);
    value_index = app->notification->settings.led_effect < LED_EFFECT_COUNT
                      ? app->notification->settings.led_effect : 0;
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, led_effect_text[value_index]);

    /* LED animation speed (ms per tick) */
    item = variable_item_list_add(
        app->variable_item_list, "LED Speed", LED_SPEED_COUNT, led_speed_changed, app);
    value_index = value_index_uint32(
        app->notification->settings.led_speed_tick_ms, led_speed_value, LED_SPEED_COUNT);
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, led_speed_text[value_index]);
    } /* end if(pixel_count > 0) */

    /* Backlight timeout */
    item = variable_item_list_add(
        app->variable_item_list, "Backlight Time", DELAY_COUNT, screen_changed, app);
    value_index = value_index_uint32(
        app->notification->settings.display_off_delay_ms, delay_value, DELAY_COUNT);
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, delay_text[value_index]);

    /* --- NIGHT SHIFT --- */
    item = variable_item_list_add(
        app->variable_item_list, "Night Shift", NIGHT_SHIFT_COUNT, night_shift_changed, app);
    value_index = value_index_float(
        app->notification->settings.night_shift, night_shift_value, NIGHT_SHIFT_COUNT);
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, night_shift_text[value_index]);

    item = variable_item_list_add(
        app->variable_item_list,
        " . Start",
        NIGHT_SHIFT_START_COUNT,
        night_shift_start_changed,
        app);
    value_index = value_index_uint32(
        app->notification->settings.night_shift_start,
        night_shift_start_value,
        NIGHT_SHIFT_START_COUNT);
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, night_shift_start_text[value_index]);
    variable_item_set_locked(
        item, (app->notification->settings.night_shift == 1), "Night Shift \nOFF!");
    app->night_shift_start_item = item;

    item = variable_item_list_add(
        app->variable_item_list, " . End", NIGHT_SHIFT_END_COUNT, night_shift_end_changed, app);
    value_index = value_index_uint32(
        app->notification->settings.night_shift_end, night_shift_end_value, NIGHT_SHIFT_END_COUNT);
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, night_shift_end_text[value_index]);
    variable_item_set_locked(
        item, (app->notification->settings.night_shift == 1), "Night Shift \nOFF!");
    app->night_shift_end_item = item;
    /* --- NIGHT SHIFT END --- */

    /* Volume (respects stealth mode) */
    if(furi_hal_rtc_is_flag_set(FuriHalRtcFlagStealthMode)) {
        item = variable_item_list_add(app->variable_item_list, "Volume", 1, NULL, app);
        variable_item_set_current_value_index(item, 0);
        variable_item_set_current_value_text(item, "Stealth");
    } else {
        item = variable_item_list_add(
            app->variable_item_list, "Volume", VOLUME_COUNT, volume_changed, app);
        value_index = value_index_float(
            app->notification->settings.speaker_volume, volume_value, VOLUME_COUNT);
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, volume_text[value_index]);
    }

    Gui* gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_attach_to_gui(app->view_dispatcher, gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_add_view(app->view_dispatcher, 0, view);
    view_dispatcher_switch_to_view(app->view_dispatcher, 0);
    return app;
}

static void free_settings(NotificationAppSettings* app) {
    view_dispatcher_remove_view(app->view_dispatcher, 0);
    variable_item_list_free(app->variable_item_list);
    view_dispatcher_free(app->view_dispatcher);

    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    free(app);
}

int32_t notification_settings_app(void* p) {
    UNUSED(p);
    NotificationAppSettings* app = alloc_settings();
    view_dispatcher_run(app->view_dispatcher);
    notification_message_save_settings(app->notification);
    free_settings(app);
    return 0;
}
