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
} NotificationSettings;

struct NotificationApp {
    FuriMessageQueue* queue;
    FuriPubSub* event_record;
    FuriTimer* display_timer;
    FuriTimer* night_shift_timer;
    FuriTimer* night_shift_demo_timer;

    NotificationDisplayLayer display;
    bool display_led_lock;

    NotificationSettings settings;
    float current_night_shift;
};

/** Save current settings (enqueues a save message to the notification thread). */
void notification_message_save_settings(NotificationApp* app);

/** Night-shift timer control (used by settings app). */
void night_shift_timer_start(NotificationApp* app);
void night_shift_timer_stop(NotificationApp* app);

#ifdef __cplusplus
}
#endif
