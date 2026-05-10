#include "wlan_lan_view.h"
#include "wlan_view_common.h"
#include "wlan_view_events.h"
#include <gui/canvas.h>
#include <gui/elements.h>
#include <input/input.h>
#include <assets_icons.h>
#include <stdio.h>
#include <string.h>

#define HEADER_HEIGHT (WLAN_VIEW_HEADER_LINE_Y + 1)
#define LINE_HEIGHT   11
#define ITEMS_ON_SCREEN 4 // (64 - 12) / 11 ≈ 4 Items komplett sichtbar.

static uint8_t wlan_lan_view_visible_items(const WlanLanViewModel* model) {
    return model->back_label[0] ? (uint8_t)(ITEMS_ON_SCREEN - 1) : ITEMS_ON_SCREEN;
}

typedef struct {
    View* view;
    ViewDispatcher* view_dispatcher;
} WlanLanViewCtx;

static WlanLanViewCtx s_ctx;

static void wlan_lan_view_draw_header(Canvas* canvas, const WlanLanViewModel* model) {
    const char* title = model->header_title[0] ? model->header_title : "LAN";
    wlan_view_draw_header(canvas, title);

    // Counter über Devices ermitteln, rechts oben.
    uint16_t total = 0, sel = 0;
    for(uint8_t i = 0; i < model->item_count; ++i) {
        if(model->items[i].kind == WlanLanItemKindDevice) {
            total++;
            if(!model->items[i].is_default) sel++;
        }
    }

    if(total > 0) {
        char stats[16];
        if(sel > 0 || model->counter_show_selection_always) {
            snprintf(stats, sizeof(stats), "%u/%u", (unsigned)sel, (unsigned)total);
        } else {
            snprintf(stats, sizeof(stats), "%u", (unsigned)total);
        }
        canvas_set_font(canvas, FontSecondary);
        uint16_t sw = canvas_string_width(canvas, stats);
        canvas_draw_str(canvas, WLAN_VIEW_DISPLAY_W - 3 - sw, WLAN_VIEW_HEADER_BASELINE_Y, stats);
    }
}

static void wlan_lan_view_draw_menu(Canvas* canvas, const WlanLanViewModel* model) {
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
        canvas,
        menu_x,
        menu_y + 1 + line_height,
        menu_x + menu_w - 1,
        menu_y + 1 + line_height);

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, menu_x + 12, menu_y + line_height - 1, "Actions");

    for(uint8_t i = 0; i < model->menu_count; ++i) {
        canvas_draw_str(
            canvas,
            menu_x + 12,
            menu_y + 1 + line_height + (i + 1) * line_height,
            model->menu_items[i].label);
    }

    canvas_draw_icon(
        canvas,
        menu_x + 4,
        menu_y + 4 + (model->menu_selected + 1) * line_height,
        &I_ButtonRight_4x7);
}

static uint16_t wlan_lan_view_compute_col2_x(Canvas* canvas, const WlanLanViewModel* model) {
    uint16_t max_w = 0;
    for(uint8_t i = 0; i < model->item_count; ++i) {
        if(model->items[i].kind != WlanLanItemKindDevice) continue;
        canvas_set_font(canvas,
            model->items[i].col1_small ? FontBatteryPercent : FontSecondary);
        uint16_t w = canvas_string_width(canvas, model->items[i].label);
        if(w > max_w) max_w = w;
    }
    canvas_set_font(canvas, FontSecondary);
    return 3 + max_w + 6; // 3 px linker Rand + max col1 + 6 px Spacer
}

