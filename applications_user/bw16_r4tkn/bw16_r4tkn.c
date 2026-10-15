/* R4TKN v4 UART controller. See README for protocol and hardware limitations. */
#include <furi.h>
#include <gui/gui.h>
#include <input/input.h>
#include <freertos/queue.h>
#include <esp_timer.h>
#include <furi_hal_rtc.h>
#include <furi_hal_random.h>
#include <string.h>
#include <stdlib.h>
#include "bw16_control.h"
#include "bw16_uart.h"
#include "bw16_selection.h"
#include "bw16_batch.h"
#include "bw16_capture.h"

#define TAG "BW16R4TKN"
typedef enum { Connect, Menu, Waiting, Results, ApActions, Clients, Confirm,
               Operation, Info, Error, Unplug, Diagnostics, Synchronizing, Groups, PairNames, CaptureReport } Screen;
typedef enum { MenuScan, MenuSelect, MenuPair, MenuSelected, MenuStations, MenuObserve,
               MenuStation, MenuClient, MenuAll, MenuCapture, MenuCaptureDeauth, MenuDiagnostics, MenuInfo, MenuCount } MenuItem;
typedef struct {
    FuriMutex* mutex;
    FuriMessageQueue* keys;
    ViewPort* viewport;
    Bw16Uart io;
    Bw16Line line;
    Bw16Control control;
    Bw16Selection selection;
    Bw16Batch batch;
    Bw16Capture capture;
    bool capture_details, capture_deauth_pending;
    Screen screen;
    unsigned menu, selected, client, action, intent;
    unsigned group, review, pair_first;
    bool selection_ready, grouped_flow, index_pending, confirm_batch;
    uint32_t index_started;
    Bw16Target pending;
    uint32_t last_rx, sync_started;
    bool caps_queried;
    bool input_overflow, dirty, exit_requested, back_latched, waiting_clients, setup_failed;
    char notice[48];
    Screen diagnostics_return;
    unsigned diagnostics_page, rx_first_count;
    uint32_t rx_bytes, rx_lines, rx_dropped, tx_started, elapsed_ms;
    bool transaction_running;
    uint8_t rx_first[8];
    char last_command[BW16_COMMAND_SIZE], last_line[BW16_LINE_SIZE];
} App;

/* Protocol deadlines are milliseconds regardless of the firmware RTOS tick rate. */
static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool send_command(void* context, const char* command) {
    App* app = context;
    if(!app->io.installed) return false;
    app->rx_bytes = app->rx_lines = app->rx_dropped = app->rx_first_count = 0;
    app->last_line[0] = 0;
    snprintf(app->last_command, sizeof(app->last_command), "%s", command);
    size_t length = strlen(app->last_command);
    if(length && app->last_command[length - 1] == '\n') app->last_command[length - 1] = 0;
    app->tx_started = now_ms();
    app->elapsed_ms = 0;
    app->transaction_running = true;
    app->io.error = bw16_uart_send(&app->io, command, strlen(command));
    return app->io.error == ESP_OK;
}

static void note(App* app, const char* text) {
    snprintf(app->notice, sizeof(app->notice), "%s", text);
    app->dirty = true;
}

static bool selection_valid(const App* app) {
    return app->selection_ready && app->control.aps_valid && app->control.indices_valid &&
           app->selection.ap_count == app->control.ap_count;
}

static bool loading(const App* app) {
    Bw16State state = app->control.state;
    return app->screen == Synchronizing || app->screen == Waiting ||
           state == Bw16Starting || state == Bw16Stopping ||
           (app->batch.running && state == Bw16Stopped);
}

static void sync_state(App* app) {
    if(app->capture.file && app->control.target == Bw16TargetCapture &&
       (app->control.state == Bw16Stopped || app->control.state == Bw16Fault)) {
        if(app->control.state == Bw16Fault) {
            app->capture.failed = true;
            snprintf(app->capture.error, sizeof(app->capture.error), "%s", app->control.error);
        }
        if(!bw16_capture_close(&app->capture) && app->control.state != Bw16Fault)
            bw16_control_error(&app->control, app->capture.error, now_ms());
    }
    Screen previous = app->screen;
    Bw16State state = app->control.state;
    if(state == Bw16Fault && app->screen != Unplug && app->screen != Diagnostics) app->screen = Error;
    else if(state == Bw16Starting || state == Bw16Active || state == Bw16Stopping || state == Bw16Stopped)
        app->screen = Operation;
    else if(app->screen == Waiting && state == Bw16Idle) {
        if(app->grouped_flow) {
            if(app->control.indices_valid) {
                bw16_selection_build(&app->selection, app->control.aps, app->control.ap_count);
                app->selection_ready = true;
                app->grouped_flow = app->index_pending = false;
                app->group = app->review = 0;
                app->pair_first = BW16_MAX_APS;
                app->screen = app->intent == MenuPair ? PairNames : Groups;
            } else if(!app->index_pending) {
                app->index_pending = true;
                app->index_started = now_ms();
            }
        } else {
            app->screen = app->waiting_clients ? Clients : Results;
            if(!app->waiting_clients && app->intent == MenuAll && app->control.indices_valid &&
               app->control.ap_count && app->control.ap_count == app->control.ap_total) {
                app->pending = Bw16TargetAll;
                app->confirm_batch = false;
                app->screen = Confirm;
            }
        }
    }
    if(previous != app->screen) { app->dirty = true; app->notice[0] = 0; }
}

