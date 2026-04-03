#include <furi.h>
#include <gui/elements.h>
#include <assets_icons.h>
#include <string.h>

#include "../desktop_i.h"
#include "desktop_view_lock_menu.h"

#define LOCK_MENU_MAX_ITEMS 4

// Menu items and events are built dynamically based on wifi state.
// Not connected: Connect Wifi, Sub-GHz
// Connected:     Disconnect Wifi, Sub-GHz, Handshake, Deauth

typedef struct {
    const char* label;
    DesktopEvent event;
} LockMenuItem;

static LockMenuItem s_items[LOCK_MENU_MAX_ITEMS];
static uint8_t s_item_count = 0;

static void lock_menu_build_items(bool wifi_connected) {
    s_item_count = 0;

    if(wifi_connected) {
        s_items[s_item_count++] = (LockMenuItem){"Disconnect WiFi", DesktopLockMenuEventDisconnectWifi};
        s_items[s_item_count++] = (LockMenuItem){"Sub-GHz", DesktopLockMenuEventSubGhz};
        s_items[s_item_count++] = (LockMenuItem){"Handshake", DesktopLockMenuEventHandshake};
        s_items[s_item_count++] = (LockMenuItem){"Deauth", DesktopLockMenuEventDeauth};
    } else {
        s_items[s_item_count++] = (LockMenuItem){"Connect WiFi", DesktopLockMenuEventConnectWifi};
        s_items[s_item_count++] = (LockMenuItem){"Sub-GHz", DesktopLockMenuEventSubGhz};
    }
}

void desktop_lock_menu_set_callback(
    DesktopLockMenuView* lock_menu,
    DesktopLockMenuViewCallback callback,
    void* context) {
    furi_assert(lock_menu);
    furi_assert(callback);
    lock_menu->callback = callback;
    lock_menu->context = context;
}

void desktop_lock_menu_set_idx(DesktopLockMenuView* lock_menu, uint8_t idx) {
    furi_assert(idx < LOCK_MENU_MAX_ITEMS);
    with_view_model(
        lock_menu->view, DesktopLockMenuViewModel * model, { model->idx = idx; }, true);
}

void desktop_lock_menu_set_wifi_state(
    DesktopLockMenuView* lock_menu,
    bool connected,
    const char* ip) {
    lock_menu_build_items(connected);
    with_view_model(
        lock_menu->view,
        DesktopLockMenuViewModel * model,
        {
            model->wifi_connected = connected;
            model->idx = 0;
            if(ip) {
                strncpy(model->ip_str, ip, sizeof(model->ip_str) - 1);
                model->ip_str[sizeof(model->ip_str) - 1] = '\0';
            } else {
                model->ip_str[0] = '\0';
            }
        },
        true);
}

void desktop_lock_menu_draw_callback(Canvas* canvas, void* model) {
    DesktopLockMenuViewModel* m = model;

    canvas_set_color(canvas, ColorBlack);
    canvas_draw_icon(canvas, -57, 0 + STATUS_BAR_Y_SHIFT, &I_DoorLeft_70x55);
    canvas_draw_icon(canvas, 116, 0 + STATUS_BAR_Y_SHIFT, &I_DoorRight_70x55);

    uint8_t y_offset = 0;

    // Show IP with frame when connected
    if(m->wifi_connected && m->ip_str[0]) {
        canvas_set_font(canvas, FontSecondary);
        uint16_t ip_width = canvas_string_width(canvas, m->ip_str);
        int16_t ip_x = (128 - ip_width) / 2 - 2;
        if(ip_x < 2) ip_x = 2;
        elements_frame(canvas, ip_x, STATUS_BAR_Y_SHIFT, ip_width + 4, 12);
        canvas_draw_str_aligned(
            canvas, 64, 6 + STATUS_BAR_Y_SHIFT, AlignCenter, AlignCenter, m->ip_str);
        y_offset = 14;
    }

    canvas_set_font(canvas, FontSecondary);
    for(size_t i = 0; i < s_item_count; ++i) {
        canvas_draw_str_aligned(
            canvas,
            64,
            9 + (i * 17) + STATUS_BAR_Y_SHIFT + y_offset,
            AlignCenter,
            AlignCenter,
            s_items[i].label);

        if(m->idx == i) {
            elements_frame(
                canvas,
                15,
                1 + (i * 17) + STATUS_BAR_Y_SHIFT + y_offset,
                98,
                15);
        }
    }
}

View* desktop_lock_menu_get_view(DesktopLockMenuView* lock_menu) {
    furi_assert(lock_menu);
    return lock_menu->view;
}

bool desktop_lock_menu_input_callback(InputEvent* event, void* context) {
    furi_assert(event);
    furi_assert(context);

    DesktopLockMenuView* lock_menu = context;
    uint8_t idx = 0;
    bool consumed = false;
    bool update = false;

    with_view_model(
        lock_menu->view,
        DesktopLockMenuViewModel * model,
        {
            if((event->type == InputTypeShort) || (event->type == InputTypeRepeat)) {
                if(event->key == InputKeyUp) {
                    if(model->idx == 0) {
                        model->idx = s_item_count - 1;
                    } else {
                        model->idx--;
                    }
                    update = true;
                    consumed = true;
                } else if(event->key == InputKeyDown) {
                    if(model->idx >= s_item_count - 1) {
                        model->idx = 0;
                    } else {
                        model->idx++;
                    }
                    update = true;
                    consumed = true;
                }
            }
            idx = model->idx;
        },
        update);

    if(event->key == InputKeyOk && event->type == InputTypeShort) {
        if(idx < s_item_count) {
            lock_menu->callback(s_items[idx].event, lock_menu->context);
        }
        consumed = true;
    }

    return consumed;
}

DesktopLockMenuView* desktop_lock_menu_alloc(void) {
    DesktopLockMenuView* lock_menu = malloc(sizeof(DesktopLockMenuView));
    lock_menu->view = view_alloc();
    view_allocate_model(lock_menu->view, ViewModelTypeLocking, sizeof(DesktopLockMenuViewModel));
    view_set_context(lock_menu->view, lock_menu);
    view_set_draw_callback(lock_menu->view, (ViewDrawCallback)desktop_lock_menu_draw_callback);
    view_set_input_callback(lock_menu->view, desktop_lock_menu_input_callback);

    // Default: not connected
    lock_menu_build_items(false);

    return lock_menu;
}

void desktop_lock_menu_free(DesktopLockMenuView* lock_menu_view) {
    furi_assert(lock_menu_view);

    view_free(lock_menu_view->view);
    free(lock_menu_view);
}
