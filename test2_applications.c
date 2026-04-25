#include "applications.h"
#include <assets_icons.h>

const char* FLIPPER_AUTORUN_APP_NAME = "";

extern int32_t example_apps_assets_main(void* p);
extern int32_t example_apps_data_main(void* p);
extern int32_t example_number_input(void* p);
extern int32_t subghz_app(void* p);
extern int32_t about_app(void* p);
extern int32_t archive_app(void* p);
extern int32_t cli_vcp_srv(void* p);
extern int32_t input_srv(void* p);
extern int32_t js_app(void* p);
extern int32_t power_settings_app(void* p);
extern int32_t bt_settings_app(void* p);
extern int32_t lfrfid_app(void* p);
extern int32_t notification_srv(void* p);
extern int32_t storage_settings_app(void* p);
extern int32_t gui_srv(void* p);
extern int32_t nfc_app(void* p);
extern int32_t dialogs_srv(void* p);
extern int32_t infrared_app(void* p);
extern int32_t bt_srv(void* p);
extern int32_t dolphin_srv(void* p);
extern int32_t loader_srv(void* p);
extern int32_t wifi_app(void* p);
extern int32_t ble_spam_app(void* p);
extern int32_t bad_usb_app(void* p);
extern int32_t notification_settings_app(void* p);
extern int32_t passport_app(void* p);
extern int32_t clock_app(void* p);
extern int32_t power_srv(void* p);
extern int32_t storage_srv(void* p);
extern int32_t desktop_srv(void* p);
extern void cli_on_system_start(void);
extern void storage_on_system_start(void);
extern void loader_on_system_start(void);
extern void power_on_system_start(void);
extern void locale_on_system_start(void);
extern void js_app_on_system_start(void);
extern void subghz_dangerous_freq(void);

const FlipperInternalApplication FLIPPER_SERVICES[] = {
    {.app = input_srv, .name = "InputSrv", .appid = "input", .stack_size = 4096, .icon = NULL, .flags = FlipperInternalApplicationFlagDefault},
    {.app = cli_vcp_srv, .name = "CliVcpSrv", .appid = "cli_vcp", .stack_size = 4096, .icon = NULL, .flags = FlipperInternalApplicationFlagDefault},
    {.app = notification_srv, .name = "NotificationSrv", .appid = "notification", .stack_size = 4096, .icon = NULL, .flags = FlipperInternalApplicationFlagDefault},
    {.app = gui_srv, .name = "GuiSrv", .appid = "gui", .stack_size = 8192, .icon = NULL, .flags = FlipperInternalApplicationFlagDefault},
    {.app = dialogs_srv, .name = "DialogsSrv", .appid = "dialogs", .stack_size = 4096, .icon = NULL, .flags = FlipperInternalApplicationFlagDefault},
    {.app = bt_srv, .name = "BtSrv", .appid = "bt", .stack_size = 4096, .icon = NULL, .flags = FlipperInternalApplicationFlagDefault},
    {.app = dolphin_srv, .name = "DolphinSrv", .appid = "dolphin", .stack_size = 4096, .icon = NULL, .flags = FlipperInternalApplicationFlagDefault},
    {.app = loader_srv, .name = "LoaderSrv", .appid = "loader", .stack_size = 4096, .icon = NULL, .flags = FlipperInternalApplicationFlagDefault},
    {.app = power_srv, .name = "PowerSrv", .appid = "power", .stack_size = 4096, .icon = NULL, .flags = FlipperInternalApplicationFlagDefault},
    {.app = storage_srv, .name = "StorageSrv", .appid = "storage", .stack_size = 4096, .icon = NULL, .flags = FlipperInternalApplicationFlagDefault},
    {.app = desktop_srv, .name = "DesktopSrv", .appid = "desktop", .stack_size = 4096, .icon = NULL, .flags = FlipperInternalApplicationFlagDefault},
};
const size_t FLIPPER_SERVICES_COUNT = COUNT_OF(FLIPPER_SERVICES);

const FlipperInternalApplication FLIPPER_APPS[] = {
    {.app = subghz_app, .name = "Sub-GHz", .appid = "subghz", .stack_size = 8192, .icon = &A_Sub1ghz_14, .flags = FlipperInternalApplicationFlagDefault},
};
const size_t FLIPPER_APPS_COUNT = COUNT_OF(FLIPPER_APPS);

const FlipperInternalOnStartHook FLIPPER_ON_SYSTEM_START[] = {
    cli_on_system_start,
    storage_on_system_start,
    loader_on_system_start,
    power_on_system_start,
    locale_on_system_start,
    js_app_on_system_start,
    subghz_dangerous_freq,
};
const size_t FLIPPER_ON_SYSTEM_START_COUNT = COUNT_OF(FLIPPER_ON_SYSTEM_START);

