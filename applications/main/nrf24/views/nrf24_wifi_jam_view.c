#include "nrf24_wifi_jam_view.h"

#include <gui/canvas.h>
#include <gui/elements.h>
#include <gui/view_dispatcher.h>
#include <input/input.h>
#include <assets_icons.h>
#include <stdio.h>
#include <string.h>

static void truncate_ssid(char* dst, size_t dst_size, const char* src, size_t max_chars) {
    size_t src_len = strlen(src);
    if(src_len <= max_chars) {
        snprintf(dst, dst_size, "%s", src);
        return;
    }
    if(max_chars >= 3) {
        snprintf(dst, dst_size, "%.*s...", (int)(max_chars - 3), src);
    } else {
        snprintf(dst, dst_size, "%.*s", (int)max_chars, src);
    }
}

static void nrf24_wifi_jam_draw_callback(Canvas* canvas, void* _model) {
    Nrf24WifiJamModel* model = _model;
    canvas_clear(canvas);

    canvas_set_color(canvas, ColorBlack);

    /* ---- Header: "<SSID> | CH: N" + separator line ---- */
    canvas_set_font(canvas, FontPrimary);
    char ssid[20];
    truncate_ssid(ssid, sizeof(ssid), model->ssid[0] ? model->ssid : "-", 14);
    char header[40];
    snprintf(header, sizeof(header), "%s | CH: %u", ssid, model->wifi_channel);
    canvas_draw_str_aligned(canvas, 64, 2, AlignCenter, AlignTop, header);
    canvas_draw_line(canvas, 0, 11, 127, 11);

    if(!model->hardware_ok) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_icon(canvas, 60, 22, &I_Quest_7x8);
        canvas_draw_str_aligned(canvas, 64, 38, AlignCenter, AlignCenter, "NRF24 not found");
        return;
    }

    /* ---- MHZ range right ---- */
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 87, 32, "MHZ");

    canvas_set_font(canvas, FontPrimary);
    char range[16];
    snprintf(
        range,
        sizeof(range),
        "%u-%u",
        2400u + model->nrf_ch_min,
        2400u + model->nrf_ch_max);
    canvas_draw_str_aligned(canvas, 96, 36, AlignCenter, AlignTop, range);

    /* ---- Footer: status + button hint ---- */
//    canvas_set_font(canvas, FontPrimary);
//    canvas_draw_str(canvas, 2, 62, model->running ? "Running" : "Stopped");

    canvas_set_font(canvas, FontSecondary);
    elements_button_center(canvas, model->running ? "Pause" : "Run");


    /* ---- Hero icon left ---- */
    canvas_draw_icon(canvas, 2, 14, &I_Connect_me_62x31);

}

static bool nrf24_wifi_jam_input_callback(InputEvent* event, void* context) {
    ViewDispatcher* vd = context;
    if(event->type == InputTypeShort && event->key == InputKeyOk) {
        view_dispatcher_send_custom_event(vd, Nrf24WifiJamEventToggle);
        return true;
    }
    return false;
}

View* nrf24_wifi_jam_view_alloc(void) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(Nrf24WifiJamModel));
    view_set_draw_callback(view, nrf24_wifi_jam_draw_callback);
    view_set_input_callback(view, nrf24_wifi_jam_input_callback);
    return view;
}

void nrf24_wifi_jam_view_free(View* view) {
    view_free(view);
}
