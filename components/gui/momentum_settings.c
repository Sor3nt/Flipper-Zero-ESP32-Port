#include "momentum_settings.h"

#include <furi.h>
#include <saved_struct.h>
#include <storage.h>

#define TAG "MomentumSettings"

#define MOMENTUM_SETTINGS_MAGIC (0x4D)
#define MOMENTUM_SETTINGS_VER   (1)

/* Defaults follow Momentum-Firmware lib/momentum/settings.c */
MomentumSettings momentum_settings = {
    .asset_pack = "", // Default
    .anim_speed = 100, // 100%
    .cycle_anims = MOMENTUM_CYCLE_ANIMS_META, // Meta.txt
    .unlock_anims = false, // OFF
    .lock_on_boot = true, // ON
    .bad_pins_format = false, // OFF
    .allow_locked_rpc_usb = false, // OFF
    .allow_locked_rpc_ble = true, // ON (default: don't block Flipper Mobile/BLE RPC when PIN-locked)
    .lockscreen_poweroff = true, // ON
    .lockscreen_time = true, // ON
    .lockscreen_seconds = false, // OFF
    .lockscreen_date = true, // ON
    .lockscreen_statusbar = true, // ON
    .lockscreen_prompt = true, // ON
    .lockscreen_transparent = false, // OFF
    .lockscreen_skip_animation = false, // OFF
    .statusbar_hidden = 0, // all icons shown
};

static bool momentum_settings_loaded = false;

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
    if(momentum_settings_loaded) return;

    MomentumSettings* loaded = malloc(sizeof(MomentumSettings));
    if(saved_struct_load(
           MOMENTUM_SETTINGS_PATH,
           loaded,
           sizeof(MomentumSettings),
           MOMENTUM_SETTINGS_MAGIC,
           MOMENTUM_SETTINGS_VER)) {
        momentum_settings_sanitize(loaded);
        FURI_CRITICAL_ENTER();
        momentum_settings = *loaded;
        FURI_CRITICAL_EXIT();
        momentum_settings_loaded = true;
        FURI_LOG_I(TAG, "Loaded");
    } else {
        /* A missing/old file on a mounted card means: keep the defaults.
         * Without a card, try again on the next call (card mount). */
        Storage* storage = furi_record_open(RECORD_STORAGE);
        momentum_settings_loaded = (storage_sd_status(storage) == FSE_OK);
        furi_record_close(RECORD_STORAGE);
        FURI_LOG_I(TAG, "Using defaults");
    }
    free(loaded);
}

void momentum_settings_save(void) {
    MomentumSettings* snapshot = malloc(sizeof(MomentumSettings));
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
        /* Saved values win over a later deferred load. */
        momentum_settings_loaded = true;
    }
    free(snapshot);
}

void momentum_settings_get_asset_pack(char* out, size_t out_size) {
    furi_check(out);
    furi_check(out_size);
    FURI_CRITICAL_ENTER();
    strlcpy(out, momentum_settings.asset_pack, out_size);
    FURI_CRITICAL_EXIT();
}

void momentum_settings_set_asset_pack(const char* name) {
    char copy[ASSET_PACKS_NAME_LEN];
    strlcpy(copy, name ? name : "", sizeof(copy));
    FURI_CRITICAL_ENTER();
    memcpy(momentum_settings.asset_pack, copy, sizeof(copy));
    FURI_CRITICAL_EXIT();
}

bool momentum_settings_statusbar_icon_visible(ViewPortStatusBarIcon icon) {
    if(icon == ViewPortStatusBarIconNone || icon >= ViewPortStatusBarIconCount) return true;
    return !(momentum_settings.statusbar_hidden & (1UL << icon));
}

void momentum_settings_set_statusbar_icon_visible(ViewPortStatusBarIcon icon, bool visible) {
    if(icon == ViewPortStatusBarIconNone || icon >= ViewPortStatusBarIconCount) return;
    if(visible) {
        momentum_settings.statusbar_hidden &= ~(1UL << icon);
    } else {
        momentum_settings.statusbar_hidden |= (1UL << icon);
    }
}
