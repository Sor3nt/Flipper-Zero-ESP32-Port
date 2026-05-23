#pragma once

#include <stdint.h>
#include <stdbool.h>

#define MOMENTUM_SETTINGS_PATH INT_PATH(".momentum_settings.txt")
#define MOMENTUM_SETTINGS_VERSION 1

typedef enum {
    MenuStyleList,
    MenuStyleWii,
    MenuStyleDsi,
    MenuStylePS4,
    MenuStyleVertical,
    MenuStyleC64,
    MenuStyleCompact,
    MenuStyleMntm,
    MenuStyleCoverFlow,

    MenuStyleCount,
} MenuStyle;

typedef enum {
    BatteryIconOff,
    BatteryIconBar,
    BatteryIconPercent,
    BatteryIconInvertedPercent,
    BatteryIconRetro3,
    BatteryIconRetro5,
    BatteryIconBarPercent,

    BatteryIconCount,
} BatteryIcon;

typedef enum {
    BrowserPathOff,
    BrowserPathCurrent,
    BrowserPathBrief,
    BrowserPathFull,

    BrowserPathCount,
} BrowserPathMode;

typedef struct {
    char asset_pack[32];
    uint32_t anim_speed;
    int32_t cycle_anims;
    bool unlock_anims;

    MenuStyle menu_style;

    bool lock_on_boot;
    bool bad_pins_format;
    bool allow_locked_rpc_usb;
    bool allow_locked_rpc_ble;
    bool lockscreen_poweroff;
    bool lockscreen_time;
    bool lockscreen_seconds;
    bool lockscreen_date;
    bool lockscreen_statusbar;
    bool lockscreen_prompt;
    bool lockscreen_transparent;

    BatteryIcon battery_icon;
    bool status_icons;
    bool bar_borders;
    bool bar_background;

    bool sort_dirs_first;
    bool show_hidden_files;
    bool show_internal_tab;
    BrowserPathMode browser_path_mode;
    uint32_t favorite_timeout;

    bool scroll_marquee;
    bool dark_mode;
    bool midnight_format_00;
    bool popup_overlay;
    uint32_t butthurt_timer;

    bool file_naming_prefix_after;
    uint32_t spoof_color;
    char startup_app[64];
} MomentumSettings;

extern MomentumSettings momentum_settings;

void momentum_settings_load(void);
void momentum_settings_save(void);
