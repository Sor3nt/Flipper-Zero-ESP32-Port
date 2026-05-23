#include "momentum_settings.h"

#include <furi.h>
#include <storage/storage.h>
#include <flipper_format/flipper_format.h>

#define TAG "MomentumSrv"
#define SETTINGS_KEY "MomentumSettings"

MomentumSettings momentum_settings = {
    .asset_pack = "",
    .anim_speed = 100,
    .cycle_anims = 0,
    .unlock_anims = false,
    .menu_style = MenuStyleDsi,
    .lock_on_boot = true,
    .bad_pins_format = false,
    .allow_locked_rpc_usb = false,
    .allow_locked_rpc_ble = false,
    .lockscreen_poweroff = true,
    .lockscreen_time = true,
    .lockscreen_seconds = false,
    .lockscreen_date = true,
    .lockscreen_statusbar = true,
    .lockscreen_prompt = true,
    .lockscreen_transparent = false,
    .battery_icon = BatteryIconBarPercent,
    .status_icons = true,
    .bar_borders = true,
    .bar_background = false,
    .sort_dirs_first = true,
    .show_hidden_files = false,
    .show_internal_tab = false,
    .browser_path_mode = BrowserPathOff,
    .favorite_timeout = 0,
    .scroll_marquee = false,
    .dark_mode = false,
    .midnight_format_00 = true,
    .popup_overlay = true,
    .butthurt_timer = 21600,
    .file_naming_prefix_after = false,
    .spoof_color = 0,
    .startup_app = "",
};

void momentum_settings_load(void) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* ff = flipper_format_file_alloc(storage);

    FURI_LOG_I(TAG, "loading settings from %s", MOMENTUM_SETTINGS_PATH);

    do {
        if(!flipper_format_file_open_existing(ff, MOMENTUM_SETTINGS_PATH)) {
            FURI_LOG_W(TAG, "no settings file, using defaults");
            break;
        }

        uint32_t version = 0;
        if(!flipper_format_read_uint32(ff, "Version", &version, 1)) {
            FURI_LOG_W(TAG, "no version, using defaults");
            break;
        }
        if(version != MOMENTUM_SETTINGS_VERSION) {
            FURI_LOG_W(TAG, "version mismatch, using defaults");
            break;
        }

        flipper_format_read_string(ff, "AssetPack", (FuriString*)&momentum_settings.asset_pack);
        flipper_format_read_uint32(ff, "AnimSpeed", &momentum_settings.anim_speed, 1);
        flipper_format_read_int32(ff, "CycleAnims", &momentum_settings.cycle_anims, 1);
        flipper_format_read_bool(ff, "UnlockAnims", &momentum_settings.unlock_anims, 1);

        uint32_t menu_style = momentum_settings.menu_style;
        flipper_format_read_uint32(ff, "MenuStyle", &menu_style, 1);
        if(menu_style < MenuStyleCount) momentum_settings.menu_style = menu_style;

        uint32_t battery_icon = momentum_settings.battery_icon;
        flipper_format_read_uint32(ff, "BatteryIcon", &battery_icon, 1);
        if(battery_icon < BatteryIconCount) momentum_settings.battery_icon = battery_icon;

        flipper_format_read_bool(ff, "StatusIcons", &momentum_settings.status_icons, 1);
        flipper_format_read_bool(ff, "BarBorders", &momentum_settings.bar_borders, 1);

        flipper_format_read_bool(ff, "SortDirsFirst", &momentum_settings.sort_dirs_first, 1);
        flipper_format_read_bool(
            ff, "ShowHiddenFiles", &momentum_settings.show_hidden_files, 1);

        uint32_t browser_path = momentum_settings.browser_path_mode;
        flipper_format_read_uint32(ff, "BrowserPathMode", &browser_path, 1);
        if(browser_path < BrowserPathCount)
            momentum_settings.browser_path_mode = browser_path;

        flipper_format_read_bool(
            ff, "MidnightFormat00", &momentum_settings.midnight_format_00, 1);

        FuriString* startup = furi_string_alloc();
        if(flipper_format_read_string(ff, "StartupApp", startup)) {
            strncpy(
                momentum_settings.startup_app,
                furi_string_get_cstr(startup),
                sizeof(momentum_settings.startup_app) - 1);
        }
        furi_string_free(startup);
    } while(false);

    flipper_format_free(ff);
    furi_record_close(RECORD_STORAGE);
}

void momentum_settings_save(void) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* ff = flipper_format_file_alloc(storage);

    FURI_LOG_I(TAG, "saving settings to %s", MOMENTUM_SETTINGS_PATH);

    do {
        if(!flipper_format_file_open_always(ff, MOMENTUM_SETTINGS_PATH)) {
            FURI_LOG_E(TAG, "failed to open settings file");
            break;
        }

        uint32_t version = MOMENTUM_SETTINGS_VERSION;
        flipper_format_write_header_cstr(ff, "MomentumSettings", version);
        flipper_format_write_string_cstr(ff, "AssetPack", momentum_settings.asset_pack);
        flipper_format_write_uint32(ff, "AnimSpeed", &momentum_settings.anim_speed, 1);
        flipper_format_write_int32(ff, "CycleAnims", &momentum_settings.cycle_anims, 1);
        flipper_format_write_bool(ff, "UnlockAnims", &momentum_settings.unlock_anims, 1);

        uint32_t menu_style = momentum_settings.menu_style;
        flipper_format_write_uint32(ff, "MenuStyle", &menu_style, 1);

        uint32_t battery_icon = momentum_settings.battery_icon;
        flipper_format_write_uint32(ff, "BatteryIcon", &battery_icon, 1);
        flipper_format_write_bool(ff, "StatusIcons", &momentum_settings.status_icons, 1);
        flipper_format_write_bool(ff, "BarBorders", &momentum_settings.bar_borders, 1);
        flipper_format_write_bool(
            ff, "SortDirsFirst", &momentum_settings.sort_dirs_first, 1);
        flipper_format_write_bool(
            ff, "ShowHiddenFiles", &momentum_settings.show_hidden_files, 1);
        uint32_t browser_path = momentum_settings.browser_path_mode;
        flipper_format_write_uint32(ff, "BrowserPathMode", &browser_path, 1);
        flipper_format_write_bool(
            ff, "MidnightFormat00", &momentum_settings.midnight_format_00, 1);
        flipper_format_write_string_cstr(
            ff, "StartupApp", momentum_settings.startup_app);
    } while(false);

    flipper_format_free(ff);
    furi_record_close(RECORD_STORAGE);
}
