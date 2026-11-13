#include "momentum_settings.h"

#include <furi.h>
#include <saved_struct.h>
#include <storage.h>

#define TAG "MomentumSettings"

#define MOMENTUM_SETTINGS_MAGIC (0x4D)
#define MOMENTUM_SETTINGS_VER   (1)

static MomentumSettings momentum_settings = {
    .asset_pack = "",
    .anim_speed = 100,
    .cycle_anims = MOMENTUM_CYCLE_ANIMS_META,
    .unlock_anims = false,
    .lock_on_boot = true,
    .bad_pins_format = false,
    .allow_locked_rpc_usb = false,
    .allow_locked_rpc_ble = true,
    .lockscreen_poweroff = true,
    .lockscreen_time = true,
    .lockscreen_seconds = false,
    .lockscreen_date = true,
    .lockscreen_statusbar = true,
    .lockscreen_prompt = true,
    .lockscreen_transparent = false,
    .lockscreen_skip_animation = false,
    .statusbar_hidden = 0,
};

static bool momentum_settings_loaded = false;
static bool momentum_settings_loading = false;
static bool momentum_settings_statusbar_ready = false;

static const size_t momentum_settings_bool_offsets[MomentumSettingsBoolCount] = {
    [MomentumSettingsBoolLockOnBoot] = offsetof(MomentumSettings, lock_on_boot),
    [MomentumSettingsBoolBadPinsFormat] = offsetof(MomentumSettings, bad_pins_format),
    [MomentumSettingsBoolAllowLockedRpcUsb] = offsetof(MomentumSettings, allow_locked_rpc_usb),
    [MomentumSettingsBoolAllowLockedRpcBle] = offsetof(MomentumSettings, allow_locked_rpc_ble),
    [MomentumSettingsBoolLockscreenPoweroff] = offsetof(MomentumSettings, lockscreen_poweroff),
    [MomentumSettingsBoolLockscreenTime] = offsetof(MomentumSettings, lockscreen_time),
    [MomentumSettingsBoolLockscreenSeconds] = offsetof(MomentumSettings, lockscreen_seconds),
    [MomentumSettingsBoolLockscreenDate] = offsetof(MomentumSettings, lockscreen_date),
    [MomentumSettingsBoolLockscreenStatusbar] = offsetof(MomentumSettings, lockscreen_statusbar),
    [MomentumSettingsBoolLockscreenPrompt] = offsetof(MomentumSettings, lockscreen_prompt),
    [MomentumSettingsBoolLockscreenTransparent] = offsetof(MomentumSettings, lockscreen_transparent),
    [MomentumSettingsBoolLockscreenSkipAnimation] =
        offsetof(MomentumSettings, lockscreen_skip_animation),
    [MomentumSettingsBoolUnlockAnims] = offsetof(MomentumSettings, unlock_anims),
};

static bool* momentum_settings_bool_ptr(MomentumSettingsBool setting) {
    return (bool*)((uint8_t*)&momentum_settings + momentum_settings_bool_offsets[setting]);
}

static void momentum_settings_sanitize(MomentumSettings* settings) {
    settings->asset_pack[ASSET_PACKS_NAME_LEN - 1] = '\0';
    if(settings->anim_speed < 25 || settings->anim_speed > 300) settings->anim_speed = 100;
    if(settings->cycle_anims < MOMENTUM_CYCLE_ANIMS_OFF || settings->cycle_anims > 86400) {
        settings->cycle_anims = MOMENTUM_CYCLE_ANIMS_META;
    }
    settings->statusbar_hidden &= (1UL << ViewPortStatusBarIconCount) - 1;
    settings->statusbar_hidden &= ~(1UL << ViewPortStatusBarIconNone);
}

void momentum_settings_load(void) {
    bool load_started;
    /* Own block: FURI_CRITICAL_ENTER declares __furi_critical_info, which would
     * clash with the second critical section in this function otherwise. */
    {
        FURI_CRITICAL_ENTER();
        load_started = momentum_settings_loaded || momentum_settings_loading;
        if(!load_started) momentum_settings_loading = true;
        FURI_CRITICAL_EXIT();
    }
    if(load_started) return;

    MomentumSettings* loaded = malloc(sizeof(MomentumSettings));
    furi_check(loaded);

    /* Wait for StorageSrv so saved_struct_load can prefer the SD copy over NVS. */
    Storage* storage = furi_record_open(RECORD_STORAGE);
    furi_record_close(RECORD_STORAGE);

    const bool settings_loaded = saved_struct_load(
           MOMENTUM_SETTINGS_PATH,
           loaded,
           sizeof(MomentumSettings),
           MOMENTUM_SETTINGS_MAGIC,
           MOMENTUM_SETTINGS_VER);

    bool storage_ready = false;
    if(settings_loaded) {
        momentum_settings_sanitize(loaded);
        FURI_LOG_I(TAG, "Loaded");
    } else {
        storage = furi_record_open(RECORD_STORAGE);
        storage_ready = storage_sd_status(storage) == FSE_OK;
        furi_record_close(RECORD_STORAGE);
        FURI_LOG_I(TAG, "Using defaults");
    }

    {
        FURI_CRITICAL_ENTER();
        if(!momentum_settings_loaded) {
            if(settings_loaded) momentum_settings = *loaded;
            momentum_settings_loaded = settings_loaded || storage_ready;
        }
        momentum_settings_loading = false;
        FURI_CRITICAL_EXIT();
    }
    free(loaded);
}