static void wlan_lan_view_draw_callback(Canvas* canvas, void* _model) {
    WlanLanViewModel* model = _model;
    canvas_clear(canvas);
    wlan_lan_view_draw_header(canvas, model);

    if(model->item_count == 0) {
        wlan_view_draw_empty_box(canvas, model->empty_text[0] ? model->empty_text : "(empty)");
        return;
    }

    uint16_t col2_x = wlan_lan_view_compute_col2_x(canvas, model);
    canvas_set_font(canvas, FontSecondary);
    uint8_t visible = wlan_lan_view_visible_items(model);
    if(model->item_count < visible) visible = model->item_count;

    for(uint8_t i = 0; i < visible; i++) {
        uint8_t idx = model->window_offset + i;
        if(idx >= model->item_count) break;

        const WlanLanItem* it = &model->items[idx];
        uint8_t y = HEADER_HEIGHT + 1 + i * LINE_HEIGHT;

        if(it->kind == WlanLanItemKindSeparator) {
            canvas_draw_line(canvas, 4, y + LINE_HEIGHT / 2, 124, y + LINE_HEIGHT / 2);
            continue;
        }

        bool selected = (idx == model->selected);
        bool is_device = (it->kind == WlanLanItemKindDevice);

        if(selected) {
            canvas_set_color(canvas, ColorBlack);
            canvas_draw_box(canvas, 0, y, 122, LINE_HEIGHT);
            canvas_set_color(canvas, ColorWhite);
        }

        if(it->centered) {
            canvas_draw_str_aligned(canvas, 61, y + 9, AlignCenter, AlignBottom, it->label);
        } else if(is_device && it->col1_small) {
            canvas_set_font(canvas, FontBatteryPercent);
            canvas_draw_str(canvas, 3, y + 9, it->label);
            canvas_set_font(canvas, FontSecondary);
        } else {
            canvas_draw_str(canvas, 3, y + 9, it->label);
        }

        if(is_device) {
            if(it->label2[0]) {
                canvas_draw_str(canvas, col2_x, y + 9, it->label2);
            }
            if(it->value_icon) {
                uint16_t iw = icon_get_width(it->value_icon);
                uint16_t ih = icon_get_height(it->value_icon);
                uint8_t iy = y + (LINE_HEIGHT - ih) / 2;
                canvas_draw_icon(canvas, 122 - 2 - iw, iy, it->value_icon);
            } else if(it->value_label[0]) {
                uint16_t vw = canvas_string_width(canvas, it->value_label);
                canvas_draw_str(canvas, 122 - 2 - vw, y + 9, it->value_label);
            }
        }

        if(selected) {
            canvas_set_color(canvas, ColorBlack);
        }
    }

    if(model->item_count > wlan_lan_view_visible_items(model)) {
        elements_scrollbar(canvas, model->selected, model->item_count);
    }

    if(model->back_label[0]) {
        elements_button_left(canvas, model->back_label);
    }

    wlan_lan_view_draw_menu(canvas, model);
}

