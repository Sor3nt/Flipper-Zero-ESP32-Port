/**
 * @file notification.c
 * @brief ESP32 notification service with STM32-style backlight handling.
 */

#include <furi.h>
#include <furi_hal_display.h>
#include <furi_hal_rtc.h>
#include <furi_hal_speaker.h>
#include <input.h>
#include <saved_struct.h>
#include <storage/storage.h>
#include "notification_app.h"
#include "notification_messages.h"

#define TAG "NotificationSrv"

#define NOTIFICATION_EVENT_COMPLETE 0x00000001U

#define NOTIFICATION_SETTINGS_PATH    INT_PATH(".notification.settings")
#define NOTIFICATION_SETTINGS_MAGIC   0x42
#define NOTIFICATION_SETTINGS_VERSION 0x01

typedef enum {
    NotificationLayerMessage,
    InternalLayerMessage,
    SaveSettingsMessage,
} NotificationAppMessageType;

typedef struct {
    const NotificationSequence* sequence;
    NotificationAppMessageType type;
    FuriEventFlag* back_event;
} NotificationAppMessage;

static bool lcd_backlight_is_on = false;

static uint8_t notification_clamp_u8(float value) {
    if(value <= 0.0f) return 0;
    if(value >= 255.0f) return UINT8_MAX;

    return (uint8_t)value;
}

static uint8_t notification_scale_display_brightness(
    NotificationApp* app,
    uint8_t value,
    float display_brightness_setting) {
    return notification_clamp_u8(value * display_brightness_setting * app->current_night_shift);
}

static uint8_t notification_settings_get_display_brightness(NotificationApp* app, uint8_t value) {
    return notification_clamp_u8(value * app->settings.display_brightness);
}

static uint32_t notification_settings_display_off_delay_ticks(NotificationApp* app) {
    return (float)(app->settings.display_off_delay_ms) /
           (1000.0f / furi_kernel_get_tick_frequency());
}

static void notification_apply_internal_display_layer(NotificationApp* app, uint8_t layer_value) {
    furi_assert(app);

    app->display.value[LayerInternal] = layer_value;

    if(app->display.index == LayerInternal) {
        furi_hal_display_set_backlight(app->display.value[LayerInternal]);
    }
}

static void notification_apply_notification_display_layer(NotificationApp* app, uint8_t layer_value) {
    furi_assert(app);

    app->display.index = LayerNotification;
    app->display.value[LayerNotification] = layer_value;

    if(lcd_backlight_is_on) return;

    furi_hal_display_set_backlight(app->display.value[LayerNotification]);
}

static void notification_reset_notification_display_layer(NotificationApp* app) {
    furi_assert(app);

    app->display.value[LayerNotification] = 0;
    app->display.index = LayerInternal;

    furi_hal_display_set_backlight(app->display.value[LayerInternal]);
}

static void notification_reset_notification_layer(
    NotificationApp* app,
    bool reset_display,
    float display_brightness_setting) {
    if(!reset_display) return;

    if(display_brightness_setting != app->settings.display_brightness) {
        furi_hal_display_set_backlight(
            notification_clamp_u8(
                app->settings.display_brightness * 255.0f * app->current_night_shift));
    }

    if(app->settings.display_off_delay_ms > 0) {
        furi_timer_start(app->display_timer, notification_settings_display_off_delay_ticks(app));
    }
}

static void notification_display_timer(void* context) {
    furi_assert(context);
    NotificationApp* app = context;
    notification_message(app, &sequence_display_backlight_off);
}

static void night_shift_demo_timer_callback(void* context) {
    furi_assert(context);
    NotificationApp* app = context;
    notification_message(app, &sequence_display_backlight_force_on);
}

void night_shift_timer_start(NotificationApp* app) {
    if(app->settings.night_shift != 1.0f) {
        if(furi_timer_is_running(app->night_shift_timer)) {
            furi_timer_stop(app->night_shift_timer);
        }
        furi_timer_start(app->night_shift_timer, furi_ms_to_ticks(1000));
    }
}

void night_shift_timer_stop(NotificationApp* app) {
    if(furi_timer_is_running(app->night_shift_timer)) {
        furi_timer_stop(app->night_shift_timer);
    }
}

static void night_shift_timer_callback(void* context) {
    furi_assert(context);
    NotificationApp* app = context;
    DateTime current_date_time;

    furi_hal_rtc_get_datetime(&current_date_time);
    uint32_t time = current_date_time.hour * 60 + current_date_time.minute;

    if((time > app->settings.night_shift_end) && (time < app->settings.night_shift_start)) {
        app->current_night_shift = 1.0f;
    } else {
        app->current_night_shift = app->settings.night_shift;
    }
}

