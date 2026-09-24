/* Scan-only R4TKN companion. See README for wiring and protocol evidence. */
#include <furi.h>
#include <gui/gui.h>
#include <input/input.h>
#include <freertos/queue.h>
#include <string.h>
#include <stdlib.h>
#include "bw16_protocol.h"
#include "bw16_uart.h"

#define TAG "BW16R4TKN"
typedef enum { ScreenWiring, ScreenReady, ScreenScanning, ScreenResults, ScreenError,
               ScreenUnplug } Screen;
typedef struct {
    FuriMutex* mutex;
    FuriMessageQueue* keys;
    ViewPort* viewport;
    Bw16Uart io;
    Bw16Line line;
    Bw16Ap aps[BW16_MAX_APS];
    unsigned count, selected, omitted;
    uint32_t started, last_rx;
    bool input_overflow, dirty, exit_requested;
    Screen screen;
    char error[48];
} App;

static void fail(App* app, const char* text) {
    snprintf(app->error, sizeof(app->error), "%s", text);
    app->screen = ScreenError;
    app->dirty = true;
    FURI_LOG_E(TAG, "%s", text);
}

static void draw(Canvas* canvas, void* context) {
    App* app = context;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "BW16 Scan");
    canvas_set_font(canvas, FontSecondary);
    char text[48];
    switch(app->screen) {
    case ScreenWiring:
        canvas_draw_str(canvas, 2, 23, "NRF stays fitted (v2).");
        canvas_draw_str(canvas, 2, 34, "Unplug BW16/RFID/IR.");
        canvas_draw_str(canvas, 2, 45, "BW TX->43  BW RX->44");
        canvas_draw_str(canvas, 2, 62, "Hold OK: prepare pins");
        break;
    case ScreenReady:
        canvas_draw_str(canvas, 2, 24, "Now connect BW16.");
        canvas_draw_str(canvas, 2, 36, "BW TX->43  BW RX->44");
        canvas_draw_str(canvas, 2, 62, "OK: scan  Back: unplug");
        break;
    case ScreenScanning:
        snprintf(text, sizeof(text), "Waiting: %u APs", app->count);
        canvas_draw_str(canvas, 2, 28, text);
        snprintf(text, sizeof(text), "%lu / 20 seconds",
                 (unsigned long)((furi_get_tick() - app->started) / 1000));
        canvas_draw_str(canvas, 2, 41, text);
        canvas_draw_str(canvas, 2, 62, "Back: leave scan");
        break;
    case ScreenResults:
        if(!app->count) {
            canvas_draw_str(canvas, 2, 28, "Done: 0 valid records");
        } else {
            Bw16Ap* ap = &app->aps[app->selected];
            snprintf(text, sizeof(text), "%u/%u%s CH%u %ddBm", app->selected + 1,
                     app->count, app->omitted ? "+" : "", ap->channel, ap->rssi);
            canvas_draw_str(canvas, 2, 23, text);
            snprintf(text, sizeof(text), "%.20s", ap->ssid[0] ? ap->ssid : "<hidden>");
            canvas_draw_str(canvas, 2, 35, text);
            canvas_draw_str(canvas, 2, 46, ap->mac);
        }
        canvas_draw_str(canvas, 2, 62, "Up/Down  OK: scan again");
        break;
    case ScreenError:
        snprintf(text, sizeof(text), "%.21s", app->error);
        canvas_draw_str(canvas, 2, 25, text);
        canvas_draw_str(canvas, 2, 37, strlen(app->error) > 21 ? app->error + 21 : "");
        canvas_draw_str(canvas, 2, 49, "No complete scan.");
        canvas_draw_str(canvas, 2, 62, "Back: unplug / exit");
        break;
    case ScreenUnplug:
        canvas_draw_str(canvas, 2, 23, "Disconnect BW16 wires");
        canvas_draw_str(canvas, 2, 34, "before restoring pins.");
        canvas_draw_str(canvas, 2, 45, "Scan may finish remotely.");
        canvas_draw_str(canvas, 2, 62, "Unplugged? Hold OK");
        break;
    }
    furi_mutex_release(app->mutex);
}

static void input(InputEvent* event, void* context) {
    App* app = context;
    if(event->type != InputTypeShort && event->type != InputTypeLong) return;
    /* Never wait for queue space on the GUI thread. Latch an overflow/Back. */
    if(furi_message_queue_put(app->keys, event, 0) != FuriStatusOk) {
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        app->input_overflow = true;
        if(event->key == InputKeyBack) app->exit_requested = true;
        furi_mutex_release(app->mutex);
    }
}

static void handle_line(App* app) {
    if(app->screen != ScreenScanning) return;
    Bw16Ap ap;
    switch(bw16_parse(app->line.text, &ap)) {
    case Bw16Record:
        for(unsigned i = 0; i < app->count; ++i) {
            if((ap.mac[0] && strcmp(ap.mac, app->aps[i].mac) == 0) ||
               (!ap.mac[0] && ap.channel == app->aps[i].channel &&
                strcmp(ap.ssid, app->aps[i].ssid) == 0)) return;
        }
        if(app->count < BW16_MAX_APS) app->aps[app->count++] = ap;
        else ++app->omitted;
        app->dirty = true;
        break;
    case Bw16Done:
        app->screen = ScreenResults;
        app->dirty = true;
        break;
    case Bw16Empty: break; /* Keep waiting for SCAN_DONE. */
    case Bw16Malformed: fail(app, "Malformed scan response"); break;
    case Bw16Ignore: break;
    }
}

