#include "mesh_view_common.h"

#include <furi.h>
#include <gui/elements.h>
#include <string.h>
#include <stdio.h>

#include "../desktop_i.h" /* STATUS_BAR_Y_SHIFT */

#define HDR_NAME_MAX 12 /* gekürzt, damit "Ch:NN" rechts nicht kollidiert */

void mesh_view_draw_header(Canvas* canvas, const char* name, uint8_t channel) {
    /* Name zentriert. */
    canvas_set_font(canvas, FontPrimary);
    char nm[HDR_NAME_MAX + 1];
    size_t nl = strnlen(name ? name : "", HDR_NAME_MAX);
    if(name) memcpy(nm, name, nl);
    nm[nl] = '\0';
    canvas_draw_str_aligned(canvas, 64, 2 + STATUS_BAR_Y_SHIFT, AlignCenter, AlignTop, nm);

    /* Channel rechtsbündig (nur bekannte Kanäle). */
    if(channel >= 1 && channel <= 13) {
        char ch[8];
        snprintf(ch, sizeof(ch), "Ch:%u", channel);
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 124, 4 + STATUS_BAR_Y_SHIFT, AlignRight, AlignTop, ch);
    }

    canvas_draw_line(canvas, 8, 14 + STATUS_BAR_Y_SHIFT, 120, 14 + STATUS_BAR_Y_SHIFT);
}

void mesh_view_draw_overlay(Canvas* canvas, const char* text) {
    if(!text || !text[0]) return;

    canvas_set_font(canvas, FontPrimary);
    int tw = (int)canvas_string_width(canvas, text);
    int w = tw + 14;
    int h = 20;
    if(w > 126) w = 126;
    int x = (128 - w) / 2;
    int y = (64 - h) / 2;

    /* weiße Fläche, dann fetter Rahmen, dann Text — liegt über dem Inhalt. */
    canvas_set_color(canvas, ColorWhite);
    canvas_draw_rbox(canvas, x, y, w, h, 3);
    canvas_set_color(canvas, ColorBlack);
    elements_bold_rounded_frame(canvas, x, y, w, h);
    canvas_draw_str_aligned(canvas, 64, y + h / 2, AlignCenter, AlignCenter, text);
}
