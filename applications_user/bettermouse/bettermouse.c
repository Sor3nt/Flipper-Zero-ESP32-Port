#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_usb.h>
#include <furi_hal_usb_hid.h>
#include <furi_hal_board_info.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/widget.h>
#include <dialogs/dialogs.h>
#include <notification/notification.h>

#include "bm_mouse.h"
#include "bm_settings.h"

#include <stddef.h>

#define TAG "BetterMouse"

// ESP32 port notes (see PORT_NOTES.md)
//  - USB is taken as an exclusive LEASE and HID is selected with
//    set_config_for_owner (the stock mouse_click_recorder pattern), NOT the
//    upstream furi_hal_usb_unlock()/set_config(&usb_hid) which would rip USB
//    from a running RPC/qFlipper session. Light sleep is inhibited while the
//    lease is held. Refused leases show "USB is busy" instead of a reset.

static const char bm_usb_owner[] = "Better Mouse";

#define BM_RELEASE_TRIES 5
#define BM_RELEASE_RETRY_MS 100

typedef enum {
    BmViewMenu,
    BmViewMouse,
    BmViewSettings,
    BmViewHelp,
} BmView;

typedef enum {
    BmMenuIndexMouse,
    BmMenuIndexSettings,
    BmMenuIndexHelp,
} BmMenuIndex;

typedef enum {
    BmCustomEventExitMouse = 100,
} BmCustomEvent;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    NotificationApp* notifications;

    Submenu* submenu;
    VariableItemList* variable_item_list;
    Widget* widget;
    BmMouse* bm_mouse;

    /* The view dispatcher has no getter for this, so we track it ourselves. */
    uint32_t current_view;

    /* Scratch for formatting setting values; the list copies the string. */
    char value_text[16];

    /* USB ownership — given back on exit, retried on failure. */
    FuriHalUsbInterface* usb_prev;
    bool usb_lease;
    bool sleep_inhibited;

    BmSettings settings;
} BetterMouse;

/* ----------------------------------------------------------------- USB lease */

static bool bm_usb_release(BetterMouse* app) {
    if(app->usb_lease) {
        (void)furi_hal_usb_set_config_for_owner(bm_usb_owner, NULL, NULL); // stop HID
        if(!furi_hal_usb_hid_release_for_owner(bm_usb_owner)) {
            FURI_LOG_W(TAG, "HID stack not released; USB-Serial-JTAG may stay off until reboot");
        }
        if(furi_hal_usb_lease_release(bm_usb_owner, app->usb_prev)) {
            app->usb_lease = false;
            app->usb_prev = NULL;
        } else if(!furi_hal_usb_is_lease_owner(bm_usb_owner)) {
            app->usb_lease = false;
            app->usb_prev = NULL;
        } else {
            return false; // still ours — keep the sleep veto, retry later
        }
    }
    if(app->sleep_inhibited) {
        furi_hal_power_light_sleep_inhibit_exit();
        app->sleep_inhibited = false;
    }
    return true;
}

static bool bm_usb_acquire(BetterMouse* app) {
    if(!furi_hal_usb_lease_acquire(bm_usb_owner, &app->usb_prev)) {
        return false; // locked (RPC), leased elsewhere, or USB storage
    }
    app->usb_lease = true;
    if(!furi_hal_usb_set_config_for_owner(bm_usb_owner, &usb_hid, NULL)) {
        (void)bm_usb_release(app);
        return false;
    }
    furi_hal_power_light_sleep_inhibit_enter();
    app->sleep_inhibited = true;
    return true;
}

/* ------------------------------------------------------------------ settings */

typedef struct {
    const char* label;
    const BmRange* range;
    const char* unit;
    size_t offset; /* into BmSettings */
    bool wide; /* field is uint16_t rather than uint8_t */
} BmTunable;

#define BM_TUNABLE(lbl, rng, un, field, is_wide)                             \
    {.label = (lbl),                                                         \
     .range = (rng),                                                         \
     .unit = (un),                                                           \
     .offset = offsetof(BmSettings, field),                                  \
     .wide = (is_wide)}

