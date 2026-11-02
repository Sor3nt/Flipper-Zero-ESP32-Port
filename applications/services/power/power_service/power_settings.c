#include "power_settings.h"
#include "power_settings_filename.h"

#include <saved_struct.h>
#include <storage/storage.h>

#define TAG "PowerSettings"

#define POWER_SETTINGS_VER_1 (1) // Oldest version number
#define POWER_SETTINGS_VER_2 (2) // Previous version number
#define POWER_SETTINGS_VER_3 (3) // Previous version number
#define POWER_SETTINGS_VER_4 (4) // Previous version number
#define POWER_SETTINGS_VER   (5) // New version number

#define POWER_SETTINGS_PATH     INT_PATH(POWER_SETTINGS_FILE_NAME)
#define POWER_SETTINGS_MAGIC_V1 (0x19)
#define POWER_SETTINGS_MAGIC_V2 (0x21)
#define POWER_SETTINGS_MAGIC_V3 (0x22)
#define POWER_SETTINGS_MAGIC_V4 (0x23)
#define POWER_SETTINGS_MAGIC    (0x24)
#define POWER_SETTINGS_DEFAULT_LIGHT_SLEEP_DELAY_MS (30000U)

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

typedef struct {
    uint32_t auto_poweroff_delay_ms;
    uint8_t charge_supress_percent;
    uint8_t off_mode;
    uint32_t light_sleep_delay_ms;
    bool light_sleep_enabled;
} PowerSettingsV4;

static void power_settings_set_new_defaults(PowerSettings* settings) {
    settings->deep_sleep_usb_check_enabled = false;
    settings->deep_sleep_usb_check_interval_minutes =
        POWER_SETTINGS_DEFAULT_DEEP_SLEEP_USB_INTERVAL_MINUTES;
    settings->light_sleep_ship_mode_enabled = false;
    settings->light_sleep_ship_mode_interval_minutes =
        POWER_SETTINGS_DEFAULT_LIGHT_SLEEP_SHIP_INTERVAL_MINUTES;
}

static bool power_settings_normalize(PowerSettings* settings) {
    bool changed = false;
    if(settings->deep_sleep_usb_check_interval_minutes < POWER_SETTINGS_INTERVAL_MIN_MINUTES ||
       settings->deep_sleep_usb_check_interval_minutes > POWER_SETTINGS_INTERVAL_MAX_MINUTES) {
        settings->deep_sleep_usb_check_interval_minutes =
            POWER_SETTINGS_DEFAULT_DEEP_SLEEP_USB_INTERVAL_MINUTES;
        changed = true;
    }
    if(settings->light_sleep_ship_mode_interval_minutes < POWER_SETTINGS_INTERVAL_MIN_MINUTES ||
       settings->light_sleep_ship_mode_interval_minutes > POWER_SETTINGS_INTERVAL_MAX_MINUTES) {
        settings->light_sleep_ship_mode_interval_minutes =
            POWER_SETTINGS_DEFAULT_LIGHT_SLEEP_SHIP_INTERVAL_MINUTES;
        changed = true;
    }
    if(!settings->light_sleep_enabled && settings->light_sleep_ship_mode_enabled) {
        settings->light_sleep_ship_mode_enabled = false;
        changed = true;
    }
    return changed;
}

void power_settings_load(PowerSettings* settings) {
    furi_assert(settings);

    bool success = false;
    bool should_save = false;

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

        } else if(version == POWER_SETTINGS_VER_4) {
            PowerSettingsV4 previous = {0};

            success = saved_struct_load(
                POWER_SETTINGS_PATH,
                &previous,
                sizeof(PowerSettingsV4),
                POWER_SETTINGS_MAGIC_V4,
                POWER_SETTINGS_VER_4);
            if(success) {
                settings->auto_poweroff_delay_ms = previous.auto_poweroff_delay_ms;
                settings->charge_supress_percent = previous.charge_supress_percent;
                settings->off_mode = previous.off_mode;
                settings->light_sleep_delay_ms = previous.light_sleep_delay_ms;
                settings->light_sleep_enabled = previous.light_sleep_enabled;
                power_settings_set_new_defaults(settings);
                should_save = true;
            }

        } else if(version == POWER_SETTINGS_VER_3) {
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
                settings->light_sleep_delay_ms = POWER_SETTINGS_DEFAULT_LIGHT_SLEEP_DELAY_MS;
                settings->light_sleep_enabled = false;
                power_settings_set_new_defaults(settings);
                should_save = true;
            }

            // if config previous version - load it and manual set new settings to initial value
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
                settings->light_sleep_delay_ms = POWER_SETTINGS_DEFAULT_LIGHT_SLEEP_DELAY_MS;
                settings->light_sleep_enabled = false;
                power_settings_set_new_defaults(settings);
                should_save = true;
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
                settings->light_sleep_delay_ms = POWER_SETTINGS_DEFAULT_LIGHT_SLEEP_DELAY_MS;
                settings->light_sleep_enabled = false;
                power_settings_set_new_defaults(settings);
                should_save = true;
            }
        }

    } while(false);

    if(!success) {
        FURI_LOG_W(TAG, "Failed to load file, using defaults");
        memset(settings, 0, sizeof(PowerSettings));
        settings->light_sleep_delay_ms = POWER_SETTINGS_DEFAULT_LIGHT_SLEEP_DELAY_MS;
        power_settings_set_new_defaults(settings);
        should_save = true;
        success = true;
    }

    should_save = power_settings_normalize(settings) || should_save;
    if(should_save) power_settings_save(settings);
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
