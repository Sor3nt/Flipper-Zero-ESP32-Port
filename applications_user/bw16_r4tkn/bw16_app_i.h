#pragma once

#include "bw16_app.h"
#include "scenes/bw16_scene.h"
#include "bw16_custom_event.h"
#include "bw16_uart.h"

#include <furi.h>
#include <furi_hal.h>

#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_box.h>
#include <gui/modules/widget.h>

#define BW16_TEXT_BOX_STORE_SIZE (4096)
#define BW16_TX_CMD_SIZE         (48)

#define BW16_MAX_AP      (48)
#define BW16_SSID_LEN    (33) /* 32 + NUL */
#define BW16_MAX_CLIENT  (48)
#define BW16_MAC_STR_LEN (24) /* "aa:bb:cc:dd:ee:ff" (+ optional extra field) + NUL */

typedef enum {
    Bw16CaptureNone = 0,
    Bw16CaptureAp,
    Bw16CaptureClient,
} Bw16CaptureMode;

typedef struct {
    char ssid[BW16_SSID_LEN];
    int channel;
    int rssi;
} Bw16ApEntry;

typedef enum {
    Bw16AppViewSubmenu = 0,
    Bw16AppViewTextBox,
    Bw16AppViewWidget,
} Bw16AppView;

struct Bw16App {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    SceneManager* scene_manager;

    Submenu* submenu;
    TextBox* text_box;
    Widget* widget;

    Bw16Uart* uart;

    /* The UART worker thread and the GUI thread share the state below; `mutex` guards
     * ALL of it. ESP32-S3 is dual-core, so a bare flag is not enough for cross-core
     * visibility — the mutex provides the ordering barrier. The worker NEVER calls into
     * the GUI (view dispatcher); it only mutates state + sets flags, and the scenes poll
     * those flags from their tick handler. This removes any chance of the worker blocking
     * on the view-dispatcher queue during shutdown (which would hang furi_thread_join). */
    FuriMutex* mutex;

    /* Console (run) state */
    FuriString* text_box_store; /* producer buffer (worker appends under mutex) */
    /* GUI-only snapshots. Double-buffered: a tick snapshots `store` into the IDLE buffer and
     * only then points the TextBox at it, so the GUI draw thread never reads the buffer that
     * is being overwritten (both are reserved up front, so the pointer never reallocates). */
    FuriString* text_box_view[2];
    uint8_t text_box_view_index;
    size_t text_box_store_strlen;
    bool console_dirty; /* worker set: store changed since last snapshot */
    char tx_command[BW16_TX_CMD_SIZE]; /* command sent on console enter */
    const char* console_title; /* shown as first console line (points at a literal) */
    bool console_continuous; /* attack mode: re-send on exit to stop */

    /* Line assembler + list parsing (touched by the UART worker thread, under mutex) */
    FuriString* rx_line;
    Bw16CaptureMode capture_mode;

    /* Access-point list (produced by LIST_STATION) */
    Bw16ApEntry aps[BW16_MAX_AP];
    size_t ap_count;
    bool ap_list_done; /* worker set: LIST_STATION_DONE seen */

    /* Client list (produced by LIST_CLIENTS <idx>) */
    char clients[BW16_MAX_CLIENT][BW16_MAC_STR_LEN];
    size_t client_count;
    bool client_list_done; /* worker set: LIST_CLIENTS_DONE seen */

    /* Selection carried between scenes (GUI thread only) */
    int selected_station;
    int selected_client;
};

/* bw16_app.c */
void bw16_app_set_command(Bw16App* app, const char* title, const char* command, bool continuous);

/* bw16_rx.c (line handling, shared by scenes) */
void bw16_rx_handle_data(uint8_t* buf, size_t len, void* context);