static const BmTunable bm_tunables[] = {
    BM_TUNABLE("Max speed", &bm_range_max_speed, "/s", max_speed, true),
    BM_TUNABLE("Pre-ramp", &bm_range_min_speed, "/s", min_speed, true),
    BM_TUNABLE("Accel ramp", &bm_range_accel, "ms", accel_ms, true),
    BM_TUNABLE("Scroll speed", &bm_range_scroll, "/s", scroll_speed, false),
    BM_TUNABLE("Report rate", &bm_range_tick, "Hz", tick_hz, true),
    BM_TUNABLE("Ice glide", &bm_range_ice_glide, "ms", ice_glide_ms, true),
};

static uint16_t bm_tunable_get(const BetterMouse* app, const BmTunable* t) {
    const uint8_t* base = (const uint8_t*)&app->settings;
    if(t->wide) {
        uint16_t v;
        memcpy(&v, base + t->offset, sizeof(v));
        return v;
    }
    return *(base + t->offset);
}

static void bm_tunable_set(BetterMouse* app, const BmTunable* t, uint16_t value) {
    uint8_t* base = (uint8_t*)&app->settings;
    if(t->wide) {
        memcpy(base + t->offset, &value, sizeof(value));
    } else {
        *(base + t->offset) = (uint8_t)value;
    }
}

static void bm_tunable_changed(VariableItem* item) {
    BetterMouse* app = variable_item_get_context(item);
    uint8_t list_index = variable_item_list_get_selected_item_index(app->variable_item_list);
    if(list_index >= COUNT_OF(bm_tunables)) return;

    const BmTunable* t = &bm_tunables[list_index];
    uint16_t value = bm_range_value(t->range, variable_item_get_current_value_index(item));
    bm_tunable_set(app, t, value);

    snprintf(app->value_text, sizeof(app->value_text), "%u%s", (unsigned)value, t->unit);
    variable_item_set_current_value_text(item, app->value_text);
}

static void bm_toggle_changed(VariableItem* item, uint8_t* field) {
    uint8_t index = variable_item_get_current_value_index(item);
    *field = index;
    variable_item_set_current_value_text(item, bm_toggle_names[index]);
}

static void bm_setting_ice(VariableItem* item) {
    BetterMouse* app = variable_item_get_context(item);
    bm_toggle_changed(item, &app->settings.ice);
}

static void bm_setting_invert_y(VariableItem* item) {
    BetterMouse* app = variable_item_get_context(item);
    bm_toggle_changed(item, &app->settings.invert_y);
}

static void bm_setting_invert_scroll(VariableItem* item) {
    BetterMouse* app = variable_item_get_context(item);
    bm_toggle_changed(item, &app->settings.invert_scroll);
}

static void bm_setting_haptic(VariableItem* item) {
    BetterMouse* app = variable_item_get_context(item);
    bm_toggle_changed(item, &app->settings.haptic);
}

static void bm_add_toggle(
    BetterMouse* app,
    const char* label,
    uint8_t value,
    VariableItemChangeCallback callback) {
    VariableItem* item = variable_item_list_add(app->variable_item_list, label, 2, callback, app);
    variable_item_set_current_value_index(item, value);
    variable_item_set_current_value_text(item, bm_toggle_names[value]);
}

static void bm_settings_build(BetterMouse* app) {
    for(size_t i = 0; i < COUNT_OF(bm_tunables); i++) {
        const BmTunable* t = &bm_tunables[i];
        uint16_t value = bm_tunable_get(app, t);

        VariableItem* item = variable_item_list_add(
            app->variable_item_list, t->label, t->range->count, bm_tunable_changed, app);
        variable_item_set_current_value_index(item, bm_range_index(t->range, value));

        snprintf(app->value_text, sizeof(app->value_text), "%u%s", (unsigned)value, t->unit);
        variable_item_set_current_value_text(item, app->value_text);
    }

    bm_add_toggle(app, "Ice mode", app->settings.ice, bm_setting_ice);
    bm_add_toggle(app, "Invert Y", app->settings.invert_y, bm_setting_invert_y);
    bm_add_toggle(app, "Invert scroll", app->settings.invert_scroll, bm_setting_invert_scroll);
    bm_add_toggle(app, "Click vibro", app->settings.haptic, bm_setting_haptic);
}