static void fail(App* app, const char* text) {
    bw16_control_error(&app->control, text, now_ms());
    sync_state(app);
    app->dirty = true;
    FURI_LOG_E(TAG, "%s", text);
}

/* The automatic transitions use the same framing/buffer checks as manual commands. */
static bool uart_settled(App* app, uint32_t wait_ms) {
    if(!app->io.installed || (uint32_t)(now_ms() - app->last_rx) < wait_ms ||
       app->line.used || app->line.discard) return false;
    size_t buffered = 0;
    if(uart_get_buffered_data_len(BW16_UART, &buffered) != ESP_OK) {
        fail(app, "UART status failed");
        return false;
    }
    return buffered == 0;
}

static void poll_control(App* app) {
    uint32_t now = now_ms();
    bw16_control_tick(&app->control, now);
    if(app->screen == Synchronizing) {
        if(!app->caps_queried && uart_settled(app, 500)) {
            /* Old v4 firmware may ignore this read-only command. */
            app->caps_queried = true;
            if(!bw16_control_capabilities(&app->control, now)) fail(app, "Cannot query BW16 version");
            app->dirty = true;
        } else if(app->caps_queried && app->control.state == Bw16Idle) {
            app->screen = Menu;
            app->dirty = true;
        } else if(!app->caps_queried && (uint32_t)(now - app->sync_started) >= 3000) {
            fail(app, "UART did not settle");
        }
    }
    if(app->batch.running) {
        bool was_running = app->batch.running;
        unsigned previous_position = app->batch.position;
        bw16_batch_tick(&app->batch, &app->control, now, false);
        if(app->batch.running != was_running || app->batch.position != previous_position)
            app->dirty = true;
    }
    if(app->screen == Waiting && app->index_pending && app->control.state == Bw16Idle) {
        if(uart_settled(app, 500)) {
            /* SCAN order is not a command index: fetch the cached indexed list. */
            app->index_pending = false;
            if(!bw16_control_scan(&app->control, true, now)) fail(app, "Cannot read indexed list");
        } else if((uint32_t)(now - app->index_started) >= 3000) {
            fail(app, "UART did not settle");
        }
    }
    if(app->transaction_running) {
        app->elapsed_ms = now - app->tx_started;
        if(!bw16_control_busy(&app->control)) app->transaction_running = false;
    }
    sync_state(app);
}

static const char* bands_label(uint8_t bands) {
    return bands == (BW16_BAND_24 | BW16_BAND_5) ? "2.4+5 GHz" :
           bands == BW16_BAND_5 ? "5 GHz" : "2.4 GHz";
}

static const char* ap_name(const Bw16Ap* ap) {
    return ap->ssid[0] ? ap->ssid : "<hidden>";
}

/* Find another explicitly paired name for the compact group summary. */
static const char* paired_name(const App* app, unsigned group) {
    const Bw16Selection* selection = &app->selection;
    unsigned first = selection->groups[group].first;
    for(unsigned i = 0; i < selection->ap_count; ++i)
        if(selection->group_for_ap[i] == group &&
           strcmp(app->control.aps[first].raw_ssid, app->control.aps[i].raw_ssid))
            return ap_name(&app->control.aps[i]);
    return NULL;
}

/* Display strings are ASCII; clip by real glyph width, not character count. */
static void draw_row(Canvas* canvas, unsigned y, char* text) {
    size_t length = strlen(text);
    while(length && canvas_string_width(canvas, text) > 126)
        text[--length] = 0;
    canvas_draw_str(canvas, 2, y, text);
}

