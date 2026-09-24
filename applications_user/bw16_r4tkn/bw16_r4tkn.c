/* R4TKN v4 UART controller. See README for protocol and hardware limitations. */
#include <furi.h>
#include <gui/gui.h>
#include <input/input.h>
#include <freertos/queue.h>
#include <string.h>
#include <stdlib.h>
#include "bw16_control.h"
#include "bw16_uart.h"

#define TAG "BW16R4TKN"
typedef enum { Wiring, Menu, Waiting, Results, ApActions, Clients, Confirm,
               Operation, Info, Error, Unplug } Screen;
typedef struct {
    FuriMutex* mutex;
    FuriMessageQueue* keys;
    ViewPort* viewport;
    Bw16Uart io;
    Bw16Line line;
    Bw16Control control;
    Screen screen;
    unsigned menu, selected, client, action;
    Bw16Target pending;
    uint32_t last_rx;
    bool input_overflow, dirty, exit_requested, back_latched, waiting_clients;
    char notice[48];
} App;

static bool send_command(void* context, const char* command) {
    App* app = context;
    if(!app->io.installed) return false;
    app->io.stage = "Protected UART TX failed";
    app->io.error = furi_hal_bw16_guard_send(&app->io.guard, command, strlen(command));
    return app->io.error == ESP_OK;
}

static void note(App* app, const char* text) {
    snprintf(app->notice, sizeof(app->notice), "%s", text);
    app->dirty = true;
}

static void sync_state(App* app) {
    Screen previous = app->screen;
    Bw16State state = app->control.state;
    if(state == Bw16Fault && app->screen != Unplug) app->screen = Error;
    else if(state == Bw16Starting || state == Bw16Active || state == Bw16Stopping || state == Bw16Stopped)
        app->screen = Operation;
    else if(app->screen == Waiting && state == Bw16Idle)
        app->screen = app->waiting_clients ? Clients : Results;
    if(previous != app->screen) { app->dirty = true; app->notice[0] = 0; }
}

static void fail(App* app, const char* text) {
    bw16_control_error(&app->control, text, furi_get_tick());
    sync_state(app);
    app->dirty = true;
    FURI_LOG_E(TAG, "%s", text);
}