void momentum_settings_reload(void) {
    bool load_in_progress;
    FURI_CRITICAL_ENTER();
    load_in_progress = momentum_settings_loading;
    if(!load_in_progress) momentum_settings_loaded = false;
    FURI_CRITICAL_EXIT();

    if(!load_in_progress) momentum_settings_load();
}

void momentum_settings_save(void) {
    MomentumSettings* snapshot = malloc(sizeof(MomentumSettings));
    furi_check(snapshot);
    FURI_CRITICAL_ENTER();
    *snapshot = momentum_settings;
    FURI_CRITICAL_EXIT();

    if(!saved_struct_save(
           MOMENTUM_SETTINGS_PATH,
           snapshot,
           sizeof(MomentumSettings),
           MOMENTUM_SETTINGS_MAGIC,
           MOMENTUM_SETTINGS_VER)) {
        FURI_LOG_E(TAG, "Failed to save");
    } else {
        FURI_CRITICAL_ENTER();
        momentum_settings_loaded = true;
        FURI_CRITICAL_EXIT();
    }
    free(snapshot);
}

void momentum_settings_get_snapshot(MomentumSettings* out) {
    furi_check(out);
    FURI_CRITICAL_ENTER();
    *out = momentum_settings;
    FURI_CRITICAL_EXIT();
}

void momentum_settings_mark_statusbar_ready(void) {
    FURI_CRITICAL_ENTER();
    momentum_settings_statusbar_ready = true;
    FURI_CRITICAL_EXIT();
}

bool momentum_settings_get_bool(MomentumSettingsBool setting) {
    furi_check((int)setting >= 0 && setting < MomentumSettingsBoolCount);
    FURI_CRITICAL_ENTER();
    const bool value = *momentum_settings_bool_ptr(setting);
    FURI_CRITICAL_EXIT();
    return value;
}

void momentum_settings_set_bool(MomentumSettingsBool setting, bool value) {
    furi_check((int)setting >= 0 && setting < MomentumSettingsBoolCount);
    FURI_CRITICAL_ENTER();
    *momentum_settings_bool_ptr(setting) = value;
    FURI_CRITICAL_EXIT();
}

uint32_t momentum_settings_get_anim_speed(void) {
    FURI_CRITICAL_ENTER();
    const uint32_t speed = momentum_settings.anim_speed;
    FURI_CRITICAL_EXIT();
    return speed;
}

void momentum_settings_set_anim_speed(uint32_t speed) {
    FURI_CRITICAL_ENTER();
    momentum_settings.anim_speed = speed;
    FURI_CRITICAL_EXIT();
}

int32_t momentum_settings_get_cycle_anims(void) {
    FURI_CRITICAL_ENTER();
    const int32_t cycle = momentum_settings.cycle_anims;
    FURI_CRITICAL_EXIT();
    return cycle;
}

void momentum_settings_set_cycle_anims(int32_t cycle) {
    FURI_CRITICAL_ENTER();
    momentum_settings.cycle_anims = cycle;
    FURI_CRITICAL_EXIT();
}

void momentum_settings_get_asset_pack(char* out, size_t out_size) {
    furi_check(out);
    furi_check(out_size);
    FURI_CRITICAL_ENTER();
    strlcpy(out, momentum_settings.asset_pack, out_size);
    FURI_CRITICAL_EXIT();
}

void momentum_settings_set_asset_pack(const char* name) {
    char copy[ASSET_PACKS_NAME_LEN] = {0};
    strlcpy(copy, name ? name : "", sizeof(copy));
    FURI_CRITICAL_ENTER();
    memcpy(momentum_settings.asset_pack, copy, sizeof(copy));
    FURI_CRITICAL_EXIT();
}

bool momentum_settings_statusbar_icon_visible(ViewPortStatusBarIcon icon) {
    if(icon == ViewPortStatusBarIconNone || icon >= ViewPortStatusBarIconCount) return true;
    FURI_CRITICAL_ENTER();
    const bool visible = momentum_settings_statusbar_ready &&
                         !(momentum_settings.statusbar_hidden & (1UL << icon));
    FURI_CRITICAL_EXIT();
    return visible;
}

void momentum_settings_set_statusbar_icon_visible(ViewPortStatusBarIcon icon, bool visible) {
    if(icon == ViewPortStatusBarIconNone || icon >= ViewPortStatusBarIconCount) return;
    FURI_CRITICAL_ENTER();
    if(visible) {
        momentum_settings.statusbar_hidden &= ~(1UL << icon);
    } else {
        momentum_settings.statusbar_hidden |= (1UL << icon);
    }
    FURI_CRITICAL_EXIT();
}
