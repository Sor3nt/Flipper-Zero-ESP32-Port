#include "wlan_connect_view.h"
#include "wlan_view_common.h"
#include "wlan_view_events.h"
#include <gui/canvas.h>
#include <gui/elements.h>
#include <gui/icon.h>
#include <input/input.h>
#include <assets_icons.h>
#include <stdio.h>
#include <string.h>

static void wlan_connect_view_draw_menu(Canvas* canvas, const WlanConnectViewModel* model) {
    if(!model->menu_open || model->menu_count == 0) return;

    const uint8_t menu_x = 70;
    const uint8_t menu_y = 0;
    const uint8_t menu_w = 58;
    const uint8_t line_height = 10;
    uint8_t menu_h = (1 + model->menu_count) * line_height + 6;

    canvas_set_color(canvas, ColorWhite);
    canvas_draw_box(canvas, menu_x + 1, menu_y + 1, menu_w - 2, menu_h - 2);
    canvas_set_color(canvas, ColorBlack);
    elements_slightly_rounded_frame(canvas, menu_x, menu_y, menu_w, menu_h);
    canvas_draw_line(
        canvas, menu_x, menu_y + 1 + line_height,
        menu_x + menu_w - 1, menu_y + 1 + line_height);

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, menu_x + 12, menu_y + line_height - 1, "Actions");

    for(uint8_t i = 0; i < model->menu_count; ++i) {
        canvas_draw_str(
            canvas, menu_x + 12,
            menu_y + 1 + line_height + (i + 1) * line_height,
            model->menu_items[i].label);
    }

    canvas_draw_icon(
        canvas, menu_x + 4,
        menu_y + 4 + (model->menu_selected + 1) * line_height,
        &I_ButtonRight_4x7);
}

#define HEADER_HEIGHT (WLAN_VIEW_HEADER_LINE_Y + 1)
#define LINE_HEIGHT   11
#define ITEMS_ON_SCREEN 4

typedef struct {
    View* view;
    ViewDispatcher* view_dispatcher;
} WlanConnectViewCtx;

static WlanConnectViewCtx s_ctx;

static void wlan_connect_view_draw_callback(Canvas* canvas, void* _model) {
    WlanConnectViewModel* model = _model;
    canvas_clear(canvas);

    wlan_view_draw_header(canvas, "Networks");

    if(model->ap_count > 0) {
        canvas_set_font(canvas, FontSecondary);
        char count_buf[8];
        snprintf(count_buf, sizeof(count_buf), "%u", (unsigned)model->ap_count);
        uint16_t cw = canvas_string_width(canvas, count_buf);
        canvas_draw_str(
            canvas, WLAN_VIEW_DISPLAY_W - 7 - cw, WLAN_VIEW_HEADER_BASELINE_Y, count_buf);
    }

    if(model->ap_count == 0) {
        wlan_view_draw_empty_box(canvas, "No networks found");
        return;
    }

    canvas_set_font(canvas, FontSecondary);
    uint8_t visible = ITEMS_ON_SCREEN;
    if(model->ap_count < visible) visible = model->ap_count;

    for(uint8_t i = 0; i < visible; ++i) {
        uint8_t idx = model->window_offset + i;
        if(idx >= model->ap_count) break;

        const WlanConnectViewAp* ap = &model->aps[idx];
        uint8_t y = HEADER_HEIGHT + 1 + i * LINE_HEIGHT;
        bool selected = (idx == model->selected);

        if(selected) {
            canvas_set_color(canvas, ColorBlack);
            canvas_draw_box(canvas, 0, y, 122, LINE_HEIGHT);
            canvas_set_color(canvas, ColorWhite);
        }

        canvas_draw_str(canvas, 3, y + 9, ap->ssid);

        const Icon* icon = ap->unlocked ? &I_Unlock_7x8 : &I_Lock_7x8;
        uint16_t iw = icon_get_width(icon);
        uint16_t ih = icon_get_height(icon);
        uint8_t iy = y + (LINE_HEIGHT - ih) / 2;
        canvas_draw_icon(canvas, 122 - 2 - iw, iy, icon);

        if(selected) {
            canvas_set_color(canvas, ColorBlack);
        }
    }

    if(model->ap_count > ITEMS_ON_SCREEN) {
        elements_scrollbar(canvas, model->selected, model->ap_count);
    }

    wlan_connect_view_draw_menu(canvas, model);
}

static bool wlan_connect_view_input_callback(InputEvent* event, void* context) {
    WlanConnectViewCtx* ctx = context;
    View* view = ctx->view;
    bool consumed = false;

    if(event->type != InputTypeShort && event->type != InputTypeRepeat &&
       event->type != InputTypeLong) {
        return false;
    }

    WlanConnectViewModel* model = view_get_model(view);

    // Modal-Menu hat Vorrang.
    if(model->menu_open && model->menu_count > 0) {
        if(event->key == InputKeyBack && event->type == InputTypeShort) {
            model->menu_open = false;
            view_commit_model(view, true);
            return true;
        } else if(event->key == InputKeyUp) {
            if(model->menu_selected > 0) model->menu_selected--;
            view_commit_model(view, true);
            return true;
        } else if(event->key == InputKeyDown) {
            if(model->menu_selected + 1 < model->menu_count) model->menu_selected++;
            view_commit_model(view, true);
            return true;
        } else if(event->key == InputKeyOk && event->type == InputTypeShort) {
            view_commit_model(view, false);
            if(ctx->view_dispatcher) {
                view_dispatcher_send_custom_event(
                    ctx->view_dispatcher, WlanAppCustomEventConnectMenuOk);
            }
            return true;
        } else {
            view_commit_model(view, false);
            return true;
        }
    }

    if(model->ap_count == 0) {
        view_commit_model(view, false);
        return false;
    }

    if(event->key == InputKeyUp) {
        if(model->selected > 0) {
            model->selected--;
            if(model->selected < model->window_offset) {
                model->window_offset = model->selected;
            }
        }
        view_commit_model(view, true);
        consumed = true;
    } else if(event->key == InputKeyDown) {
        if(model->selected + 1 < model->ap_count) {
            model->selected++;
            if(model->selected >= model->window_offset + ITEMS_ON_SCREEN) {
                model->window_offset = model->selected - ITEMS_ON_SCREEN + 1;
            }
        }
        view_commit_model(view, true);
        consumed = true;
    } else if(event->key == InputKeyOk && event->type == InputTypeShort) {
        view_commit_model(view, false);
        if(ctx->view_dispatcher) {
            view_dispatcher_send_custom_event(
                ctx->view_dispatcher, WlanAppCustomEventApSelected);
        }
        consumed = true;
    } else if(event->key == InputKeyOk && event->type == InputTypeLong) {
        view_commit_model(view, false);
        if(ctx->view_dispatcher) {
            view_dispatcher_send_custom_event(
                ctx->view_dispatcher, WlanAppCustomEventConnectLongOk);
        }
        consumed = true;
    } else {
        view_commit_model(view, false);
    }

    return consumed;
}

