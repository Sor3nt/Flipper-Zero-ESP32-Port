#include "wlan_view_common.h"
#include <gui/elements.h>
#include <stdio.h>

void wlan_view_draw_header(Canvas* canvas, const char* title) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(
        canvas,
        WLAN_VIEW_DISPLAY_W / 2,
        WLAN_VIEW_HEADER_BASELINE_Y,
        AlignCenter,
        AlignBottom,
        title ? title : "");
    canvas_draw_line(
        canvas, 0, WLAN_VIEW_HEADER_LINE_Y, WLAN_VIEW_DISPLAY_W, WLAN_VIEW_HEADER_LINE_Y);
}

void wlan_view_draw_empty_box(Canvas* canvas, const char* text) {
    if(!text || !text[0]) return;
    canvas_set_font(canvas, FontSecondary);
    uint16_t tw = canvas_string_width(canvas, text);
    uint8_t pad_x = 6;
    uint8_t pad_y = 4;
    uint8_t bw = (uint8_t)(tw + pad_x * 2);
    if(bw > WLAN_VIEW_DISPLAY_W - 4) bw = WLAN_VIEW_DISPLAY_W - 4;
    uint8_t bh = (uint8_t)(8 + pad_y * 2);
    uint8_t bx = (uint8_t)((WLAN_VIEW_DISPLAY_W - bw) / 2);
    uint8_t by = (uint8_t)((WLAN_VIEW_HEADER_LINE_Y + 64 - bh) / 2);
    elements_slightly_rounded_frame(canvas, bx, by, bw, bh);
    canvas_draw_str_aligned(
        canvas, WLAN_VIEW_DISPLAY_W / 2, by + bh / 2 + 1, AlignCenter, AlignCenter, text);
}

void wlan_view_draw_target_counter(Canvas* canvas, uint8_t count) {
    if(count == 0) return;
    char tbuf[12];
    snprintf(tbuf, sizeof(tbuf), "T:%u", (unsigned)count);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(
        canvas,
        WLAN_VIEW_DISPLAY_W / 2,
        WLAN_VIEW_TARGET_COUNTER_Y,
        AlignCenter,
        AlignBottom,
        tbuf);
}