/* Four content rows plus a footer, within the 128x64 canvas. */
static void draw(Canvas* canvas, void* context) {
    App* app = context;
    Bw16Control* c = &app->control;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    char rows[4][48] = {{0}}, footer[48] = {0};
    switch(app->screen) {
    case Connect:
        strcpy(rows[0], "Connect powered BW16");
        strcpy(rows[1], "TX43 -> PB2 / RX1");
        strcpy(rows[2], "RX44 <- PA7 / TX0");
        strcpy(rows[3], "3V3 + GND; BW16 idle");
        strcpy(footer, "OK: connect  Back: exit");
        break;
    case Synchronizing:
        strcpy(rows[0], app->caps_queried ? "Checking BW16 support" : "Starting UART session");
        strcpy(rows[1], "Keep BW16 powered.");
        strcpy(rows[2], "Please wait...");
        strcpy(footer, "Back: cancel / exit");
        break;
    case Menu: {
        static const char* items[MenuCount] = {"Scan networks", "Select networks", "Pair Wi-Fi names",
            "Deauth selected", "Station list", "Observe clients", "Deauth station", "Deauth client",
            "Deauth ALL scanned", "Capture handshake", "Capture + deauth", "UART details", "Firmware / wiring"};
        unsigned first = app->menu > 3 ? app->menu - 3 : 0;
        for(unsigned i = 0; i < 4; ++i) snprintf(rows[i], sizeof(rows[i]), "%c %s", first + i == app->menu ? '>' : ' ', items[first + i]);
        strcpy(footer, "OK: choose  Back: exit");
        break;
    }
    case Waiting:
        strcpy(rows[0], app->waiting_clients ? "Listening for peers..." :
               app->grouped_flow && (app->index_pending || c->state == Bw16Stations) ?
               "Indexing networks..." : "Waiting for AP list...");
        snprintf(rows[1], sizeof(rows[1]), "%u records, %lu / 20s", app->waiting_clients ? c->client_total : c->ap_total,
                 (unsigned long)((now_ms() - c->started) / 1000));
        snprintf(rows[2], sizeof(rows[2]), "RX: %lu bytes", (unsigned long)app->rx_bytes);
        snprintf(rows[3], sizeof(rows[3]), "%.21s", app->last_line);
        strcpy(footer, "Back: cancel / exit");
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
        strcpy(footer, (app->intent == MenuCapture || app->intent == MenuCaptureDeauth) ? "Up/Down  OK: review" :
               c->indices_valid ? "Up/Down  OK: AP actions" : "OK: indexed AP list");
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
    case Groups:
        if(!app->selection.group_count) strcpy(rows[0], "Done: no networks");
        else {
            const Bw16Group* group = &app->selection.groups[app->group];
            unsigned marked = bw16_selection_group_selected(&app->selection, app->group);
            snprintf(rows[0], sizeof(rows[0]), "[%c] %.17s", marked == group->count ? 'x' : marked ? '-' : ' ',
                     ap_name(&c->aps[group->first]));
            const char* second = paired_name(app, app->group);
            if(second) snprintf(rows[1], sizeof(rows[1]), "+ %.19s", second);
            else snprintf(rows[1], sizeof(rows[1]), "Group %u/%u", app->group + 1, app->selection.group_count);
            snprintf(rows[2], sizeof(rows[2]), "%s %u radios", bands_label(group->bands), group->count);
            snprintf(rows[3], sizeof(rows[3]), "Selected: %u radios", bw16_selection_count(&app->selection));
        }
        strcpy(footer, "OK: mark  Hold: review");
        break;
    case PairNames:
        if(!app->selection.group_count) strcpy(rows[0], "Scan found no names");
        else {
            const Bw16Group* group = &app->selection.groups[app->group];
            strcpy(rows[0], app->pair_first == BW16_MAX_APS ? "Pair names: pick first" : "Pair names: pick next");
            snprintf(rows[1], sizeof(rows[1]), "%.21s", ap_name(&c->aps[group->first]));
            snprintf(rows[2], sizeof(rows[2]), "%s %u radios", bands_label(group->bands), group->count);
            if(app->pair_first == BW16_MAX_APS) strcpy(rows[3], "Links reset on scan");
            else snprintf(rows[3], sizeof(rows[3]), "With: %.15s",
                          ap_name(&c->aps[app->selection.groups[app->pair_first].first]));
        }
        strcpy(footer, "OK: pair  Back: menu");
        break;
    case Confirm:
        if(app->pending == Bw16TargetCapture && !app->confirm_batch) {
            if(app->capture_deauth_pending) strcpy(rows[0], "Capture + deauth");
            else snprintf(rows[0], sizeof(rows[0]), "Passive %.13s", ap_name(&c->aps[app->selected]));
            snprintf(rows[1], sizeof(rows[1]), "%s", c->aps[app->selected].mac);
            snprintf(rows[2], sizeof(rows[2]), "CH%u / 120s / one AP", c->aps[app->selected].channel);
            strcpy(rows[3], app->capture_deauth_pending ? "One burst (3 frames)" : "Reconnect own client");
            strcpy(footer, app->capture_deauth_pending ? "Hold OK: active capture" : "Hold OK: capture");
        } else if(app->confirm_batch) {
            unsigned indices[BW16_MAX_APS];
            unsigned count = bw16_selection_indices(&app->selection, indices, BW16_MAX_APS);
            if(selection_valid(app) && count && app->review < count) {
                const Bw16Ap* ap = &c->aps[indices[app->review]];
                snprintf(rows[0], sizeof(rows[0]), "Selected radio %u/%u", app->review + 1, count);
                snprintf(rows[1], sizeof(rows[1]), "%.21s", ap_name(ap));
                snprintf(rows[2], sizeof(rows[2]), "%s CH%u", ap->channel <= 14 ? "2.4 GHz" : "5 GHz", ap->channel);
                strcpy(rows[3], ap->mac);
                strcpy(footer, count > 1 && !c->native_selected ?
                       "BW16 update needed" : "Hold OK: start 30s");
            } else {
                strcpy(rows[0], "Select networks first");
                strcpy(footer, "Back: selection");
            }
        } else {
            strcpy(rows[0], "Confirm deauth");
            if(app->pending == Bw16TargetAll) snprintf(rows[1], sizeof(rows[1]), "ALL %u scanned APs", c->ap_count);
            else if(app->pending == Bw16TargetClient) strcpy(rows[1], c->clients[app->client]);
            else snprintf(rows[1], sizeof(rows[1]), "AP %.18s", c->aps[app->selected].ssid);
            strcpy(rows[2], "30s limit; Back cancels");
            strcpy(footer, "Hold OK to start");
        }
        break;
    case CaptureReport:
    case Operation:
        if(app->screen == CaptureReport || c->target == Bw16TargetCapture) {
            const Bw16Capture* cap = &app->capture;
            const char* name = strrchr(cap->path, '/');
            name = name ? name + 1 : "No file created";
            uint32_t lost = cap->gaps > cap->dropped ? cap->gaps : cap->dropped;
            if(app->screen != CaptureReport)
                lost = c->capture_dropped > UINT32_MAX - lost ? UINT32_MAX : lost + c->capture_dropped;
            if(app->capture_details) {
                strcpy(rows[0], "/ext/apps_data/bw16/");
                strcpy(rows[1], "captures/");
                snprintf(rows[2], sizeof(rows[2]), "%s", name);
                if(c->capture_with_deauth) snprintf(rows[3], sizeof(rows[3]), "Drops: %lu", (unsigned long)lost);
                else strcpy(rows[3], "Beacons + EAPOL only");
            } else {
                strcpy(rows[0], cap->failed ? "Capture failed/partial" :
                       c->state == Bw16Starting ? "Waiting for START..." :
                       c->state == Bw16Stopping ? "Stopping; drain RX..." :
                       c->state == Bw16Stopped ? "Saved PCAP" :
                       c->capture_with_deauth ? "Capture + deauth /120s" : "Passive capture / 120s");
                snprintf(rows[1], sizeof(rows[1]), "EAPOL: %u Pkts: %u", cap->eapol, cap->packets);
                if(app->screen == CaptureReport) {
                    strcpy(rows[2], "Capture not started");
                    snprintf(rows[3], sizeof(rows[3]), "%s", name);
                } else if(c->capture_with_deauth) {
                    if(c->capture_deauth_reported)
                        snprintf(rows[2], sizeof(rows[2]), "TX attempts: %lu", (unsigned long)c->capture_tx_attempts);
                    else strcpy(rows[2], c->state == Bw16Stopped ? "TX attempts: unknown" : "TX attempts: pending");
                    if(c->state == Bw16Active || c->state == Bw16Starting || c->state == Bw16Stopping)
                        snprintf(rows[3], sizeof(rows[3]), "Drops: %lu", (unsigned long)lost);
                    else snprintf(rows[3], sizeof(rows[3]), "%s", name);
                } else {
                    snprintf(rows[2], sizeof(rows[2]), "Drops: %lu", (unsigned long)lost);
                    snprintf(rows[3], sizeof(rows[3]), "%s",
                             c->state == Bw16Active ? "Reconnect own client" : name);
                }
            }
            strcpy(footer, c->state == Bw16Stopped || app->screen == CaptureReport ?
                   "Back: menu Right: file" : "Back: STOP Right: file");
            break;
        }
        strcpy(rows[0], c->state == Bw16Starting ? "Waiting for START..." :
               c->state == Bw16Stopping ? "Waiting for STOP..." :
               c->state == Bw16Stopped ? (app->batch.running ? "Switching target..." : "Device confirmed stop") :
               app->confirm_batch && app->batch.native ? "BW16 cycles selected" : "Device reports active");
        snprintf(rows[1], sizeof(rows[1]), "%s %lu", c->target == Bw16TargetAll ? "Cycles:" : "TX attempts",
                 (unsigned long)(app->confirm_batch ? bw16_batch_counter(&app->batch, c) : c->counter));
        strcpy(rows[2], "Outcome not verified");
        if(app->confirm_batch && app->batch.count && app->batch.position < app->batch.count) {
            if(app->batch.native && !c->selected_ap_valid) {
                snprintf(rows[3], sizeof(rows[3]), "%u radios selected", app->batch.count);
            } else {
                unsigned index = app->batch.native ? c->selected_ap : app->batch.targets[app->batch.position];
                if(index < c->ap_count) snprintf(rows[3], sizeof(rows[3]), "%u/%u %s CH%u", app->batch.position + 1,
                    app->batch.count, c->aps[index].channel <= 14 ? "2.4 GHz" : "5 GHz", c->aps[index].channel);
            }
        }
        strcpy(footer, c->state == Bw16Stopped && !app->batch.running ? "OK / Back: menu" : "OK / Back: request STOP");
        break;
    case Info:
        strcpy(rows[0], c->native_selected ? "Native selected ready" : "FLIPPER_ZERO v4");
        strcpy(rows[1], c->capture_deauth_supported ? "Capture + deauth ready" :
               c->capture_supported ? "Passive PCAP ready" : "Capture needs BW16 4.3");
        strcpy(rows[2], "UART2 115200 / 8N1");
        strcpy(rows[3], "TX43 RX44  3V3 + GND");
        strcpy(footer, "Back / OK: menu");
        break;
    case Error:
        if(c->target == Bw16TargetCapture && app->capture.created) {
            const char* name = strrchr(app->capture.path, '/');
            if(app->capture_details) {
                strcpy(rows[0], "/ext/apps_data/bw16/");
                strcpy(rows[1], "captures/");
                snprintf(rows[2], sizeof(rows[2]), "%s", name ? name + 1 : "");
                strcpy(rows[3], "Partial file retained");
            } else {
                strcpy(rows[0], "Capture failed/partial");
                snprintf(rows[1], sizeof(rows[1]), "%.21s", app->capture.error[0] ? app->capture.error : c->error);
                strcpy(rows[2], c->remote_unknown ? "Power OFF BW16 first!" : "Partial file retained");
                snprintf(rows[3], sizeof(rows[3]), "%s", name ? name + 1 : "");
            }
            strcpy(footer, "Back: exit Right: file");
            break;
        }
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
            if(strcmp(c->error, "Timeout; reset BW16 and reopen") == 0) {
                strcpy(rows[0], "Scan/list timed out");
                strcpy(rows[1], app->rx_bytes ? "No completion reply" : "No UART reply received");
                snprintf(rows[2], sizeof(rows[2]), "RX:%lu Wait:%lus", (unsigned long)(app->rx_bytes > 999999 ? 999999 : app->rx_bytes),
                         (unsigned long)(app->elapsed_ms / 1000 > 999 ? 999 : app->elapsed_ms / 1000));
                strcpy(rows[3], "OK: UART details");
            } else if(app->setup_failed) {
                snprintf(rows[2], sizeof(rows[2]), "Error: 0x%08lx", (unsigned long)(uint32_t)app->io.error);
                strcpy(rows[3], "Close GPS / retry app");
            } else strcpy(rows[2], "Reset BW16 to retry.");
        }
        strcpy(footer, "Back: exit");
        break;
    case Diagnostics:
        if(app->diagnostics_page == 0) {
            snprintf(rows[0], sizeof(rows[0]), "RX bytes: %lu", (unsigned long)app->rx_bytes);
            snprintf(rows[1], sizeof(rows[1]), "Lines:%lu Bad:%lu", (unsigned long)(app->rx_lines > 99999 ? 99999 : app->rx_lines),
                     (unsigned long)(app->rx_dropped > 9999 ? 9999 : app->rx_dropped));
            snprintf(rows[2], sizeof(rows[2]), "Elapsed: %lus", (unsigned long)(app->elapsed_ms / 1000));
            snprintf(rows[3], sizeof(rows[3]), "TX: %.17s", app->last_command[0] ? app->last_command : "none");
        } else if(app->diagnostics_page == 1 || app->diagnostics_page == 2) {
            unsigned start = (app->diagnostics_page - 1) * 63;
            strcpy(rows[0], app->diagnostics_page == 1 ? "Last RX line (1/2)" : "Last RX line (2/2)");
            if(!app->last_line[0]) strcpy(rows[1], "No complete line");
            else for(unsigned i = 0; i < 3; ++i) {
                unsigned offset = start + i * 21;
                if(offset < strlen(app->last_line)) {
                    snprintf(rows[i + 1], sizeof(rows[i + 1]), "%.21s", app->last_line + offset);
                    for(char* ch = rows[i + 1]; *ch; ++ch) if((unsigned char)*ch < 32 || (unsigned char)*ch > 126) *ch = '?';
                }
            }
        } else {
            strcpy(rows[0], "First RX bytes (hex)");
            if(!app->rx_first_count) strcpy(rows[1], "No bytes received");
            for(unsigned i = 0; i < app->rx_first_count; ++i)
                snprintf(rows[1 + i / 4] + (i % 4) * 3, 4, "%02X ", app->rx_first[i]);
            strcpy(rows[3], "115200 baud / 8N1");
        }
        strcpy(footer, "OK: next  Back: return");
        break;
    case Unplug:
        strcpy(rows[0], c->remote_unknown ? "Power OFF BW16 first!" : "UART cleanup failed");
        strcpy(rows[1], c->remote_unknown ? "Stop was not confirmed" : "Retry release of UART");
        strcpy(rows[2], "Then hold OK to exit");
        strcpy(footer, "Hold OK: close app");
        break;
    }
    if(app->notice[0]) snprintf(footer, sizeof(footer), "%s", app->notice);
    bool show_loading = loading(app);
    unsigned spinner_frame = (now_ms() / 100U) % 8U;
    furi_mutex_release(app->mutex);
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "BW16 R4TKN 4.4");
    if(show_loading) {
        static const int8_t x[8] = {0, 3, 4, 3, 0, -3, -4, -3};
        static const int8_t y[8] = {-4, -3, 0, 3, 4, 3, 0, -3};
        for(unsigned i = 0; i < 3; ++i) {
            unsigned phase = (spinner_frame + 8U - i) % 8U;
            canvas_draw_dot(canvas, 120 + x[phase], 6 + y[phase]);
            if(i == 0) {
                canvas_draw_dot(canvas, 121 + x[phase], 6 + y[phase]);
                canvas_draw_dot(canvas, 120 + x[phase], 7 + y[phase]);
            }
        }
    }
    canvas_set_font(canvas, FontSecondary);
    for(unsigned i = 0; i < 4; ++i) draw_row(canvas, 22 + i * 10, rows[i]);
    draw_row(canvas, 63, footer);
}

