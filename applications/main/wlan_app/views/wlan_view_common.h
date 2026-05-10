#pragma once

#include <gui/canvas.h>
#include <stdbool.h>
#include <stdint.h>

/** Standard-Layout-Konstanten für alle Custom-Views der WLAN-App. */
#define WLAN_VIEW_HEADER_BASELINE_Y 10
#define WLAN_VIEW_HEADER_LINE_Y     11
#define WLAN_VIEW_DISPLAY_W         128
#define WLAN_VIEW_TARGET_COUNTER_Y  61

/** Header zeichnen: Title (FontPrimary) zentriert, Trennlinie auf Standardhöhe. */
void wlan_view_draw_header(Canvas* canvas, const char* title);

/** Empty-State: gerundeter Rahmen mit zentriertem Text in der Mitte des
 *  Listenbereichs (zwischen Header-Linie und Bildunterkante). */
void wlan_view_draw_empty_box(Canvas* canvas, const char* text);

/** Target-Counter "T:<n>" zentriert zwischen den Soft-Buttons; bei
 *  count=0 wird nichts gezeichnet. */
void wlan_view_draw_target_counter(Canvas* canvas, uint8_t count);
