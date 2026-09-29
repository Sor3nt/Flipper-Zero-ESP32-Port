#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "subghz_spectrum_math.h"
#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))
#define SUBGHZ_SPECTRUM_BINS 128
typedef void View;
typedef void Gui;
typedef void FuriPubSub;
typedef void FuriPubSubSubscription;
typedef void FuriThread;
typedef void FuriMutex;
typedef void SubGhzSpectrumWorker;
typedef void (*SubGhzSpectrumCallback)(int, void*);
typedef void* esp_lcd_panel_handle_t;
typedef struct SubGhzSpectrum SubGhzSpectrum;
static unsigned blit_rows;
static bool furi_hal_display_draw_rgb565(uint16_t x,uint16_t y,uint16_t width,uint16_t height,const uint16_t* pixels) {
    assert(x==0 && y==blit_rows && width==320 && height>0 && height<=8 && pixels);
    assert(y+height<=170 && (size_t)width*height*2<=320*8*2);
    blit_rows+=height;
    return true;
}