static void input(InputEvent* event, void* context) {
    App* app = context;
    if(event->type != InputTypeShort && event->type != InputTypeLong &&
       !(event->type == InputTypeRepeat && (event->key == InputKeyUp || event->key == InputKeyDown))) return;
    if(furi_message_queue_put(app->keys, event, 0) != FuriStatusOk) {
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        app->input_overflow = true;
        if(event->key == InputKeyBack) app->back_latched = true;
        furi_mutex_release(app->mutex);
    }
}

static bool quiet(App* app) {
    if((uint32_t)(now_ms() - app->last_rx) < 500) { note(app, "Wait for UART quiet"); return false; }
    size_t buffered = 0;
    if(uart_get_buffered_data_len(BW16_UART, &buffered) != ESP_OK) { fail(app, "UART status failed"); return false; }
    if(buffered || app->line.used || app->line.discard) { note(app, "Wait for UART quiet"); return false; }
    note(app, "");
    return true;
}

static bool receiving_transaction(const App* app) {
    return bw16_control_busy(&app->control) || app->batch.running || app->index_pending;
}

static bool capture_receiving(const App* app) {
    return app->control.target == Bw16TargetCapture &&
           (app->control.state == Bw16Starting || app->control.state == Bw16Active ||
            app->control.state == Bw16Stopping);
}

