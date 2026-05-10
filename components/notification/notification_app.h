/**
 * @file notification_app.h
 * @brief ESP32 notification app internals — exposes NotificationSettings and
 *        NotificationApp so that the settings UI can read/write them directly.
 *
 * Kept as close to the STM32 unleashed-firmware version as possible;
 * STM32-only fields (RGB backlight, contrast, lcd_inversion, led_brightness)
 * are omitted because the ESP32 boards lack that hardware.
 */
#pragma once

#include <furi.h>
#include "notification.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LayerInternal = 0,
    LayerNotification = 1,
    LayerMAX = 2,
} NotificationLedLayerIndex;

/* WS2812 ring effect modes (T-Embed Plus). Solid is the static settings.led_color.
 * Off is unconditional dark. The rest are animated and run on a periodic timer
 * whose interval comes from settings.led_speed_tick_ms.
 * Storage uses uint8_t for forward compat. */
typedef enum {
    LedEffectSolid     = 0,
    LedEffectOff       = 1,
    LedEffectBreathing = 2,  /* smooth pulse, ~2s/breath */
    LedEffectChase     = 3,  /* one bright pixel walking around with dim trail */
    LedEffectRainbow   = 4,  /* hue spatial spin around ring */
    LedEffectStrobe    = 5,  /* sharp on/off flashes */
    LedEffectCylon     = 6,  /* back-and-forth sweep with adjacent fade */
    LedEffectSparkle   = 7,  /* random pixels twinkle on dark base */
    LedEffectWipe      = 8,  /* color paints across the ring then clears */
    LedEffectCount     = 9,
} LedEffect;

typedef struct {
    uint8_t value[LayerMAX];
    NotificationLedLayerIndex index;
} NotificationDisplayLayer;

typedef struct {
    float display_brightness;
    uint32_t display_off_delay_ms;
    float speaker_volume;
    bool vibro_on;
    float night_shift;
    uint32_t night_shift_start;
    uint32_t night_shift_end;
    /* WS2812 RGB ring color (T-Embed Plus). Packed as 0x00RRGGBB at full
     * saturation per active channel. 0 = off. The actual hardware output is
     * (channel * led_brightness). */
    uint32_t led_color;
    /* WS2812 brightness multiplier (0.0 - 1.0). Multiplied into each
     * channel of led_color before the WS2812 push. Default 0.5 keeps the
     * 8-LED ring within ~240 mA peak draw — safe on hub-powered USB. */
    float led_brightness;
    /* Auto-off timeout for the WS2812 ring, in milliseconds.
     * 0 = always on. The ring re-lights with the last color on the next
     * input event. Mirrors display_off_delay_ms. */
    uint32_t led_off_delay_ms;
    /* LED animation effect (LedEffect enum stored as uint8_t). */
    uint8_t led_effect;
    /* Animation tick interval in milliseconds. Smaller = faster effects.
     * Default 33ms (~30fps). User picks from a fixed set in the settings UI. */
    uint32_t led_speed_tick_ms;
    /* UI Background tint (fg_color in the HAL — fills "unset" mono pixels =
     * the field around drawn UI elements). Index into ui_color_value[] in
     * notification.c. UI_COLOR_SPECTRUM_INDEX triggers a periodic HSV cycle.
     * Default = Orange (preserves stock Flipper look). */
    uint8_t ui_color_index;
    /* UI Foreground tint (bg_color in the HAL — fills "set" mono pixels =
     * the drawn UI elements: text, icons, borders). Same palette as
     * ui_color_index. Default = Black (preserves stock contrast). */
    uint8_t ui_fg_color_index;
} NotificationSettings;

struct NotificationApp {
    FuriMessageQueue* queue;
    FuriPubSub* event_record;
    FuriTimer* display_timer;
    FuriTimer* night_shift_timer;
    FuriTimer* night_shift_demo_timer;
    FuriTimer* led_off_timer;
    FuriTimer* led_effect_timer;
    FuriTimer* ui_spectrum_timer;

    NotificationDisplayLayer display;
    bool display_led_lock;

    NotificationSettings settings;
    float current_night_shift;
};

/** Save current settings (enqueues a save message to the notification thread). */
void notification_message_save_settings(NotificationApp* app);

/** Push settings.led_color to the WS2812 ring. Call after settings.led_color
 * changes so the LEDs reflect it without requiring a reboot. */
void notification_apply_led_color(NotificationApp* app);

/** Push settings.ui_color_index to the LCD foreground tint. For the Spectrum
 * index, starts/stops a periodic timer that cycles the hue. */
void notification_apply_ui_color(NotificationApp* app);

/** Night-shift timer control (used by settings app). */
void night_shift_timer_start(NotificationApp* app);
void night_shift_timer_stop(NotificationApp* app);

#ifdef __cplusplus
}
#endif