/* --------------------------------------------------------------- navigation */

static void bm_switch_to(BetterMouse* app, uint32_t view_id) {
    app->current_view = view_id;
    view_dispatcher_switch_to_view(app->view_dispatcher, view_id);
}

static void bm_submenu_callback(void* context, uint32_t index) {
    BetterMouse* app = context;

    if(index == BmMenuIndexMouse) {
        bm_switch_to(app, BmViewMouse);
    } else if(index == BmMenuIndexSettings) {
        bm_switch_to(app, BmViewSettings);
    } else if(index == BmMenuIndexHelp) {
        bm_switch_to(app, BmViewHelp);
    }
}

static bool bm_custom_event_callback(void* context, uint32_t event) {
    BetterMouse* app = context;

    if(event == BmCustomEventExitMouse) {
        bm_switch_to(app, BmViewMenu);
        return true;
    }
    return false;
}

static bool bm_navigation_callback(void* context) {
    BetterMouse* app = context;

    if(app->current_view == BmViewMenu) {
        view_dispatcher_stop(app->view_dispatcher);
        return true;
    }

    if(app->current_view == BmViewSettings) bm_settings_save(&app->settings);

    bm_switch_to(app, BmViewMenu);
    return true;
}

static void bm_mouse_exit_request(void* context) {
    BetterMouse* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, BmCustomEventExitMouse);
}

/* --------------------------------------------------------------------- app */

static BetterMouse* bettermouse_alloc(void) {
    BetterMouse* app = malloc(sizeof(BetterMouse));
    memset(app, 0, sizeof(BetterMouse));

    bm_settings_load(&app->settings);

    app->gui = furi_record_open(RECORD_GUI);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);

    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, bm_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, bm_navigation_callback);

    app->submenu = submenu_alloc();
    submenu_add_item(app->submenu, "Mouse", BmMenuIndexMouse, bm_submenu_callback, app);
    submenu_add_item(app->submenu, "Settings", BmMenuIndexSettings, bm_submenu_callback, app);
    submenu_add_item(app->submenu, "Controls", BmMenuIndexHelp, bm_submenu_callback, app);
    view_dispatcher_add_view(app->view_dispatcher, BmViewMenu, submenu_get_view(app->submenu));

    app->bm_mouse = bm_mouse_alloc(&app->settings, app->notifications);
    bm_mouse_set_exit_callback(app->bm_mouse, bm_mouse_exit_request, app);
    view_dispatcher_add_view(app->view_dispatcher, BmViewMouse, bm_mouse_get_view(app->bm_mouse));

    app->variable_item_list = variable_item_list_alloc();
    bm_settings_build(app);
    view_dispatcher_add_view(
        app->view_dispatcher,
        BmViewSettings,
        variable_item_list_get_view(app->variable_item_list));

    app->widget = widget_alloc();
    widget_add_text_scroll_element(
        app->widget,
        0,
        0,
        128,
        64,
        // PORT: encoder boards get their own key map (bm_mouse_input_event_encoder).
        furi_hal_board_get_input_layout() == FuriHalBoardInputLayoutEncoder ?
            "\e#Controls\n"
            "Turn: move up/down\n"
            "OK hold + turn:\n"
            "  move left/right\n"
            "Spin fast = bigger\n"
            "  steps\n"
            "OK: left click\n"
            "OK hold: drag lock,\n"
            "  click OK to drop\n"
            "Side key: right click\n"
            "\n\e#Scroll\n"
            "Hold side key, then\n"
            "click OK: the wheel\n"
            "scrolls. Again: off.\n"
            "\n\e#Menu / exit\n"
            "Hold the side key\n"
            "~1.3s for this menu\n"
            "(MENU warns first).\n"
            "Back again leaves.\n"
            "\n\e#Ice mode\n"
            "Pointer keeps its\n"
            "momentum and coasts\n"
            "to a stop. Ice glide\n"
            "sets how long.\n" :
            "\e#Controls\n"
            "Arrows: move\n"
            "OK: left click\n"
            "OK hold: drag lock,\n"
            "  press OK to drop\n"
            "Back tap: right click\n"
            "\n\e#Scroll\n"
            "Back hold + OK\n"
            "toggles the wheel.\n"
            "\n\e#Menu / exit\n"
            "App opens straight\n"
            "into the mouse.\n"
            "Hold Back 0.8s for\n"
            "this menu, shows\n"
            "MENU as a warning\n"
            "first. Back again\n"
            "leaves the app.\n"
            "\n\e#Ice mode\n"
            "Pointer keeps its\n"
            "momentum and coasts\n"
            "to a stop. Ice glide\n"
            "sets how long.\n");
    view_dispatcher_add_view(app->view_dispatcher, BmViewHelp, widget_get_view(app->widget));

    return app;
}