/* Four content rows plus a footer, within the 128x64 canvas. */
static void draw(Canvas* canvas, void* context) {
    App* app = context;
    Bw16Control* c = &app->control;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    char rows[4][48] = {{0}}, footer[48] = {0};
    switch(app->screen) {
    case Wiring:
        strcpy(rows[0], "NRF stays fitted.");
        strcpy(rows[1], "Unplug BW16/RFID/IR.");
        strcpy(rows[2], "BW TX->43  BW RX->44");
        strcpy(footer, "Hold OK: prepare pins");
        break;
    case Menu: {
        static const char* items[] = {"Scan networks", "Station list", "Deauth ALL scanned", "Firmware / limits"};
        for(unsigned i = 0; i < 4; ++i) snprintf(rows[i], sizeof(rows[i]), "%c %s", i == app->menu ? '>' : ' ', items[i]);
        strcpy(footer, "Connect BW16, then OK");
        break;
    }
    case Waiting:
        strcpy(rows[0], app->waiting_clients ? "Listening for peers..." : "Waiting for AP list...");
        snprintf(rows[1], sizeof(rows[1]), "%u records, %lu / 20s", app->waiting_clients ? c->client_total : c->ap_total,
                 (unsigned long)((furi_get_tick() - c->started) / 1000));
        strcpy(footer, "Back: disconnect / exit");
        break;
    case Results:
        if(!c->ap_count) strcpy(rows[0], "Done: no networks");
        else {
            Bw16Ap* ap = &c->aps[app->selected];
            snprintf(rows[0], sizeof(rows[0]), "%u/%u%s CH%u %ddBm", app->selected + 1,
                     c->ap_count, c->ap_total > c->ap_count ? "+" : "", ap->channel, ap->rssi);
            snprintf(rows[1], sizeof(rows[1]), "%.20s", ap->ssid[0] ? ap->ssid : "<hidden>");
            strcpy(rows[2], ap->mac);
        }
        strcpy(footer, c->indices_valid ? "Up/Down  OK: AP actions" : "OK: indexed AP list");
        break;
    case ApActions:
        snprintf(rows[0], sizeof(rows[0]), "%.20s", c->aps[app->selected].ssid);
        strcpy(rows[1], app->action == 0 ? "> Observe clients" : "  Observe clients");
        strcpy(rows[2], app->action == 1 ? "> Deauth this station" : "  Deauth this station");
        strcpy(footer, "OK: choose  Back: APs");
        break;
    case Clients:
        snprintf(rows[0], sizeof(rows[0]), "Observed peers: %u%s", c->client_count, c->client_total > c->client_count ? "+" : "");
        if(c->client_count) {
            snprintf(rows[1], sizeof(rows[1]), "%u/%u %s", app->client + 1, c->client_count, c->clients[app->client]);
            strcpy(rows[2], "OK: deauth this client");
        } else strcpy(rows[1], "No peers observed (5s)");
        strcpy(footer, "Up/Down  Back: AP menu");
        break;
    case Confirm:
        strcpy(rows[0], "Confirm deauth");
        if(app->pending == Bw16TargetAll) snprintf(rows[1], sizeof(rows[1]), "ALL %u scanned APs", c->ap_count);
        else if(app->pending == Bw16TargetClient) strcpy(rows[1], c->clients[app->client]);
        else snprintf(rows[1], sizeof(rows[1]), "AP %.18s", c->aps[app->selected].ssid);
        strcpy(rows[2], "30s limit; Back cancels");
        strcpy(footer, "Hold OK to start");
        break;
    case Operation:
        strcpy(rows[0], c->state == Bw16Starting ? "Waiting for START..." :
               c->state == Bw16Stopping ? "Waiting for STOP..." :
               c->state == Bw16Stopped ? "Device confirmed stop" : "Device reports active");
        snprintf(rows[1], sizeof(rows[1]), "%s: %lu", c->target == Bw16TargetAll ? "Cycles" : "TX attempts", (unsigned long)c->counter);
        strcpy(rows[2], "Outcome not verified");
        strcpy(footer, c->state == Bw16Stopped ? "OK / Back: menu" : "OK / Back: request STOP");
        break;
    case Info:
        strcpy(rows[0], "FLIPPER_ZERO v4 only");
        strcpy(rows[1], "No beacon/capture API.");
        strcpy(rows[2], "NRF paused during use.");
        strcpy(footer, "Back / OK: menu");
        break;
    case Error:
        if(c->remote_unknown) {
            strcpy(rows[0], "Stop not confirmed");
            strcpy(rows[1], "Power OFF BW16 first!");
            strcpy(rows[2], "BW16 may transmit!");
        } else {
            size_t cut = strlen(c->error);
            if(cut > 21) {
                cut = 21;
                while(cut && c->error[cut] != ' ') --cut;
                if(!cut) cut = 21;
            }
            memcpy(rows[0], c->error, cut);
            const char* rest = c->error + cut;
            while(*rest == ' ') ++rest;
            snprintf(rows[1], sizeof(rows[1]), "%.21s", rest);
            strcpy(rows[2], "Reset BW16 to retry.");
        }
        strcpy(footer, "Back: disconnect / exit");
        break;
    case Unplug:
        strcpy(rows[0], c->remote_unknown ? "Power OFF BW16 first!" : "Disconnect BW16 wires");
        strcpy(rows[1], "Unplug TX and RX.");
        strcpy(rows[2], "Before restoring pins.");
        strcpy(footer, "Done? Hold OK to exit");
        break;
    }
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "BW16 R4TKN v4");
    canvas_set_font(canvas, FontSecondary);
    for(unsigned i = 0; i < 4; ++i) canvas_draw_str(canvas, 2, 22 + i * 10, rows[i]);
    canvas_draw_str(canvas, 2, 63, app->notice[0] ? app->notice : footer);
    furi_mutex_release(app->mutex);
}

static void input(InputEvent* event, void* context) {
    App* app = context;
    if(event->type != InputTypeShort && event->type != InputTypeLong) return;
    if(furi_message_queue_put(app->keys, event, 0) != FuriStatusOk) {
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        app->input_overflow = true;
        if(event->key == InputKeyBack) app->back_latched = true;
        furi_mutex_release(app->mutex);
    }
}

static bool quiet(App* app) {
    if((uint32_t)(furi_get_tick() - app->last_rx) < 500) { note(app, "Wait for UART quiet"); return false; }
    size_t buffered = 0;
    if(uart_get_buffered_data_len(UART_NUM_1, &buffered) != ESP_OK) { fail(app, "UART status failed"); return false; }
    if(buffered || app->line.used || app->line.discard) { note(app, "Wait for UART quiet"); return false; }
    note(app, "");
    return true;
}

static void receive(App* app) {
    if(!app->io.installed) return;
    if(uxQueueMessagesWaiting(app->io.events) >= 64) fail(app, "UART event queue full");
    uart_event_t event;
    for(unsigned i = 0; i < 64 && xQueueReceive(app->io.events, &event, 0) == pdTRUE; ++i)
        if(event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL || event.type == UART_FRAME_ERR ||
           event.type == UART_PARITY_ERR || event.type == UART_BREAK) fail(app, "UART RX loss/framing error");
    uint8_t bytes[512];
    int n = uart_read_bytes(UART_NUM_1, bytes, sizeof(bytes), 0);
    if(n < 0) { fail(app, "UART read failed"); return; }
    if(n) app->last_rx = furi_get_tick();
    for(int i = 0; i < n; ++i) {
        Bw16Frame frame = bw16_frame(&app->line, bytes[i]);
        if(frame == Bw16LineDropped && bw16_control_busy(&app->control)) fail(app, "RX line too long/corrupt");
        else if(frame == Bw16LineReady) {
            bw16_control_line(&app->control, app->line.text, furi_get_tick());
            sync_state(app);
        }
    }
    if(n) app->dirty = true;
}

