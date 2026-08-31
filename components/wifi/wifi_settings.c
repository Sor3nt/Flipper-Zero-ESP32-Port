#include "wifi_settings.h"
#include "wifi_settings_filename.h"

#include <furi.h>
#include <storage/storage.h>
#include <toolbox/saved_struct.h>
#include <string.h>

#define TAG "WifiSettings"

#define WIFI_SETTINGS_PATH    INT_PATH(WIFI_SETTINGS_FILE_NAME)
#define WIFI_SETTINGS_VERSION (0)
#define WIFI_SETTINGS_MAGIC   (0x1A)

void wifi_settings_load(WifiSettings* settings) {
    furi_assert(settings);

    const bool load_success = saved_struct_load(
        WIFI_SETTINGS_PATH,
        settings,
        sizeof(WifiSettings),
        WIFI_SETTINGS_MAGIC,
        WIFI_SETTINGS_VERSION);

    if(!load_success) {
        FURI_LOG_W(TAG, "Failed to load settings, using defaults");
        memset(settings, 0, sizeof(WifiSettings));
        settings->enabled = false;
        wifi_settings_save(settings);
    }
}

void wifi_settings_save(const WifiSettings* settings) {
    furi_assert(settings);

    const bool success = saved_struct_save(
        WIFI_SETTINGS_PATH,
        settings,
        sizeof(WifiSettings),
        WIFI_SETTINGS_MAGIC,
        WIFI_SETTINGS_VERSION);

    if(!success) {
        FURI_LOG_E(TAG, "Failed to save settings");
    }
}