static void bettermouse_free(BetterMouse* app) {
    view_dispatcher_remove_view(app->view_dispatcher, BmViewMenu);
    view_dispatcher_remove_view(app->view_dispatcher, BmViewMouse);
    view_dispatcher_remove_view(app->view_dispatcher, BmViewSettings);
    view_dispatcher_remove_view(app->view_dispatcher, BmViewHelp);

    submenu_free(app->submenu);
    bm_mouse_free(app->bm_mouse);
    variable_item_list_free(app->variable_item_list);
    widget_free(app->widget);

    view_dispatcher_free(app->view_dispatcher);

    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);

    free(app);
}

static bool bettermouse_board_supported(void) {
    if(furi_hal_board_supports_ported_apps()) return true;
    DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
    DialogMessage* message = dialog_message_alloc();
    dialog_message_set_header(message, "Board not supported", 64, 2, AlignCenter, AlignTop);
    dialog_message_set_text(
        message,
        "Runs on T-Embed, Paradoxt,\nThorChimera and Thormini\nonly.",
        64,
        34,
        AlignCenter,
        AlignCenter);
    dialog_message_set_buttons(message, NULL, "OK", NULL);
    dialog_message_show(dialogs, message);
    dialog_message_free(message);
    furi_record_close(RECORD_DIALOGS);
    return false;
}

static void bettermouse_show_usb_busy(void) {
    DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
    DialogMessage* message = dialog_message_alloc();
    dialog_message_set_header(message, "USB is busy", 64, 2, AlignCenter, AlignTop);
    dialog_message_set_text(
        message,
        "Close USB storage or\nqFlipper, then reopen\nBetter Mouse.",
        64,
        34,
        AlignCenter,
        AlignCenter);
    dialog_message_set_buttons(message, NULL, "OK", NULL);
    dialog_message_show(dialogs, message);
    dialog_message_free(message);
    furi_record_close(RECORD_DIALOGS);
}

int32_t bettermouse_app(void* p) {
    UNUSED(p);
    if(!bettermouse_board_supported()) return 0;

    BetterMouse* app = bettermouse_alloc();

    /* Claim the USB port as an exclusive HID lease. Refused (busy) shows a
       message and exits instead of resetting the device. */
    if(!bm_usb_acquire(app)) {
        bettermouse_show_usb_busy();
        bettermouse_free(app);
        return 0;
    }

    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    /* Straight into the mouse, ready to go. Holding Back drops to the menu,
       and Back from the menu leaves the app. */
    bm_switch_to(app, BmViewMouse);
    view_dispatcher_run(app->view_dispatcher);

    bm_settings_save(&app->settings);

    /* Give USB back, retrying a release that fails (a lease whose preempted
       owner is slow to resume). */
    for(uint8_t attempt = 0; attempt < BM_RELEASE_TRIES; attempt++) {
        if(bm_usb_release(app)) break;
        furi_delay_ms(BM_RELEASE_RETRY_MS);
    }
    if(app->usb_lease) {
        FURI_LOG_E(TAG, "USB could not be released; it stays reserved until reboot");
    }

    bettermouse_free(app);
    return 0;
}