static void back(App* app) {
    switch(app->screen) {
    case Operation:
        if(app->control.state == Bw16Stopped) { app->control.state = Bw16Idle; app->screen = Menu; }
        else bw16_control_stop(&app->control, furi_get_tick());
        break;
    case Confirm:
        app->screen = app->pending == Bw16TargetAll ? Menu : app->pending == Bw16TargetClient ? Clients : ApActions;
        break;
    case Results: case Info: app->screen = Menu; break;
    case ApActions: app->screen = Results; break;
    case Clients: app->screen = ApActions; break;
    default:
        if(app->io.leased) app->screen = Unplug;
        else app->exit_requested = true;
        break;
    }
    note(app, "");
}

static void key(App* app, InputEvent event) {
    Bw16Control* c = &app->control;
    if(event.key == InputKeyBack && event.type == InputTypeShort) { back(app); return; }
    if(event.type == InputTypeLong && event.key == InputKeyOk) {
        if(app->screen == Wiring) {
            if(bw16_uart_open(&app->io)) { app->screen = Menu; app->last_rx = furi_get_tick(); }
            else { fail(app, app->io.stage); FURI_LOG_E(TAG, "Setup error 0x%lx", (unsigned long)app->io.error); }
        } else if(app->screen == Unplug) {
            if(bw16_uart_close(&app->io) == ESP_OK) app->exit_requested = true;
            else fail(app, "Resource cleanup: retry");
        } else if(app->screen == Confirm && quiet(app)) {
            if(bw16_control_start(c, app->pending, app->selected, app->client, furi_get_tick())) app->screen = Operation;
            else if(c->state != Bw16Fault) note(app, "List incomplete; rescan");
            sync_state(app);
        }
    } else if(event.type == InputTypeShort) {
        bool up = event.key == InputKeyUp, down = event.key == InputKeyDown, ok = event.key == InputKeyOk;
        switch(app->screen) {
        case Menu:
            if(up && app->menu) --app->menu;
            if(down && app->menu < 3) ++app->menu;
            if(ok) {
                if(app->menu < 2 && quiet(app)) {
                    app->selected = app->client = 0; app->waiting_clients = false;
                    if(bw16_control_scan(c, app->menu == 1, furi_get_tick())) app->screen = Waiting;
                } else if(app->menu == 2) {
                    if(!c->indices_valid || !c->ap_count || c->ap_count != c->ap_total) note(app, "Use Station list first");
                    else { app->pending = Bw16TargetAll; app->screen = Confirm; note(app, ""); }
                } else if(app->menu == 3) app->screen = Info;
                sync_state(app);
            }
            break;
        case Results:
            if(up && app->selected) --app->selected;
            if(down && app->selected + 1 < c->ap_count) ++app->selected;
            if(ok && c->ap_count) {
                if(c->indices_valid) { app->action = 0; app->screen = ApActions; }
                else if(quiet(app)) {
                    app->selected=0;app->waiting_clients=false;
                    if(bw16_control_scan(c,true,furi_get_tick())) app->screen=Waiting;
                    sync_state(app);
                }
            }
            break;
        case ApActions:
            if(up || down) app->action = 1 - app->action;
            if(ok && app->action == 0 && quiet(app)) {
                app->client = 0; app->waiting_clients = true;
                if(bw16_control_clients(c, app->selected, furi_get_tick())) app->screen = Waiting;
                sync_state(app);
            } else if(ok && app->action == 1) { app->pending = Bw16TargetStation; app->screen = Confirm; }
            break;
        case Clients:
            if(up && app->client) --app->client;
            if(down && app->client + 1 < c->client_count) ++app->client;
            if(ok && c->client_count) { app->pending = Bw16TargetClient; app->screen = Confirm; }
            break;
        case Operation: if(ok) back(app); break;
        case Info: if(ok) app->screen = Menu; break;
        default: break;
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
    bw16_control_init(&app->control, send_command, app);
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
        if(app->input_overflow) { app->input_overflow = false; fail(app, "Input queue full"); }
        if(app->back_latched) { app->back_latched = false; back(app); have_key = false; }
        if(have_key) key(app, event);
        if(app->exit_requested) { furi_mutex_release(app->mutex); break; }
        if(app->screen != Unplug && app->screen != Error) receive(app);
        uint32_t now = furi_get_tick();
        if(app->screen != Unplug) { bw16_control_tick(&app->control, now); sync_state(app); }
        bool update = (app->dirty || bw16_control_busy(&app->control)) && (uint32_t)(now - redraw) >= 100;
        if(update) { app->dirty = false; redraw = now; }
        furi_mutex_release(app->mutex);
        if(update) view_port_update(app->viewport);
    }
    result = 0;
cleanup:
    if(attached) gui_remove_view_port(gui, app->viewport);
    if(app->viewport) view_port_free(app->viewport);
    if(gui) furi_record_close(RECORD_GUI);
    if(app->keys) furi_message_queue_free(app->keys);
    if(app->mutex) furi_mutex_free(app->mutex);
    if(result) FURI_LOG_E(TAG, "GUI allocation failed; UART untouched");
    free(app);
    return result;
}