static void receive(App* app) {
    if(!app->io.installed) return;
    if(uxQueueMessagesWaiting(app->io.events) >= 64 && receiving_transaction(app))
        fail(app, "UART event queue full");
    uart_event_t event;
    for(unsigned i = 0; i < 64 && xQueueReceive(app->io.events, &event, 0) == pdTRUE; ++i)
        if(event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL || event.type == UART_FRAME_ERR ||
           event.type == UART_PARITY_ERR || event.type == UART_BREAK) {
            ++app->rx_dropped;
            if(capture_receiving(app)) app->capture.discard = true;
            else app->line.discard = true;
            if(receiving_transaction(app)) fail(app, "UART RX loss/framing error");
        }
    uint8_t bytes[512];
    int n = uart_read_bytes(BW16_UART, bytes, sizeof(bytes), 0);
    if(n < 0) { fail(app, "UART read failed"); return; }
    if(n) {
        app->last_rx = now_ms();
        app->rx_bytes += (uint32_t)n;
    }
    for(int i = 0; i < n; ++i) {
        if(app->rx_first_count < sizeof(app->rx_first)) app->rx_first[app->rx_first_count++] = bytes[i];
        if(capture_receiving(app)) {
            Bw16CaptureEvent result = bw16_capture_byte(&app->capture, bytes[i], app->control.start_confirmed);
            if(result == Bw16CaptureControl) {
                ++app->rx_lines;
                snprintf(app->last_line, sizeof(app->last_line), "%.127s", app->capture.line);
                bw16_control_line(&app->control, app->capture.line, now_ms());
                sync_state(app);
            } else if(result == Bw16CaptureDropped) ++app->rx_dropped;
            else if(result == Bw16CaptureWriteError) fail(app, app->capture.error);
            else if(result == Bw16CaptureLimit) bw16_control_stop(&app->control, now_ms());
            continue;
        }
        Bw16Frame frame = bw16_frame(&app->line, bytes[i]);
        if(frame == Bw16LineDropped) {
            ++app->rx_dropped;
            if(receiving_transaction(app)) fail(app, "RX line too long/corrupt");
        } else if(frame == Bw16LineReady) {
            ++app->rx_lines;
            snprintf(app->last_line, sizeof(app->last_line), "%s", app->line.text);
            bw16_control_line(&app->control, app->line.text, now_ms());
            sync_state(app);
        }
    }
    if(n) app->dirty = true;
}

