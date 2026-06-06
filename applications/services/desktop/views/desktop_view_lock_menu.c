#include <furi.h>
#include <gui/elements.h>
#include <assets_icons.h>

#include "../desktop_i.h"
#include "desktop_view_lock_menu.h"

#define LOCK_MENU_MAX_ITEMS 6

// Menu items and events are built dynamically from the current toggle states:
//   qFlipper       Enable/Disable (background RPC bridge)   [USB-OTG only]
//   USB-Storage    open the full-screen mass-storage scene  [USB-OTG only]
//   Bluetooth      Enable/Disable
//   Switch to Bruce  reboot into the Bruce firmware         [multi-boot only]
//   Mesh: Off/Master/Client    cycle the mesh role
//   Mesh Clients   open the discovery/pairing scene          [Master only]

typedef struct {
    const char* label;
    DesktopEvent event;
} LockMenuItem;

static LockMenuItem s_items[LOCK_MENU_MAX_ITEMS];
static uint8_t s_item_count = 0;

// Rows that fit on screen below the status bar (each item is 17px tall).
#define LOCK_MENU_VISIBLE 3
static uint8_t s_top = 0; // index of the first visible item

// Keep the selected item inside the visible window.
static void lock_menu_scroll_to(uint8_t idx) {
    if(idx < s_top) {
        s_top = idx;
    } else if(idx >= s_top + LOCK_MENU_VISIBLE) {
        s_top = idx - LOCK_MENU_VISIBLE + 1;
    }
}

static void lock_menu_build_items(
    bool usb_available,
    bool qflipper_on,
    bool bt_on,
    bool bruce_available) {
    s_item_count = 0;

    if(usb_available) {
        s_items[s_item_count++] = (LockMenuItem){
            qflipper_on ? "Disable qFlipper" : "Enable qFlipper",
            DesktopLockMenuEventQflipperToggle};
        s_items[s_item_count++] = (LockMenuItem){"USB-Storage", DesktopLockMenuEventUsbStorage};
    }

    s_items[s_item_count++] = (LockMenuItem){
        bt_on ? "Disable Bluetooth" : "Enable Bluetooth", DesktopLockMenuEventBluetoothToggle};

    if(bruce_available) {
        s_items[s_item_count++] = (LockMenuItem){"Switch to Bruce", DesktopLockMenuEventBruce};
    }

    /* Mesh: der T-Embed ist immer Master — kein Mode-Toggle, "Mesh Clients"
     * (Discovery/Pair) ist immer verfügbar. */
    s_items[s_item_count++] = (LockMenuItem){"Mesh Clients", DesktopLockMenuEventMeshClients};
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

void desktop_lock_menu_set_states(
    DesktopLockMenuView* lock_menu,
    bool usb_available,
    bool qflipper_on,
    bool bt_on,
    bool bruce_available) {
    lock_menu_build_items(usb_available, qflipper_on, bt_on, bruce_available);
    /* Index nicht resetten — Caller (refresh nach Toggle) erwartet, dass die
     * Selektion stehen bleibt; bei out-of-range clampen wir, damit der Wechsel
     * vom Master- in den Off-Modus (verliert "Mesh Clients") nicht ins Leere
     * zeigt. */
    with_view_model(
        lock_menu->view,
        DesktopLockMenuViewModel * model,
        {
            if(model->idx >= s_item_count) model->idx = s_item_count - 1;
            lock_menu_scroll_to(model->idx);
        },
        true);
}

void desktop_lock_menu_draw_callback(Canvas* canvas, void* model) {
    DesktopLockMenuViewModel* m = model;

    canvas_set_color(canvas, ColorBlack);
    canvas_draw_icon(canvas, -57, 0 + STATUS_BAR_Y_SHIFT, &I_DoorLeft_70x55);
    canvas_draw_icon(canvas, 116, 0 + STATUS_BAR_Y_SHIFT, &I_DoorRight_70x55);

    canvas_set_font(canvas, FontSecondary);
    for(uint8_t row = 0; row < LOCK_MENU_VISIBLE; ++row) {
        uint8_t i = s_top + row;
        if(i >= s_item_count) break;

        canvas_draw_str_aligned(
            canvas,
            64,
            9 + (row * 17) + STATUS_BAR_Y_SHIFT,
            AlignCenter,
            AlignCenter,
            s_items[i].label);

        if(m->idx == i) {
            elements_frame(canvas, 15, 1 + (row * 17) + STATUS_BAR_Y_SHIFT, 98, 15);
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
                if(update) lock_menu_scroll_to(model->idx);
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

    // Default until the scene fills in real states on enter.
    lock_menu_build_items(false, false, false, false);

    return lock_menu;
}

void desktop_lock_menu_free(DesktopLockMenuView* lock_menu_view) {
    furi_assert(lock_menu_view);

    view_free(lock_menu_view->view);
    free(lock_menu_view);
}