static void notification_process_notification_message(
    NotificationApp* app,
    NotificationAppMessage* message) {
    uint32_t notification_message_index = 0;
    const NotificationMessage* notification_message = (*message->sequence)[notification_message_index];

    bool reset_notifications = true;
    float display_brightness_setting = app->settings.display_brightness;
    float speaker_volume_setting = app->settings.speaker_volume;
    bool force_volume = false;
    bool reset_display = false;

    while(notification_message != NULL) {
        switch(notification_message->type) {
        case NotificationMessageTypeLedDisplayBacklight:
            if(notification_message->data.led.value > 0x00) {
                notification_apply_notification_display_layer(
                    app,
                    notification_scale_display_brightness(
                        app,
                        notification_message->data.led.value,
                        display_brightness_setting));
                reset_display = true;
                lcd_backlight_is_on = true;
            } else {
                reset_display = false;
                notification_reset_notification_display_layer(app);
                lcd_backlight_is_on = false;

                if(furi_timer_is_running(app->display_timer)) {
                    furi_timer_stop(app->display_timer);
                }
            }
            break;
        case NotificationMessageTypeLedDisplayBacklightForceOn:
            lcd_backlight_is_on = false;
            notification_apply_notification_display_layer(
                app,
                notification_scale_display_brightness(
                    app,
                    notification_message->data.led.value,
                    display_brightness_setting));
            reset_display = true;
            lcd_backlight_is_on = true;
            break;
        case NotificationMessageTypeLedDisplayBacklightEnforceOn:
            if(!app->display_led_lock) {
                app->display_led_lock = true;
                notification_apply_internal_display_layer(
                    app,
                    notification_scale_display_brightness(
                        app,
                        notification_message->data.led.value,
                        display_brightness_setting));
                lcd_backlight_is_on = true;
            }
            break;
        case NotificationMessageTypeLedDisplayBacklightEnforceAuto:
            if(app->display_led_lock) {
                app->display_led_lock = false;
                notification_apply_internal_display_layer(
                    app,
                    notification_scale_display_brightness(
                        app,
                        notification_message->data.led.value,
                        display_brightness_setting));
            } else {
                FURI_LOG_E(TAG, "Incorrect BacklightEnforceAuto usage");
            }
            break;
        case NotificationMessageTypeSoundOn:
            if(!furi_hal_rtc_is_flag_set(FuriHalRtcFlagStealthMode) || force_volume) {
                float vol = notification_message->data.sound.volume * speaker_volume_setting;
                if(furi_hal_speaker_is_mine() || furi_hal_speaker_acquire(30)) {
                    furi_hal_speaker_start(
                        notification_message->data.sound.frequency, vol);
                }
            }
            break;
        case NotificationMessageTypeSoundOff:
            if(furi_hal_speaker_is_mine()) {
                furi_hal_speaker_stop();
                furi_hal_speaker_release();
            }
            break;
        case NotificationMessageTypeDelay:
            furi_delay_ms(notification_message->data.delay.length);
            break;
        case NotificationMessageTypeDoNotReset:
            reset_notifications = false;
            break;
        case NotificationMessageTypeForceSpeakerVolumeSetting:
            speaker_volume_setting = notification_message->data.forced_settings.speaker_volume;
            force_volume = true;
            break;
        case NotificationMessageTypeForceDisplayBrightnessSetting:
            display_brightness_setting =
                notification_message->data.forced_settings.display_brightness;
            break;
        default:
            break;
        }

        notification_message_index++;
        notification_message = (*message->sequence)[notification_message_index];
    }

    if(reset_notifications) {
        notification_reset_notification_layer(app, reset_display, display_brightness_setting);
    }
}

static void notification_process_internal_message(
    NotificationApp* app,
    NotificationAppMessage* message) {
    uint32_t notification_message_index = 0;
    const NotificationMessage* notification_message = (*message->sequence)[notification_message_index];

    while(notification_message != NULL) {
        switch(notification_message->type) {
        case NotificationMessageTypeLedDisplayBacklight:
            notification_apply_internal_display_layer(
                app,
                notification_settings_get_display_brightness(
                    app, notification_message->data.led.value));
            break;
        default:
            break;
        }

        notification_message_index++;
        notification_message = (*message->sequence)[notification_message_index];
    }
}

static void input_event_callback(const void* value, void* context) {
    furi_assert(value);
    furi_assert(context);

    NotificationApp* app = context;
    notification_message(app, &sequence_display_backlight_on);
}

static void notification_message_send(
    NotificationApp* app,
    NotificationAppMessageType type,
    const NotificationSequence* sequence,
    FuriEventFlag* back_event) {
    furi_assert(app);
    furi_assert(sequence);

    NotificationAppMessage message = {
        .sequence = sequence,
        .type = type,
        .back_event = back_event,
    };

    furi_check(furi_message_queue_put(app->queue, &message, FuriWaitForever) == FuriStatusOk);
}