static void back(App* app) {
    switch(app->screen) {
    case Operation:
        if(app->batch.running) bw16_batch_stop(&app->batch, &app->control, now_ms());
        else if(app->control.state == Bw16Stopped) { app->control.state = Bw16Idle; app->screen = Menu; }
        else bw16_control_stop(&app->control, now_ms());
        break;
    case Diagnostics: app->screen = app->diagnostics_return; break;
    case Confirm:
        app->screen = app->confirm_batch ? Groups :
            app->pending == Bw16TargetCapture ? Results :
            app->pending == Bw16TargetAll ? Menu : app->pending == Bw16TargetClient ? Clients : ApActions;
        break;
    case PairNames: app->pair_first = BW16_MAX_APS; app->screen = Menu; break;
    case Results: case Info: case Groups: case CaptureReport: app->screen = Menu; break;
    case ApActions: app->screen = Results; break;
    case Clients: app->screen = ApActions; break;
    default:
        if(app->control.remote_unknown) app->screen = Unplug;
        else if(bw16_uart_close(&app->io) == ESP_OK) app->exit_requested = true;
        else app->screen = Unplug;
        break;
    }
    note(app, "");
}

static void begin_scan(App* app, bool stations, bool grouped) {
    if(!quiet(app)) return;
    if(bw16_control_scan(&app->control, stations, now_ms())) {
        memset(&app->selection, 0, sizeof(app->selection));
        app->selection_ready = app->index_pending = app->confirm_batch = false;
        app->grouped_flow = grouped;
        app->selected = app->client = app->action = app->group = app->review = 0;
        app->pair_first = BW16_MAX_APS;
        app->waiting_clients = false;
        app->screen = Waiting;
    }
    sync_state(app);
}

