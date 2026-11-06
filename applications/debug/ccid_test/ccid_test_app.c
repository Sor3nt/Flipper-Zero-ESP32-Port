#include <stdint.h>
#include <furi.h>
#include <furi_hal.h>

#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/gui.h>

#include "iso7816/iso7816_handler.h"
#include "iso7816/iso7816_t0_apdu.h"
#include "iso7816/iso7816_atr.h"
#include "iso7816/iso7816_response.h"

#include "ccid_test_app_commands.h"

typedef enum {
    EventTypeInput,
} EventType;

typedef struct {
    Gui* gui;
    ViewPort* view_port;
    FuriMessageQueue* event_queue;
    FuriHalUsbCcidConfig ccid_cfg;
    Iso7816Handler* iso7816_handler;
    bool usb_ready; // got the USB lease (false: USB busy -> show a notice, no CCID)
} CcidTestApp;

typedef struct {
    union {
        InputEvent input;
    };
    EventType type;
} CcidTestAppEvent;

typedef enum {
    CcidTestSubmenuIndexInsertSmartcard,
    CcidTestSubmenuIndexRemoveSmartcard,
    CcidTestSubmenuIndexInsertSmartcardReader
} SubmenuIndex;

static void ccid_test_app_render_callback(Canvas* canvas, void* ctx) {
    const CcidTestApp* app = ctx;
    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    if(app && !app->usb_ready) {
        canvas_draw_str(canvas, 0, 10, "USB is busy");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 0, 26, "Close qFlipper or the");
        canvas_draw_str(canvas, 0, 36, "USB app, then reopen.");
        canvas_draw_str(canvas, 0, 63, "Hold [back] to exit");
        return;
    }
    canvas_draw_str(canvas, 0, 10, "CCID Test App");

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 0, 63, "Hold [back] to exit");
}

static void ccid_test_app_input_callback(InputEvent* input_event, void* ctx) {
    FuriMessageQueue* event_queue = ctx;

    CcidTestAppEvent event;
    event.type = EventTypeInput;
    event.input = *input_event;
    furi_message_queue_put(event_queue, &event, FuriWaitForever);
}

uint32_t ccid_test_exit(void* context) {
    UNUSED(context);
    return VIEW_NONE;
}

CcidTestApp* ccid_test_app_alloc(void) {
    CcidTestApp* app = malloc(sizeof(CcidTestApp));
    app->usb_ready = false; // set once the lease is taken, before the view can render

    //setup CCID USB
    // On linux: set VID PID using: /usr/lib/pcsc/drivers/ifd-ccid.bundle/Contents/Info.plist
    app->ccid_cfg.vid = 0x076B;
    app->ccid_cfg.pid = 0x3A21;

    app->iso7816_handler = iso7816_handler_alloc();
    app->iso7816_handler->iso7816_answer_to_reset = iso7816_answer_to_reset;
    app->iso7816_handler->iso7816_process_command = iso7816_process_command;

    // Gui
    app->gui = furi_record_open(RECORD_GUI);

    //viewport
    app->view_port = view_port_alloc();
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);
    view_port_draw_callback_set(app->view_port, ccid_test_app_render_callback, app);

    //message queue
    app->event_queue = furi_message_queue_alloc(8, sizeof(CcidTestAppEvent));
    view_port_input_callback_set(app->view_port, ccid_test_app_input_callback, app->event_queue);

    return app;
}

void ccid_test_app_free(CcidTestApp* app) {
    furi_assert(app);

    //message queue
    furi_message_queue_free(app->event_queue);

    //view port
    gui_remove_view_port(app->gui, app->view_port);
    view_port_free(app->view_port);

    // Close gui record
    furi_record_close(RECORD_GUI);
    app->gui = NULL;

    iso7816_handler_free(app->iso7816_handler);

    // Free rest
    free(app);
}

// Unique, stable address used as this app's USB lease owner token.
static const char ccid_test_usb_owner;

int32_t ccid_test_app(void* p) {
    UNUSED(p);

    //setup view
    CcidTestApp* app = ccid_test_app_alloc();

    // Take the USB port as a cooperative lease instead of force-unlocking it: a running
    // qFlipper / RPC session is paused and resumed rather than torn away, and when USB is
    // locked (RPC) or held by USB-Storage this app is refused and the screen says so.
    // (Was: furi_hal_usb_unlock() + furi_hal_usb_set_config(), which stole USB from RPC.)
    FuriHalUsbInterface* usb_mode_prev = NULL;
    app->usb_ready = furi_hal_usb_lease_acquire(&ccid_test_usb_owner, &usb_mode_prev);
    if(app->usb_ready &&
       !furi_hal_usb_set_config_for_owner(&ccid_test_usb_owner, &usb_ccid, &app->ccid_cfg)) {
        furi_hal_usb_lease_release(&ccid_test_usb_owner, usb_mode_prev);
        app->usb_ready = false;
    }
    if(app->usb_ready) {
        iso7816_handler_set_usb_ccid_callbacks();
        furi_hal_usb_ccid_insert_smartcard();
    }

    //handle button events
    CcidTestAppEvent event;
    while(1) {
        FuriStatus event_status =
            furi_message_queue_get(app->event_queue, &event, FuriWaitForever);

        if(event_status == FuriStatusOk) {
            if(event.type == EventTypeInput) {
                if(event.input.type == InputTypeLong && event.input.key == InputKeyBack) {
                    break;
                }
            }
        }
        view_port_update(app->view_port);
    }

    //tear down USB (usb_ccid has no TinyUSB backend to release; lease_release restores the
    //previous interface and resumes any paused qFlipper session)
    if(app->usb_ready) {
        iso7816_handler_reset_usb_ccid_callbacks();
        furi_hal_usb_lease_release(&ccid_test_usb_owner, usb_mode_prev);
    }

    //teardown view
    ccid_test_app_free(app);
    return 0;
}
