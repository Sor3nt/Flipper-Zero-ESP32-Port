/**
 * @file asset_packs.h
 * Momentum-format asset pack icons and fonts.
 *
 * Animations of the selected pack are handled by the desktop animation storage.
 * This module replaces firmware icons (Icons/<Category>/<Name>.bmx, or an
 * animated Icons/<Category>/<Name>/ folder with `meta` + frame_NN.bm) and
 * fonts (Fonts/<Primary|Secondary|Keyboard|BigNumbers|BatteryPercent>.u8f).
 *
 * The pack's icons and fonts are loaded once per boot: at desktop start, or on
 * the first card mount if no card was present then. They are published
 * atomically and never freed while running, so drawing code can never see a
 * released icon; selecting another pack therefore takes effect for icons and
 * fonts after a restart. Drawing code must resolve a swap once per draw
 * (canvas does) and IconAnimation resolves it when allocated, so animations
 * created before a late first mount keep the firmware icon until restart.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "canvas.h"
#include "icon.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Load icons and fonts of the selected asset pack.
 *
 * Does nothing while the SD card is not mounted, so it can be called again on
 * card mount. After the first attempt with a mounted card it is a no-op.
 */
void asset_packs_init(void);

/** Return the asset pack replacement for a firmware icon, or the icon itself. */
const Icon* asset_packs_swap_icon(const Icon* requested);

/** Return the asset pack u8g2 font data for a font, or NULL. */
const uint8_t* asset_packs_get_font(Font font);

/** Return the parameters of an asset pack font, or NULL. */
const CanvasFontParameters* asset_packs_get_font_params(Font font);

/** Return the pack's full passport background (Passport/passport_128x64), or NULL. */
const Icon* asset_packs_get_passport_background(void);

/** Whether selecting `selected_pack` needs a restart for its icons and fonts.
 *
 * True when the loaded icons/fonts belong to another pack and either side
 * actually has icons or fonts. Animations never need a restart.
 */
bool asset_packs_restart_required(const char* selected_pack);

#ifdef __cplusplus
}
#endif