static bool wlan_lan_view_input_callback(InputEvent* event, void* context) {
    WlanLanViewCtx* ctx = context;
    View* view = ctx->view;
    bool consumed = false;

    if(event->type != InputTypeShort && event->type != InputTypeRepeat &&
       event->type != InputTypeLong) {
        return false;
    }

    WlanLanViewModel* model = view_get_model(view);

    // Menu-Mode hat Vorrang.
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
                    ctx->view_dispatcher, WlanAppCustomEventLanMenuOk);
            }
            return true;
        } else {
            view_commit_model(view, false);
            return true;
        }
    }

    if(model->item_count == 0) {
        view_commit_model(view, false);
        return false;
    }

    if(event->key == InputKeyUp) {
        uint8_t s = model->selected;
        while(s > 0) {
            s--;
            if(model->items[s].kind != WlanLanItemKindSeparator) {
                model->selected = s;
                if(model->selected < model->window_offset) {
                    model->window_offset = model->selected;
                }
                break;
            }
        }
        view_commit_model(view, true);
        consumed = true;
    } else if(event->key == InputKeyDown) {
        uint8_t s = model->selected;
        while(s + 1 < model->item_count) {
            s++;
            if(model->items[s].kind != WlanLanItemKindSeparator) {
                model->selected = s;
                uint8_t v = wlan_lan_view_visible_items(model);
                if(model->selected >= model->window_offset + v) {
                    model->window_offset = (uint8_t)(model->selected - v + 1);
                }
                break;
            }
        }
        view_commit_model(view, true);
        consumed = true;
    } else if(event->key == InputKeyOk && event->type == InputTypeShort) {
        bool is_sep = (model->items[model->selected].kind == WlanLanItemKindSeparator);
        view_commit_model(view, false);
        if(!is_sep && ctx->view_dispatcher) {
            view_dispatcher_send_custom_event(ctx->view_dispatcher, WlanAppCustomEventLanItemOk);
        }
        consumed = true;
    } else if(event->key == InputKeyOk && event->type == InputTypeLong) {
        bool is_device = (model->items[model->selected].kind == WlanLanItemKindDevice);
        view_commit_model(view, false);
        if(is_device && ctx->view_dispatcher) {
            view_dispatcher_send_custom_event(
                ctx->view_dispatcher, WlanAppCustomEventLanItemLongOk);
        }
        consumed = true;
    } else {
        view_commit_model(view, false);
    }

    return consumed;
}

View* wlan_lan_view_alloc(void) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(WlanLanViewModel));

    WlanLanViewModel* model = view_get_model(view);
    memset(model, 0, sizeof(*model));
    view_commit_model(view, false);

    view_set_draw_callback(view, wlan_lan_view_draw_callback);
    view_set_input_callback(view, wlan_lan_view_input_callback);

    s_ctx.view = view;
    s_ctx.view_dispatcher = NULL;
    view_set_context(view, &s_ctx);

    return view;
}

void wlan_lan_view_free(View* view) {
    s_ctx.view = NULL;
    s_ctx.view_dispatcher = NULL;
    view_free(view);
}

void wlan_lan_view_set_view_dispatcher(View* view, ViewDispatcher* vd) {
    UNUSED(view);
    s_ctx.view_dispatcher = vd;
}

void wlan_lan_view_set_header_title(View* view, const char* title) {
    WlanLanViewModel* model = view_get_model(view);
    if(title) {
        strncpy(model->header_title, title, sizeof(model->header_title) - 1);
        model->header_title[sizeof(model->header_title) - 1] = '\0';
    } else {
        model->header_title[0] = '\0';
    }
    view_commit_model(view, true);
}

void wlan_lan_view_set_back_label(View* view, const char* label) {
    WlanLanViewModel* model = view_get_model(view);
    if(label) {
        strncpy(model->back_label, label, sizeof(model->back_label) - 1);
        model->back_label[sizeof(model->back_label) - 1] = '\0';
    } else {
        model->back_label[0] = '\0';
    }
    view_commit_model(view, true);
}

void wlan_lan_view_set_force_selection_counter(View* view, bool force) {
    WlanLanViewModel* model = view_get_model(view);
    model->counter_show_selection_always = force;
    view_commit_model(view, true);
}

void wlan_lan_view_set_empty_text(View* view, const char* text) {
    WlanLanViewModel* model = view_get_model(view);
    if(text) {
        strncpy(model->empty_text, text, sizeof(model->empty_text) - 1);
        model->empty_text[sizeof(model->empty_text) - 1] = '\0';
    } else {
        model->empty_text[0] = '\0';
    }
    view_commit_model(view, true);
}

void wlan_lan_view_clear(View* view) {
    WlanLanViewModel* model = view_get_model(view);
    model->item_count = 0;
    model->selected = 0;
    model->window_offset = 0;
    view_commit_model(view, true);
}