static bool notification_load_settings(NotificationApp* app) {
    NotificationSettings tmp;
    bool ok = saved_struct_load(
        NOTIFICATION_SETTINGS_PATH,
        &tmp,
        sizeof(NotificationSettings),
        NOTIFICATION_SETTINGS_MAGIC,
        NOTIFICATION_SETTINGS_VERSION);
    if(ok) {
        app->settings = tmp;
        FURI_LOG_I(
            TAG,
            "LOAD ok: brightness=%.2f vol=%.2f delay_ms=%u",
            (double)app->settings.display_brightness,
            (double)app->settings.speaker_volume,
            (unsigned)app->settings.display_off_delay_ms);
    } else {
        FURI_LOG_W(TAG, "LOAD failed — using defaults");
    }
    /* Sanity: never let a 0ms delay sneak through and instantly blank the screen. */
    if(app->settings.display_off_delay_ms < 2000) app->settings.display_off_delay_ms = 2000;
    return ok;
}

static bool notification_save_settings(NotificationApp* app) {
    bool ok = saved_struct_save(
        NOTIFICATION_SETTINGS_PATH,
        &app->settings,
        sizeof(NotificationSettings),
        NOTIFICATION_SETTINGS_MAGIC,
        NOTIFICATION_SETTINGS_VERSION);
    if(ok) {
        FURI_LOG_I(
            TAG,
            "SAVE ok: brightness=%.2f vol=%.2f",
            (double)app->settings.display_brightness,
            (double)app->settings.speaker_volume);
    } else {
        FURI_LOG_E(TAG, "SAVE failed");
    }
    return ok;
}

static NotificationApp* notification_app_alloc(void) {
    NotificationApp* app = malloc(sizeof(NotificationApp));

    app->queue = furi_message_queue_alloc(8, sizeof(NotificationAppMessage));
    app->display_timer = furi_timer_alloc(notification_display_timer, FuriTimerTypeOnce, app);
    app->night_shift_timer = furi_timer_alloc(night_shift_timer_callback, FuriTimerTypePeriodic, app);
    app->night_shift_demo_timer = furi_timer_alloc(night_shift_demo_timer_callback, FuriTimerTypeOnce, app);

    app->settings.display_brightness = 1.0f;
    app->settings.display_off_delay_ms = 30000;
    app->settings.speaker_volume = 1.0f;
    app->settings.vibro_on = true;
    app->settings.night_shift = 1.0f;
    app->settings.night_shift_start = 1020;
    app->settings.night_shift_end = 300;
    app->current_night_shift = 1.0f;

    /* Try to load persisted settings (NVS-backed via saved_struct).
     * Fails silently on first boot or version mismatch — defaults stay in place. */
    notification_load_settings(app);

    app->display.value[LayerInternal] = 0x00;
    app->display.value[LayerNotification] = 0x00;
    app->display.index = LayerInternal;
    app->display_led_lock = false;

    app->event_record = furi_record_open(RECORD_INPUT_EVENTS);
    furi_pubsub_subscribe(app->event_record, input_event_callback, app);
    notification_message(app, &sequence_display_backlight_on);

    if(app->settings.night_shift != 1.0f) {
        night_shift_timer_start(app);
    } else {
        night_shift_timer_stop(app);
    }

    return app;
}

int32_t notification_srv(void* p) {
    UNUSED(p);

    NotificationApp* app = notification_app_alloc();

    notification_apply_internal_display_layer(app, 0x00);

    furi_record_create(RECORD_NOTIFICATION, app);

    NotificationAppMessage message;
    while(true) {
        furi_check(furi_message_queue_get(app->queue, &message, FuriWaitForever) == FuriStatusOk);

        switch(message.type) {
        case NotificationLayerMessage:
            notification_process_notification_message(app, &message);
            break;
        case InternalLayerMessage:
            notification_process_internal_message(app, &message);
            break;
        case SaveSettingsMessage:
            notification_save_settings(app);
            break;
        }

        if(message.back_event != NULL) {
            furi_event_flag_set(message.back_event, NOTIFICATION_EVENT_COMPLETE);
        }
    }

    return 0;
}

void notification_message(NotificationApp* app, const NotificationSequence* sequence) {
    notification_message_send(app, NotificationLayerMessage, sequence, NULL);
}

void notification_message_block(NotificationApp* app, const NotificationSequence* sequence) {
    FuriEventFlag* back_event = furi_event_flag_alloc();

    notification_message_send(app, NotificationLayerMessage, sequence, back_event);
    furi_event_flag_wait(
        back_event, NOTIFICATION_EVENT_COMPLETE, FuriFlagWaitAny, FuriWaitForever);
    furi_event_flag_free(back_event);
}

void notification_internal_message(NotificationApp* app, const NotificationSequence* sequence) {
    notification_message_send(app, InternalLayerMessage, sequence, NULL);
}

void notification_internal_message_block(
    NotificationApp* app,
    const NotificationSequence* sequence) {
    FuriEventFlag* back_event = furi_event_flag_alloc();

    notification_message_send(app, InternalLayerMessage, sequence, back_event);
    furi_event_flag_wait(
        back_event, NOTIFICATION_EVENT_COMPLETE, FuriFlagWaitAny, FuriWaitForever);
    furi_event_flag_free(back_event);
}

void notification_message_save_settings(NotificationApp* app) {
    furi_assert(app);
    notification_message_send(app, SaveSettingsMessage, &sequence_empty, NULL);
}
