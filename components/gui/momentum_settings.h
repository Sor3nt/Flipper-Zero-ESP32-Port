/**
 * @file momentum_settings.h
 * Settings shared by the interface, GUI, desktop and animation services.
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

#define MOMENTUM_CYCLE_ANIMS_OFF  (-1)
#define MOMENTUM_CYCLE_ANIMS_META (0)

typedef struct {
    char asset_pack[ASSET_PACKS_NAME_LEN];
    uint32_t anim_speed;
    int32_t cycle_anims;
    bool unlock_anims;

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

    uint32_t statusbar_hidden;
} MomentumSettings;

typedef enum {
    MomentumSettingsBoolLockOnBoot,
    MomentumSettingsBoolBadPinsFormat,
    MomentumSettingsBoolAllowLockedRpcUsb,
    MomentumSettingsBoolAllowLockedRpcBle,
    MomentumSettingsBoolLockscreenPoweroff,
    MomentumSettingsBoolLockscreenTime,
    MomentumSettingsBoolLockscreenSeconds,
    MomentumSettingsBoolLockscreenDate,
    MomentumSettingsBoolLockscreenStatusbar,
    MomentumSettingsBoolLockscreenPrompt,
    MomentumSettingsBoolLockscreenTransparent,
    MomentumSettingsBoolLockscreenSkipAnimation,
    MomentumSettingsBoolUnlockAnims,
    MomentumSettingsBoolCount,
} MomentumSettingsBool;

void momentum_settings_load(void);
void momentum_settings_reload(void);
void momentum_settings_save(void);
void momentum_settings_get_snapshot(MomentumSettings* out);
/** Allow configured status bar icons to render after startup settings are resolved. */
void momentum_settings_mark_statusbar_ready(void);
bool momentum_settings_get_bool(MomentumSettingsBool setting);
void momentum_settings_set_bool(MomentumSettingsBool setting, bool value);
uint32_t momentum_settings_get_anim_speed(void);
void momentum_settings_set_anim_speed(uint32_t speed);
int32_t momentum_settings_get_cycle_anims(void);
void momentum_settings_set_cycle_anims(int32_t cycle);
void momentum_settings_get_asset_pack(char* out, size_t out_size);
void momentum_settings_set_asset_pack(const char* name);
bool momentum_settings_statusbar_icon_visible(ViewPortStatusBarIcon icon);
void momentum_settings_set_statusbar_icon_visible(ViewPortStatusBarIcon icon, bool visible);

#ifdef __cplusplus
}
#endif