const FlipperInternalApplication FLIPPER_SYSTEM_APPS[] = {
    {.app = example_apps_assets_main, .name = "Example: Apps Assets", .appid = "example_apps_assets", .stack_size = 4096, .icon = NULL, .flags = FlipperInternalApplicationFlagDefault},
    {.app = example_apps_data_main, .name = "Example: Apps Data", .appid = "example_apps_data", .stack_size = 4096, .icon = NULL, .flags = FlipperInternalApplicationFlagDefault},
    {.app = example_number_input, .name = "Example: Number Input", .appid = "example_number_input", .stack_size = 4096, .icon = NULL, .flags = FlipperInternalApplicationFlagDefault},
    {.app = about_app, .name = "About", .appid = "about", .stack_size = 4096, .icon = &A_Settings_14, .flags = FlipperInternalApplicationFlagDefault},
    {.app = js_app, .name = "JS Runner", .appid = "js_app", .stack_size = 4096, .icon = NULL, .flags = FlipperInternalApplicationFlagDefault},
    {.app = bt_settings_app, .name = "Bluetooth", .appid = "bt_settings", .stack_size = 4096, .icon = &A_Settings_14, .flags = FlipperInternalApplicationFlagDefault},
    {.app = lfrfid_app, .name = "125 kHz RFID", .appid = "lfrfid", .stack_size = 4096, .icon = &A_125khz_14, .flags = FlipperInternalApplicationFlagDefault},
    {.app = nfc_app, .name = "NFC", .appid = "nfc", .stack_size = 5120, .icon = &A_NFC_14, .flags = FlipperInternalApplicationFlagDefault},
    {.app = infrared_app, .name = "Infrared", .appid = "infrared", .stack_size = 8192, .icon = &A_Infrared_14, .flags = FlipperInternalApplicationFlagDefault},
    {.app = wifi_app, .name = "WiFi", .appid = "wifi", .stack_size = 8192, .icon = &A_Sub1ghz_14, .flags = FlipperInternalApplicationFlagDefault},
    {.app = ble_spam_app, .name = "Bluetooth", .appid = "ble_spam", .stack_size = 8192, .icon = &A_Plugins_14, .flags = FlipperInternalApplicationFlagDefault},
    {.app = bad_usb_app, .name = "Bad USB", .appid = "bad_usb", .stack_size = 4096, .icon = &A_BadUsb_14, .flags = FlipperInternalApplicationFlagDefault},
    {.app = passport_app, .name = "Passport", .appid = "passport", .stack_size = 4096, .icon = NULL, .flags = FlipperInternalApplicationFlagDefault},
    {.app = clock_app, .name = "Clock", .appid = "clock", .stack_size = 4096, .icon = &A_Clock_14, .flags = FlipperInternalApplicationFlagDefault},
};
const size_t FLIPPER_SYSTEM_APPS_COUNT = COUNT_OF(FLIPPER_SYSTEM_APPS);

const FlipperInternalApplication FLIPPER_DEBUG_APPS[] = {
};
const size_t FLIPPER_DEBUG_APPS_COUNT = COUNT_OF(FLIPPER_DEBUG_APPS);

const FlipperInternalApplication FLIPPER_SETTINGS_APPS[] = {
    {.app = power_settings_app, .name = "Power", .appid = "power_settings", .stack_size = 4096, .icon = NULL, .flags = FlipperInternalApplicationFlagInsomniaSafe},
    {.app = storage_settings_app, .name = "Storage", .appid = "storage_settings", .stack_size = 4096, .icon = NULL, .flags = FlipperInternalApplicationFlagDefault},
    {.app = notification_settings_app, .name = "Sound & Display", .appid = "notification_settings", .stack_size = 4096, .icon = NULL, .flags = FlipperInternalApplicationFlagDefault},
};
const size_t FLIPPER_SETTINGS_APPS_COUNT = COUNT_OF(FLIPPER_SETTINGS_APPS);

const FlipperInternalApplication FLIPPER_ARCHIVE =     {.app = archive_app, .name = "Archive", .appid = "archive", .stack_size = 8192, .icon = &A_FileManager_14, .flags = FlipperInternalApplicationFlagDefault};

const FlipperExternalApplication FLIPPER_EXTSETTINGS_APPS[] = {
    {.name = "About", .icon = &A_Settings_14, .path = "about"},
    {.name = "Bluetooth", .icon = &A_Settings_14, .path = "bt_settings"},
    {.name = "Passport", .icon = NULL, .path = "passport"},
};
const size_t FLIPPER_EXTSETTINGS_APPS_COUNT = COUNT_OF(FLIPPER_EXTSETTINGS_APPS);

const FlipperExternalApplication FLIPPER_EXTERNAL_APPS[] = {
    {.name = "125 kHz RFID", .icon = &A_125khz_14, .path = "lfrfid"},
    {.name = "NFC", .icon = &A_NFC_14, .path = "nfc"},
    {.name = "Infrared", .icon = &A_Infrared_14, .path = "infrared"},
    {.name = "WiFi", .icon = &A_Sub1ghz_14, .path = "wifi"},
    {.name = "Bluetooth", .icon = &A_Plugins_14, .path = "ble_spam"},
    {.name = "Bad USB", .icon = &A_BadUsb_14, .path = "bad_usb"},
    {.name = "Clock", .icon = &A_Clock_14, .path = "clock"},
};
const size_t FLIPPER_EXTERNAL_APPS_COUNT = COUNT_OF(FLIPPER_EXTERNAL_APPS);

const FlipperExternalApplication FLIPPER_INTERNAL_EXTERNAL_APPS[] = {
    {.name = "Example: Number Input", .icon = NULL, .path = "example_number_input"},
    {.name = "Example: Apps Data", .icon = NULL, .path = "example_apps_data"},
    {.name = "Example: Apps Assets", .icon = NULL, .path = "example_apps_assets"},
};
const size_t FLIPPER_INTERNAL_EXTERNAL_APPS_COUNT = COUNT_OF(FLIPPER_INTERNAL_EXTERNAL_APPS);
