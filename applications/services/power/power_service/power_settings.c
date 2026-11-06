#include "power_settings.h"
#include "power_settings_filename.h"

#include <saved_struct.h>
#include <storage/storage.h>

#define TAG "PowerSettings"

#define POWER_SETTINGS_VER_1 (1) // Oldest version number
#define POWER_SETTINGS_VER_2 (2)
#define POWER_SETTINGS_VER_3 (3) // Previous version number
#define POWER_SETTINGS_VER   (4) // New version number

#define POWER_SETTINGS_PATH     INT_PATH(POWER_SETTINGS_FILE_NAME)
#define POWER_SETTINGS_MAGIC_V1 (0x19)
#define POWER_SETTINGS_MAGIC_V2 (0x21)
#define POWER_SETTINGS_MAGIC_V3 (0x22)
#define POWER_SETTINGS_MAGIC    (0x23)

typedef struct {
    uint32_t auto_poweroff_delay_ms;
} PowerSettingsV1;

typedef struct {
    uint32_t auto_poweroff_delay_ms;
    uint8_t charge_supress_percent;
} PowerSettingsV2;

typedef struct {
    uint32_t auto_poweroff_delay_ms;
    uint8_t charge_supress_percent;
    uint8_t off_mode;
} PowerSettingsV3;

void power_settings_load(PowerSettings* settings) {
    furi_assert(settings);

    bool success = false;

    do {
        uint8_t version;
        if(!saved_struct_get_metadata(POWER_SETTINGS_PATH, NULL, &version, NULL)) break;

        // if config actual version - load it directly
        if(version == POWER_SETTINGS_VER) {
            success = saved_struct_load(
                POWER_SETTINGS_PATH,
                settings,
                sizeof(PowerSettings),
                POWER_SETTINGS_MAGIC,
                POWER_SETTINGS_VER);

            // if config previous version - load it and manual set new settings to initial value
        } else if(version == POWER_SETTINGS_VER_3) {
            /* Same size as V4 (V3 had two padding bytes), so the new fields
             * are set explicitly instead of trusting the padding. */
            PowerSettingsV3 previous = {0};

            success = saved_struct_load(
                POWER_SETTINGS_PATH,
                &previous,
                sizeof(PowerSettingsV3),
                POWER_SETTINGS_MAGIC_V3,
                POWER_SETTINGS_VER_3);
            if(success) {
                settings->auto_poweroff_delay_ms = previous.auto_poweroff_delay_ms;
                settings->charge_supress_percent = previous.charge_supress_percent;
                settings->off_mode = previous.off_mode;
                settings->auto_light_sleep = 0;
                settings->led_usb_auto_off = 0;
            }

        } else if(version == POWER_SETTINGS_VER_2) {
            PowerSettingsV2 previous = {0};

            success = saved_struct_load(
                POWER_SETTINGS_PATH,
                &previous,
                sizeof(PowerSettingsV2),
                POWER_SETTINGS_MAGIC_V2,
                POWER_SETTINGS_VER_2);
            // new settings initialization
            if(success) {
                settings->auto_poweroff_delay_ms = previous.auto_poweroff_delay_ms;
                settings->charge_supress_percent = previous.charge_supress_percent;
                settings->off_mode = PowerOffModeDeepSleep;
                settings->auto_light_sleep = 0;
                settings->led_usb_auto_off = 0;
            }

        } else if(version == POWER_SETTINGS_VER_1) {
            PowerSettingsV1 previous = {0};

            success = saved_struct_load(
                POWER_SETTINGS_PATH,
                &previous,
                sizeof(PowerSettingsV1),
                POWER_SETTINGS_MAGIC_V1,
                POWER_SETTINGS_VER_1);
            // new settings initialization
            if(success) {
                settings->auto_poweroff_delay_ms = previous.auto_poweroff_delay_ms;
                settings->charge_supress_percent = 0;
                settings->off_mode = PowerOffModeDeepSleep;
                settings->auto_light_sleep = 0;
                settings->led_usb_auto_off = 0;
            }
        }

    } while(false);

    if(!success) {
        FURI_LOG_W(TAG, "Failed to load file, using defaults");
        memset(settings, 0, sizeof(PowerSettings));
        power_settings_save(settings);
    }
}

void power_settings_save(const PowerSettings* settings) {
    furi_assert(settings);

    const bool success = saved_struct_save(
        POWER_SETTINGS_PATH,
        settings,
        sizeof(PowerSettings),
        POWER_SETTINGS_MAGIC,
        POWER_SETTINGS_VER);

    if(!success) {
        FURI_LOG_E(TAG, "Failed to save file");
    }
}