static void review_selection(App* app) {
    if(!selection_valid(app) || !bw16_selection_count(&app->selection)) {
        note(app, "Select networks first");
        return;
    }
    app->review = 0;
    app->confirm_batch = true;
    app->screen = Confirm;
    note(app, "");
}

static void key(App* app, InputEvent event) {
    if(event.type == InputTypeRepeat) event.type = InputTypeShort;
    Bw16Control* c = &app->control;
    if(event.key == InputKeyBack && (event.type == InputTypeShort || event.type == InputTypeLong)) { back(app); return; }
    if(event.type == InputTypeLong && event.key == InputKeyOk) {
        if(app->screen == Unplug) {
            if(bw16_uart_close(&app->io) == ESP_OK) app->exit_requested = true;
            else note(app, "Cleanup failed; retry");
        } else if(app->screen == Operation) {
            back(app);
        } else if(app->screen == Groups) {
            review_selection(app);
        } else if(app->screen == Confirm && quiet(app)) {
            bool started = false;
            if(app->confirm_batch) {
                unsigned indices[BW16_MAX_APS];
                unsigned count = bw16_selection_indices(&app->selection, indices, BW16_MAX_APS);
                if(count > 1 && !c->native_selected) {
                    note(app, "BW16 update needed");
                    return;
                }
                if(selection_valid(app)) started = bw16_batch_start(&app->batch, c, indices, count, now_ms());
            } else if(app->pending == Bw16TargetCapture) {
                uint32_t session = furi_hal_random_get();
                if(!session) session = 1;
                app->capture_details = false;
                if(!bw16_capture_prepare(&app->capture, c->aps[app->selected].mac, session,
                                         furi_hal_rtc_get_timestamp())) {
                    app->screen = CaptureReport;
                    note(app, app->capture.error);
                    return;
                }
                app->capture.anchor = furi_hal_rtc_get_timestamp();
                started = app->capture_deauth_pending ?
                    bw16_control_start_capture_deauth(c, app->selected, session, now_ms()) :
                    bw16_control_start_capture(c, app->selected, session, now_ms());
                if(!started && c->state != Bw16Fault) {
                    app->capture.failed = true;
                    snprintf(app->capture.error, sizeof(app->capture.error), "Capture not started");
                    bw16_capture_close(&app->capture);
                    app->screen = CaptureReport;
                    return;
                }
            } else {
                started = bw16_control_start(c, app->pending, app->selected, app->client, now_ms());
            }
            if(started) app->screen = Operation;
            else if(c->state != Bw16Fault) note(app, "List incomplete; rescan");
            sync_state(app);
        }
    } else if(event.type == InputTypeShort) {
        bool up = event.key == InputKeyUp, down = event.key == InputKeyDown, ok = event.key == InputKeyOk;
        switch(app->screen) {
        case Connect:
            if(ok) {
                app->setup_failed = false;
                if(!bw16_uart_open(&app->io)) {
                    app->setup_failed = true;
                    fail(app, app->io.stage);
                    break;
                }
                /* End a stale partial command before issuing any real command.
                 * A NUL left by a startup BREAK is retained by the v4 parser. */
                app->io.error = bw16_uart_send(&app->io, " \n", 2);
                if(app->io.error != ESP_OK) fail(app, "UART sync TX failed");
                else {
                    memset(&app->line, 0, sizeof(app->line));
                    app->last_rx = app->sync_started = now_ms();
                    app->caps_queried = false;
                    app->screen = Synchronizing;
                }
            }
            break;
        case Menu:
            if(up && app->menu) --app->menu;
            if(down && app->menu + 1 < MenuCount) ++app->menu;
            if(ok) {
                app->intent = app->menu;
                app->capture_deauth_pending = app->menu == MenuCaptureDeauth;
                if(app->menu == MenuScan) begin_scan(app, false, true);
                else if(app->menu == MenuSelect || app->menu == MenuPair) {
                    if(selection_valid(app)) {
                        app->group = 0;
                        app->pair_first = BW16_MAX_APS;
                        app->screen = app->menu == MenuPair ? PairNames : Groups;
                    } else begin_scan(app, false, true);
                } else if(app->menu == MenuSelected) review_selection(app);
                else if(app->menu == MenuCapture || app->menu == MenuCaptureDeauth) {
                    if(app->capture_deauth_pending && !c->capture_deauth_supported)
                        note(app, "BW16 4.4 update needed");
                    else if(!c->capture_supported) note(app, "BW16 capture update needed");
                    else begin_scan(app, true, false);
                }
                else if(app->menu >= MenuStations && app->menu <= MenuAll) begin_scan(app, true, false);
                else if(app->menu == MenuDiagnostics) {
                    app->diagnostics_return = Menu; app->diagnostics_page = 0; app->screen = Diagnostics;
                } else if(app->menu == MenuInfo) app->screen = Info;
                sync_state(app);
            }
            break;
        case Groups:
            if(up && app->group) --app->group;
            if(down && app->group + 1 < app->selection.group_count) ++app->group;
            if(ok && selection_valid(app) && app->selection.group_count)
                bw16_selection_toggle_group(&app->selection, app->group);
            if(up || down || ok) note(app, "");
            break;
        case PairNames:
            if(up && app->group) --app->group;
            if(down && app->group + 1 < app->selection.group_count) ++app->group;
            if(ok && selection_valid(app) && app->selection.group_count) {
                if(app->pair_first == BW16_MAX_APS) {
                    app->pair_first = app->group;
                    if(app->group + 1 < app->selection.group_count) ++app->group;
                    else if(app->group) --app->group;
                } else {
                    unsigned first_ap = app->selection.groups[app->pair_first].first;
                    if(bw16_selection_link_groups(&app->selection, app->pair_first, app->group)) {
                        app->group = app->selection.group_for_ap[first_ap];
                        app->pair_first = BW16_MAX_APS;
                        app->screen = Groups;
                        note(app, "Names paired; OK marks");
                    } else note(app, "Choose another name");
                }
            }
            break;
        case Confirm:
            if(app->confirm_batch) {
                unsigned count = bw16_selection_count(&app->selection);
                if(up && app->review) --app->review;
                if(down && app->review + 1 < count) ++app->review;
            }
            break;
        case Results:
            if(up && app->selected) --app->selected;
            if(down && app->selected + 1 < c->ap_count) ++app->selected;
            if(ok && c->ap_count) {
                if(c->indices_valid) {
                    if(app->intent == MenuObserve || app->intent == MenuClient) {
                        if(quiet(app)) {
                            app->client = 0; app->waiting_clients = true;
                            if(bw16_control_clients(c, app->selected, now_ms())) app->screen = Waiting;
                        }
                    } else if(app->intent == MenuCapture || app->intent == MenuCaptureDeauth) {
                        app->capture_deauth_pending = app->intent == MenuCaptureDeauth;
                        app->confirm_batch = false; app->pending = Bw16TargetCapture; app->screen = Confirm;
                    } else if(app->intent == MenuStation) {
                        app->confirm_batch = false; app->pending = Bw16TargetStation; app->screen = Confirm;
                    } else { app->action = 0; app->screen = ApActions; }
                }
                else if(quiet(app)) {
                    app->intent = MenuStations;
                    begin_scan(app, true, false);
                }
            }
            break;
        case ApActions:
            if(up || down) app->action = 1 - app->action;
            if(ok && app->action == 0 && quiet(app)) {
                app->client = 0; app->waiting_clients = true;
                if(bw16_control_clients(c, app->selected, now_ms())) app->screen = Waiting;
                sync_state(app);
            } else if(ok && app->action == 1) { app->confirm_batch = false; app->pending = Bw16TargetStation; app->screen = Confirm; }
            break;
        case Clients:
            if(up && app->client) --app->client;
            if(down && app->client + 1 < c->client_count) ++app->client;
            if(ok && c->client_count) { app->confirm_batch = false; app->pending = Bw16TargetClient; app->screen = Confirm; }
            break;
        case Error:
            if(event.key == InputKeyRight && c->target == Bw16TargetCapture) app->capture_details = !app->capture_details;
            if(ok && !app->setup_failed && !c->remote_unknown) {
                app->diagnostics_return = Error; app->diagnostics_page = 0; app->screen = Diagnostics;
            }
            break;
        case Diagnostics:
            if(ok || down) app->diagnostics_page = (app->diagnostics_page + 1) % 4;
            if(up) app->diagnostics_page = (app->diagnostics_page + 3) % 4;
            break;
        case CaptureReport:
        case Operation:
            if(event.key == InputKeyRight && (app->screen == CaptureReport || c->target == Bw16TargetCapture))
                app->capture_details = !app->capture_details;
            if(ok) back(app);
            break;
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
        if(app->screen != Unplug && app->screen != Error &&
           !(app->screen == Diagnostics && app->diagnostics_return == Error)) receive(app);
        if(have_key) key(app, event);
        if(app->exit_requested) { furi_mutex_release(app->mutex); break; }
        if(app->screen != Unplug && app->screen != Error &&
           !(app->screen == Diagnostics && app->diagnostics_return == Error)) receive(app);
        uint32_t now = now_ms();
        if(app->screen != Unplug) poll_control(app);
        bool update = (app->dirty || bw16_control_busy(&app->control) || loading(app)) && (uint32_t)(now - redraw) >= 100;
        if(update) { app->dirty = false; redraw = now; }
        furi_mutex_release(app->mutex);
        if(update) view_port_update(app->viewport);
    }
    result = 0;
cleanup:
    bw16_capture_close(&app->capture);
    if(attached) gui_remove_view_port(gui, app->viewport);
    if(app->viewport) view_port_free(app->viewport);
    if(gui) furi_record_close(RECORD_GUI);
    if(app->keys) furi_message_queue_free(app->keys);
    if(app->mutex) furi_mutex_free(app->mutex);
    if(result) FURI_LOG_E(TAG, "GUI allocation failed; UART untouched");
    free(app);
    return result;
}