View* wlan_connect_view_alloc(void) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(WlanConnectViewModel));

    WlanConnectViewModel* model = view_get_model(view);
    memset(model, 0, sizeof(*model));
    view_commit_model(view, false);

    view_set_draw_callback(view, wlan_connect_view_draw_callback);
    view_set_input_callback(view, wlan_connect_view_input_callback);

    s_ctx.view = view;
    s_ctx.view_dispatcher = NULL;
    view_set_context(view, &s_ctx);

    return view;
}

void wlan_connect_view_free(View* view) {
    s_ctx.view = NULL;
    s_ctx.view_dispatcher = NULL;
    view_free(view);
}

void wlan_connect_view_set_view_dispatcher(View* view, ViewDispatcher* vd) {
    UNUSED(view);
    s_ctx.view_dispatcher = vd;
}

void wlan_connect_view_clear(View* view) {
    WlanConnectViewModel* model = view_get_model(view);
    model->ap_count = 0;
    model->selected = 0;
    model->window_offset = 0;
    view_commit_model(view, true);
}

void wlan_connect_view_add_ap(View* view, const char* ssid, bool unlocked, uint16_t user_id) {
    WlanConnectViewModel* model = view_get_model(view);
    if(model->ap_count >= WLAN_CONNECT_VIEW_MAX_APS) {
        view_commit_model(view, false);
        return;
    }
    WlanConnectViewAp* ap = &model->aps[model->ap_count++];
    memset(ap, 0, sizeof(*ap));
    ap->unlocked = unlocked;
    ap->user_id = user_id;
    strncpy(ap->ssid, ssid ? ssid : "", sizeof(ap->ssid) - 1);
    view_commit_model(view, true);
}

void wlan_connect_view_set_selected(View* view, uint8_t index) {
    WlanConnectViewModel* model = view_get_model(view);
    if(index < model->ap_count) {
        model->selected = index;
        if(model->selected < model->window_offset) {
            model->window_offset = model->selected;
        } else if(model->selected >= model->window_offset + ITEMS_ON_SCREEN) {
            model->window_offset = model->selected - ITEMS_ON_SCREEN + 1;
        }
    }
    view_commit_model(view, true);
}

uint8_t wlan_connect_view_get_selected(View* view) {
    WlanConnectViewModel* model = view_get_model(view);
    uint8_t s = model->selected;
    view_commit_model(view, false);
    return s;
}

void wlan_connect_view_clear_menu(View* view) {
    WlanConnectViewModel* model = view_get_model(view);
    model->menu_count = 0;
    model->menu_selected = 0;
    view_commit_model(view, true);
}

void wlan_connect_view_add_menu_item(View* view, const char* label, uint16_t user_id) {
    WlanConnectViewModel* model = view_get_model(view);
    if(model->menu_count >= WLAN_CONNECT_VIEW_MAX_MENU_ITEMS) {
        view_commit_model(view, false);
        return;
    }
    WlanConnectMenuItem* m = &model->menu_items[model->menu_count++];
    m->user_id = user_id;
    strncpy(m->label, label ? label : "", sizeof(m->label) - 1);
    m->label[sizeof(m->label) - 1] = '\0';
    view_commit_model(view, true);
}

void wlan_connect_view_open_menu(View* view) {
    WlanConnectViewModel* model = view_get_model(view);
    model->menu_open = (model->menu_count > 0);
    if(model->menu_selected >= model->menu_count) model->menu_selected = 0;
    view_commit_model(view, true);
}

void wlan_connect_view_close_menu(View* view) {
    WlanConnectViewModel* model = view_get_model(view);
    model->menu_open = false;
    view_commit_model(view, true);
}

bool wlan_connect_view_is_menu_open(View* view) {
    WlanConnectViewModel* model = view_get_model(view);
    bool b = model->menu_open;
    view_commit_model(view, false);
    return b;
}

uint8_t wlan_connect_view_get_menu_selected(View* view) {
    WlanConnectViewModel* model = view_get_model(view);
    uint8_t s = model->menu_selected;
    view_commit_model(view, false);
    return s;
}

WlanConnectMenuItem wlan_connect_view_get_menu_item(View* view, uint8_t index) {
    WlanConnectViewModel* model = view_get_model(view);
    WlanConnectMenuItem out;
    memset(&out, 0, sizeof(out));
    if(index < model->menu_count) out = model->menu_items[index];
    view_commit_model(view, false);
    return out;
}