static void receive(App* app) {
    if(!app->io.installed) return;
    /* A full queue or any loss/framing event invalidates the transaction. */
    if(uxQueueMessagesWaiting(app->io.events) >= 64) fail(app, "UART event queue full");
    uart_event_t event;
    for(unsigned i = 0; i < 64 && xQueueReceive(app->io.events, &event, 0) == pdTRUE; ++i) {
        if(event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL ||
           event.type == UART_FRAME_ERR || event.type == UART_PARITY_ERR ||
           event.type == UART_BREAK) fail(app, "UART RX loss/framing error");
    }
    uint8_t bytes[512];
    int n = uart_read_bytes(UART_NUM_1, bytes, sizeof(bytes), 0);
    if(n < 0) { fail(app, "UART read failed"); return; }
    if(n) app->last_rx = furi_get_tick();
    for(int i = 0; i < n; ++i) {
        Bw16Frame frame = bw16_frame(&app->line, bytes[i]);
        if(frame == Bw16LineDropped && app->screen == ScreenScanning)
            fail(app, "RX line too long/corrupt");
        else if(frame == Bw16LineReady) handle_line(app);
    }
}

static void scan(App* app) {
    /* No request IDs: after timeout/error, reset BW16 and reopen the app. */
    if((uint32_t)(furi_get_tick() - app->last_rx) < 500) return;
    size_t buffered = 0;
    if(uart_get_buffered_data_len(UART_NUM_1, &buffered) != ESP_OK || buffered) {
        fail(app, "UART not quiet; reopen");
        return;
    }
    memset(&app->line, 0, sizeof(app->line));
    app->count = app->selected = app->omitted = 0;
    app->started = furi_get_tick();
    app->screen = ScreenScanning;
    if(!bw16_uart_scan(&app->io)) fail(app, app->io.stage);
    app->dirty = true;
}

static void key(App* app, InputEvent event) {
    if(event.key == InputKeyBack && event.type == InputTypeShort) {
        if(app->io.leased) app->screen = ScreenUnplug;
        else app->exit_requested = true;
    } else if(app->screen == ScreenWiring && event.key == InputKeyOk && event.type == InputTypeLong) {
        if(bw16_uart_open(&app->io)) {
            app->screen = ScreenReady;
            app->last_rx = furi_get_tick();
        } else {
            fail(app, app->io.stage);
            FURI_LOG_E(TAG, "UART setup error 0x%lx", (unsigned long)app->io.error);
        }
    } else if(app->screen == ScreenUnplug && event.key == InputKeyOk && event.type == InputTypeLong) {
        if(bw16_uart_close(&app->io) == ESP_OK) app->exit_requested = true;
        else fail(app, "Resource cleanup: retry");
    } else if(event.type == InputTypeShort) {
        if((app->screen == ScreenReady || app->screen == ScreenResults) && event.key == InputKeyOk)
            scan(app);
        else if(app->screen == ScreenResults && app->count) {
            if(event.key == InputKeyUp && app->selected) --app->selected;
            if(event.key == InputKeyDown && app->selected + 1 < app->count) ++app->selected;
        }
    }
    app->dirty = true;
}

int32_t bw16_r4tkn_app(void* context) {
    UNUSED(context);
    App* app = calloc(1, sizeof(*app));
    Gui* gui = NULL;
    bool attached = false;
    int32_t result = -1;
    if(!app) { FURI_LOG_E(TAG, "No memory for app"); return result; }
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->keys = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->viewport = view_port_alloc();
    if(!app->mutex || !app->keys || !app->viewport) goto cleanup;
    view_port_draw_callback_set(app->viewport, draw, app);
    view_port_input_callback_set(app->viewport, input, app);
    gui = furi_record_open(RECORD_GUI);
    if(!gui) goto cleanup;
    gui_add_view_port(gui, app->viewport, GuiLayerFullscreen);
    attached = true;
    uint32_t redraw = 0;
    while(true) {
        InputEvent event;
        bool have_key = furi_message_queue_get(app->keys, &event, 20) == FuriStatusOk;
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        if(app->input_overflow) {
            app->input_overflow = false;
            fail(app, "Input queue full; Back exits");
        }
        bool latched_exit = app->exit_requested;
        if(latched_exit && app->io.leased) {
            app->exit_requested = false;
            app->screen = ScreenUnplug;
            app->dirty = true;
        }
        if(have_key && !latched_exit) key(app, event);
        if(app->exit_requested) {
            furi_mutex_release(app->mutex);
            break;
        }
        /* Unplug screen remains stable when disconnecting causes UART errors. */
        if(app->screen != ScreenUnplug && app->screen != ScreenError) receive(app);
        uint32_t now = furi_get_tick();
        if(app->screen == ScreenScanning && bw16_scan_expired(now, app->started))
            fail(app, "Timeout: check R4TKN/wires");
        bool update = (app->dirty || app->screen == ScreenScanning) && (uint32_t)(now - redraw) >= 100;
        if(update) { app->dirty = false; redraw = now; }
        furi_mutex_release(app->mutex);
        if(update) view_port_update(app->viewport);
    }
    result = 0;
cleanup:
    /* GUI removal waits for in-flight callbacks. No model mutex held here. */
    if(attached) gui_remove_view_port(gui, app->viewport);
    if(app->viewport) view_port_free(app->viewport);
    if(gui) furi_record_close(RECORD_GUI);
    if(app->keys) furi_message_queue_free(app->keys);
    if(app->mutex) furi_mutex_free(app->mutex);
    if(result) FURI_LOG_E(TAG, "GUI allocation failed; UART untouched");
    free(app);
    return result;
}
