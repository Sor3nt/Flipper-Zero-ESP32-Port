#include "ap_list.h"
#include <gui/canvas.h>
#include <gui/elements.h>
#include <gui/view_dispatcher.h>
#include <assets_icons.h>
#include <input/input.h>
#include <stdio.h>

// Custom event sent when user presses OK
#define AP_LIST_EVENT_SELECTED 100

static void ap_list_draw_callback(Canvas* canvas, void* _model) {
    ApListModel* model = _model;
    canvas_clear(canvas);

    if(model->count == 0) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 32, AlignCenter, AlignCenter, "No APs found");
        return;
    }

    const uint8_t line_height = 13;
    const uint8_t header_height = 12;

    // Header
    canvas_set_font(canvas, FontPrimary);
    char header[32];
    snprintf(header, sizeof(header), "WiFi APs (%d)", model->count);
    canvas_draw_str(canvas, 2, 10, header);
    canvas_draw_line(canvas, 0, header_height, 128, header_height);

    // List items
    canvas_set_font(canvas, FontSecondary);
    uint16_t visible = AP_LIST_ITEMS_ON_SCREEN;
    if(model->count < visible) visible = model->count;

    for(uint16_t i = 0; i < visible; i++) {
        uint16_t idx = model->window_offset + i;
        if(idx >= model->count) break;

        uint8_t y = header_height + 2 + i * line_height;
        ApListRecord* ap = (ApListRecord*)&model->records[idx];

        if(idx == model->selected) {
            canvas_draw_box(canvas, 0, y - 1, 118, line_height);
            canvas_set_color(canvas, ColorWhite);
        }

        const char* ssid = ap->ssid[0] ? ap->ssid : "???";
        char line[64];
        snprintf(line, sizeof(line), "%.14s %ddB ch%d",
                 ssid, ap->rssi, ap->channel);
        canvas_draw_str(canvas, 2, y + 9, line);

        // Lock/Unlock icon (open vs encrypted)
        const Icon* icon = ap->is_open ? &I_Unlock_7x8 : &I_Lock_7x8;
        canvas_draw_icon(canvas, 109, y + 2, icon);

        if(idx == model->selected) {
            canvas_set_color(canvas, ColorBlack);
        }
    }

    // Scrollbar
    if(model->count > AP_LIST_ITEMS_ON_SCREEN) {
        elements_scrollbar(canvas, model->selected, model->count);
    }
}

typedef struct {
    View* view;
    ViewDispatcher* view_dispatcher;
} ApListContext;

static bool ap_list_input_callback(InputEvent* event, void* context) {
    ApListContext* ctx = context;
    View* view = ctx->view;
    bool consumed = false;

    if(event->type == InputTypeShort || event->type == InputTypeRepeat) {
        ApListModel* model = view_get_model(view);
        uint16_t count = model->count;

        if(event->key == InputKeyUp && count > 0) {
            if(model->selected > 0) {
                model->selected--;
                if(model->selected < model->window_offset) {
                    model->window_offset = model->selected;
                }
            }
            view_commit_model(view, true);
            consumed = true;
        } else if(event->key == InputKeyDown && count > 0) {
            if(model->selected < count - 1) {
                model->selected++;
                if(model->selected >= model->window_offset + AP_LIST_ITEMS_ON_SCREEN) {
                    model->window_offset = model->selected - AP_LIST_ITEMS_ON_SCREEN + 1;
                }
            }
            view_commit_model(view, true);
            consumed = true;
        } else if(event->key == InputKeyOk && count > 0 && event->type == InputTypeShort) {
            view_commit_model(view, false);
            view_dispatcher_send_custom_event(ctx->view_dispatcher, AP_LIST_EVENT_SELECTED);
            consumed = true;
        } else {
            view_commit_model(view, false);
        }
    }

    return consumed;
}

static ApListContext s_ap_list_ctx;

View* ap_list_alloc(void) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(ApListModel));
    view_set_draw_callback(view, ap_list_draw_callback);
    view_set_input_callback(view, ap_list_input_callback);
    s_ap_list_ctx.view = view;
    s_ap_list_ctx.view_dispatcher = NULL;
    view_set_context(view, &s_ap_list_ctx);
    return view;
}

void ap_list_free(View* view) {
    view_free(view);
}

void ap_list_set_view_dispatcher(ViewDispatcher* vd) {
    s_ap_list_ctx.view_dispatcher = vd;
}
