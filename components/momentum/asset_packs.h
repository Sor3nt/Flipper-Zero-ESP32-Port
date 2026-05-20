#pragma once

#include <gui/canvas.h>
#include <stdint.h>
#include <stdbool.h>

#define ASSET_PACKS_PATH EXT_PATH("asset_packs")

typedef struct AssetPacks AssetPacks;

void asset_packs_init(void);
void asset_packs_free(void);
AssetPacks* asset_packs_get(void);
bool asset_packs_is_loaded(void);
const Icon* asset_packs_swap_icon(const Icon* requested);
uint8_t* asset_packs_get_font(AssetPacks* ap, Font font);
CanvasFontParameters* asset_packs_get_font_params(AssetPacks* ap, Font font);