static WlanLanItem* wlan_lan_view_add_internal(
    View* view,
    WlanLanItemKind kind,
    const char* label,
    const char* label2,
    bool centered,
    uint16_t user_id) {
    WlanLanViewModel* model = view_get_model(view);
    if(model->item_count >= WLAN_LAN_VIEW_MAX_ITEMS) {
        view_commit_model(view, false);
        return NULL;
    }
    WlanLanItem* it = &model->items[model->item_count++];
    memset(it, 0, sizeof(*it));
    it->kind = kind;
    it->centered = centered;
    it->user_id = user_id;
    it->is_default = true;
    strncpy(it->label, label ? label : "", sizeof(it->label) - 1);
    strncpy(it->label2, label2 ? label2 : "", sizeof(it->label2) - 1);
    view_commit_model(view, true);
    return it;
}

void wlan_lan_view_add_action(View* view, const char* label, uint16_t user_id) {
    wlan_lan_view_add_internal(view, WlanLanItemKindAction, label, NULL, false, user_id);
}

void wlan_lan_view_add_action_centered(View* view, const char* label, uint16_t user_id) {
    wlan_lan_view_add_internal(view, WlanLanItemKindAction, label, NULL, true, user_id);
}

static void wlan_lan_view_add_device_internal(
    View* view,
    const char* col1,
    const char* col2,
    const char* value_label,
    const Icon* value_icon,
    bool is_default,
    bool col1_small,
    uint16_t user_id) {
    WlanLanViewModel* model = view_get_model(view);
    if(model->item_count >= WLAN_LAN_VIEW_MAX_ITEMS) {
        view_commit_model(view, false);
        return;
    }
    WlanLanItem* it = &model->items[model->item_count++];
    memset(it, 0, sizeof(*it));
    it->kind = WlanLanItemKindDevice;
    it->user_id = user_id;
    it->is_default = is_default;
    it->col1_small = col1_small;
    it->value_icon = value_icon;
    strncpy(it->label, col1 ? col1 : "", sizeof(it->label) - 1);
    strncpy(it->label2, col2 ? col2 : "", sizeof(it->label2) - 1);
    if(!value_icon && value_label) {
        strncpy(it->value_label, value_label, sizeof(it->value_label) - 1);
    }
    view_commit_model(view, true);
}

void wlan_lan_view_add_device(
    View* view,
    const char* col1,
    const char* col2,
    const char* value_label,
    const Icon* value_icon,
    bool is_default,
    uint16_t user_id) {
    wlan_lan_view_add_device_internal(
        view, col1, col2, value_label, value_icon, is_default, false, user_id);
}

void wlan_lan_view_add_device_compact(
    View* view,
    const char* col1,
    const char* col2,
    const char* value_label,
    const Icon* value_icon,
    bool is_default,
    uint16_t user_id) {
    wlan_lan_view_add_device_internal(
        view, col1, col2, value_label, value_icon, is_default, true, user_id);
}

void wlan_lan_view_add_separator(View* view) {
    wlan_lan_view_add_internal(view, WlanLanItemKindSeparator, "", NULL, false, 0);
}

void wlan_lan_view_set_selected(View* view, uint8_t index) {
    WlanLanViewModel* model = view_get_model(view);
    if(index < model->item_count) {
        // Falls auf einen Separator gezeigt wird: zum nächsten echten Item.
        while(index < model->item_count &&
              model->items[index].kind == WlanLanItemKindSeparator) {
            index++;
        }
        if(index >= model->item_count) {
            index = 0;
            while(index < model->item_count &&
                  model->items[index].kind == WlanLanItemKindSeparator) {
                index++;
            }
        }
        if(index < model->item_count) {
            model->selected = index;
            uint8_t v = wlan_lan_view_visible_items(model);
            if(model->selected < model->window_offset) {
                model->window_offset = model->selected;
            } else if(model->selected >= model->window_offset + v) {
                model->window_offset = (uint8_t)(model->selected - v + 1);
            }
        }
    }
    view_commit_model(view, true);
}

