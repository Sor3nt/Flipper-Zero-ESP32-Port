/**
 * @file momentum_settings.h
 * Interface settings ported from Momentum-Firmware (Graphics, Lockscreen,
 * Status Bar icons). Edited in Settings > Interface (loader_menu.c), read by
 * gui, desktop (lockscreen + animations), rpc and the status bar services.
 *
 * Menu style, lock-menu style, battery icon, clock and bar background keep
 * their existing storage (menu.c / DesktopSettings).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "view_port.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MOMENTUM_SETTINGS_PATH "/int/.momentum.settings"

#define ASSET_PACKS_PATH     "/ext/asset_packs"
#define ASSET_PACKS_NAME_LEN 32

/* Cycle Anims special values (anything > 0 is an interval in seconds) */
#define MOMENTUM_CYCLE_ANIMS_OFF  (-1) /**< never switch the idle animation */
#define MOMENTUM_CYCLE_ANIMS_META (0) /**< use the duration from meta.txt */

typedef struct {
    /* Graphics */
    char asset_pack[ASSET_PACKS_NAME_LEN]; /**< "" = Default (/ext/dolphin) */
    uint32_t anim_speed; /**< 25..300 % */
    int32_t cycle_anims; /**< MOMENTUM_CYCLE_ANIMS_* or seconds */
    bool unlock_anims; /**< ignore level / butthurt limits */

    /* Lockscreen */
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
    bool lockscreen_skip_animation;

    /* Status bar: bit (1 << ViewPortStatusBarIcon) set = icon hidden */
    uint32_t statusbar_hidden;
} MomentumSettings;

extern MomentumSettings momentum_settings;

/** Load settings from SD. No-op once a load succeeded; keeps the defaults
 * (and retries on the next call) while the SD card is not ready. */
void momentum_settings_load(void);

/** Persist the current settings. */
void momentum_settings_save(void);

/** Copy the selected asset pack name ("" = Default) into out. */
void momentum_settings_get_asset_pack(char* out, size_t out_size);

/** Select an asset pack ("" or NULL = Default). Does not save. */
void momentum_settings_set_asset_pack(const char* name);

bool momentum_settings_statusbar_icon_visible(ViewPortStatusBarIcon icon);
void momentum_settings_set_statusbar_icon_visible(ViewPortStatusBarIcon icon, bool visible);

#ifdef __cplusplus
}
#endif