uint8_t wlan_lan_view_get_selected(View* view) {
    WlanLanViewModel* model = view_get_model(view);
    uint8_t s = model->selected;
    view_commit_model(view, false);
    return s;
}

WlanLanItem wlan_lan_view_get_item(View* view, uint8_t index) {
    WlanLanViewModel* model = view_get_model(view);
    WlanLanItem out;
    memset(&out, 0, sizeof(out));
    if(index < model->item_count) {
        out = model->items[index];
    }
    view_commit_model(view, false);
    return out;
}

static void wlan_lan_view_apply_value(
    WlanLanItem* it,
    const char* value_label,
    const Icon* value_icon,
    bool is_default) {
    it->value_icon = value_icon;
    it->is_default = is_default;
    memset(it->value_label, 0, sizeof(it->value_label));
    if(!value_icon && value_label) {
        strncpy(it->value_label, value_label, sizeof(it->value_label) - 1);
    }
}

void wlan_lan_view_set_device_value(
    View* view,
    uint8_t index,
    const char* value_label,
    const Icon* value_icon,
    bool is_default) {
    WlanLanViewModel* model = view_get_model(view);
    if(index < model->item_count && model->items[index].kind == WlanLanItemKindDevice) {
        wlan_lan_view_apply_value(&model->items[index], value_label, value_icon, is_default);
    }
    view_commit_model(view, true);
}

void wlan_lan_view_set_device_value_by_user_id(
    View* view,
    uint16_t user_id,
    const char* value_label,
    const Icon* value_icon,
    bool is_default) {
    WlanLanViewModel* model = view_get_model(view);
    for(uint8_t i = 0; i < model->item_count; ++i) {
        if(model->items[i].kind == WlanLanItemKindDevice &&
           model->items[i].user_id == user_id) {
            wlan_lan_view_apply_value(&model->items[i], value_label, value_icon, is_default);
            break;
        }
    }
    view_commit_model(view, true);
}

void wlan_lan_view_clear_menu(View* view) {
    WlanLanViewModel* model = view_get_model(view);
    model->menu_count = 0;
    model->menu_selected = 0;
    view_commit_model(view, true);
}

void wlan_lan_view_add_menu_item(View* view, const char* label, uint16_t user_id) {
    WlanLanViewModel* model = view_get_model(view);
    if(model->menu_count >= WLAN_LAN_VIEW_MAX_MENU_ITEMS) {
        view_commit_model(view, false);
        return;
    }
    WlanLanMenuItem* m = &model->menu_items[model->menu_count++];
    m->user_id = user_id;
    strncpy(m->label, label ? label : "", sizeof(m->label) - 1);
    m->label[sizeof(m->label) - 1] = '\0';
    view_commit_model(view, true);
}

void wlan_lan_view_open_menu(View* view) {
    WlanLanViewModel* model = view_get_model(view);
    model->menu_open = (model->menu_count > 0);
    if(model->menu_selected >= model->menu_count) model->menu_selected = 0;
    view_commit_model(view, true);
}

void wlan_lan_view_close_menu(View* view) {
    WlanLanViewModel* model = view_get_model(view);
    model->menu_open = false;
    view_commit_model(view, true);
}

bool wlan_lan_view_is_menu_open(View* view) {
    WlanLanViewModel* model = view_get_model(view);
    bool b = model->menu_open;
    view_commit_model(view, false);
    return b;
}

uint8_t wlan_lan_view_get_menu_selected(View* view) {
    WlanLanViewModel* model = view_get_model(view);
    uint8_t s = model->menu_selected;
    view_commit_model(view, false);
    return s;
}

WlanLanMenuItem wlan_lan_view_get_menu_item(View* view, uint8_t index) {
    WlanLanViewModel* model = view_get_model(view);
    WlanLanMenuItem out;
    memset(&out, 0, sizeof(out));
    if(index < model->menu_count) out = model->menu_items[index];
    view_commit_model(view, false);
    return out;
}
