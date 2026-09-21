/*
 * BW16 R4TKN — companion app for an AI-Thinker BW16 (RTL8720DN) running
 * R4TKN dual-band deauther firmware, driven over UART.
 *
 * Port of the R4TKN Flipper Zero app (@r4tkn) to the Sor3nt ESP32 Flipper
 * port. The Flipper original talks over furi_hal_serial, which is a no-op
 * stub on this port, so the serial layer is reimplemented directly on the
 * ESP-IDF UART driver.
 *
 * Wiring (T-Embed CC1101 -- same header as the onboard RDM6300/Grove serial
 * port, see BOARD_PIN_RFID_TX/BOARD_PIN_RFID_RX in board_lilygo_t_embed_cc1101.h):
 *   IO43 (TX) -> BW16 RX
 *   IO44 (RX) <- BW16 TX
 *   GND <-> GND, 115200 baud 8N1.
 * 125kHz RFID reading won't work while the BW16 is wired to these pins.
 * These pins don't exist on the ESP32-C6 Waveshare boards -- to use
 * different pins (or a different board), edit BW16_TX_PIN / BW16_RX_PIN
 * below and rebuild.
 *
 * Navigation follows the board's normal input mapping (rotary encoder on
 * T-Embed, physical keys elsewhere) via InputKeyUp/Down/Ok/Back.
 *
 * Wire protocol (confirmed against the BW16 R4TKN firmware image), '\n' term:
 *   TX: SCAN, DEAUTH_ALL, DEAUTH_STATION <idx>, STOP
 *   RX: scan results as "list-station: <ssid>,<ch>,...,<rssi>", ended by
 *       SCAN_DONE / LIST_STATION_DONE; status replies DEAUTH_ALL_START,
 *       DEAUTH_ALL_STOPPED, TARGETED_START/DONE, "NO SCAN RESULTS".
 * (The stock BW16 firmware implements only SCAN / DEAUTH_ALL / DEAUTH_STATION /
 *  STOP; beacon spam, evil portal and per-client deauth are not implemented.)
 */

#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <driver/uart.h>
#include <string.h>
#include <stdlib.h>

#define TAG "BW16R4TKN"

#define BW16_UART     UART_NUM_1
#define BW16_TX_PIN   43 /* T-Embed CC1101 Grove/RFID TX -> BW16 RX */
#define BW16_RX_PIN   44 /* T-Embed CC1101 Grove/RFID RX <- BW16 TX */
#define BW16_BAUD     115200
#define BW16_UART_BUF 1024

#define MAX_APS    32
#define AP_STR_LEN 36
#define LINE_LEN   64
#define LIST_ROWS  5

typedef enum {
    ScreenMenu,
    ScreenScanning,
    ScreenApList,
    ScreenRunning,
} Screen;

typedef enum {
    MenuScan,
    MenuDeauthAll,
    MenuCount,
} MenuItem;

typedef enum {
    RunDeauthAll,
    RunDeauthTarget,
} RunMode;

typedef enum {
    EventKey,
    EventRxLine,
} EventType;

typedef struct {
    EventType type;
    InputEvent input;
    char line[LINE_LEN];
} AppEvent;

typedef struct {
    FuriMutex* mutex;
    ViewPort* view_port;
    FuriMessageQueue* queue;
    FuriThread* rx_thread;
    volatile bool rx_run;

    Screen screen;
    MenuItem menu_sel;
    RunMode run_mode;

    char aps[MAX_APS][AP_STR_LEN];
    uint8_t ap_count;
    uint8_t ap_sel;
    uint8_t ap_top;

    char status[LINE_LEN]; /* last status reply from the BW16 */
} App;

/* ---- UART ---- */

static void bw16_uart_init(void) {
    uart_config_t cfg = {
        .baud_rate = BW16_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_param_config(BW16_UART, &cfg);
    uart_set_pin(BW16_UART, BW16_TX_PIN, BW16_RX_PIN, -1, -1);
    uart_driver_install(BW16_UART, BW16_UART_BUF, 0, 0, NULL, 0);
}

static void bw16_uart_deinit(void) {
    uart_driver_delete(BW16_UART);
}

static void bw16_send(const char* cmd) {
    uart_write_bytes(BW16_UART, cmd, strlen(cmd));
    uart_write_bytes(BW16_UART, "\n", 1);
    FURI_LOG_I(TAG, "TX: %s", cmd);
}

static int32_t bw16_rx_thread(void* ctx) {
    App* app = ctx;
    char buf[LINE_LEN];
    size_t len = 0;

    while(app->rx_run) {
        uint8_t c;
        int n = uart_read_bytes(BW16_UART, &c, 1, pdMS_TO_TICKS(50));
        if(n <= 0) continue;

        if(c == '\r') continue;
        if(c == '\n') {
            if(len == 0) continue;
            buf[len] = '\0';
            AppEvent ev = {.type = EventRxLine};
            strncpy(ev.line, buf, LINE_LEN - 1);
            ev.line[LINE_LEN - 1] = '\0';
            furi_message_queue_put(app->queue, &ev, 0);
            len = 0;
        } else if(len < LINE_LEN - 1) {
            buf[len++] = (char)c;
        }
    }
    return 0;
}

/* ---- Rendering ---- */

/* Title bar with an underline, shared by every screen. */
static void draw_header(Canvas* canvas, const char* title) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 11, title);
    canvas_draw_line(canvas, 0, 14, 127, 14);
    canvas_set_font(canvas, FontSecondary);
}

static void draw_footer(Canvas* canvas, const char* hint) {
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 63, hint);
}

static void render_menu(Canvas* canvas, App* app) {
    static const char* const items[MenuCount] = {
        "Scan Networks",
        "Deauth All",
    };
    draw_header(canvas, "BW16");
    for(uint8_t i = 0; i < MenuCount; i++) {
        int y = 28 + i * 13;
        if(i == app->menu_sel) {
            canvas_draw_box(canvas, 0, y - 10, 128, 13);
            canvas_set_color(canvas, ColorWhite);
        }
        canvas_draw_str(canvas, 6, y, items[i]);
        canvas_set_color(canvas, ColorBlack);
    }
    draw_footer(canvas, "Up/Down move   OK select");
}

static void render_scanning(Canvas* canvas, App* app) {
    char b[32];
    draw_header(canvas, "Scanning");
    snprintf(b, sizeof(b), "Networks found: %u", app->ap_count);
    canvas_draw_str(canvas, 2, 34, b);
    draw_footer(canvas, "Back to cancel");
}

static void render_ap_list(Canvas* canvas, App* app) {
    if(app->ap_count == 0) {
        draw_header(canvas, "Networks");
        canvas_draw_str(canvas, 2, 34, "None found - scan again");
        draw_footer(canvas, "Back to menu");
        return;
    }
    canvas_set_font(canvas, FontSecondary);
    for(uint8_t row = 0; row < LIST_ROWS; row++) {
        uint8_t idx = app->ap_top + row;
        if(idx >= app->ap_count) break;
        int y = 10 + row * 11;
        if(idx == app->ap_sel) {
            canvas_draw_box(canvas, 0, y - 9, 128, 11);
            canvas_set_color(canvas, ColorWhite);
        }
        char line[48];
        snprintf(line, sizeof(line), "%2u %s", idx, app->aps[idx]);
        canvas_draw_str(canvas, 2, y, line);
        canvas_set_color(canvas, ColorBlack);
    }
    draw_footer(canvas, "Enter: deauth   Back: menu");
}

static void render_running(Canvas* canvas, App* app) {
    draw_header(canvas, app->run_mode == RunDeauthAll ? "Deauth All" : "Targeted Deauth");
    if(app->run_mode == RunDeauthTarget && app->ap_sel < app->ap_count) {
        char b[48];
        snprintf(b, sizeof(b), "Target: %s", app->aps[app->ap_sel]);
        canvas_draw_str(canvas, 2, 30, b);
    }
    /* live status reply from the BW16 (DEAUTH_ALL_START, NO SCAN RESULTS, ...) */
    canvas_draw_str(canvas, 2, 44, app->status[0] ? app->status : "Running on BW16...");
    draw_footer(canvas, "OK / Back to stop");
}

static void render_callback(Canvas* canvas, void* ctx) {
    App* app = ctx;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    canvas_clear(canvas);
    switch(app->screen) {
    case ScreenMenu:
        render_menu(canvas, app);
        break;
    case ScreenScanning:
        render_scanning(canvas, app);
        break;
    case ScreenApList:
        render_ap_list(canvas, app);
        break;
    case ScreenRunning:
        render_running(canvas, app);
        break;
    }
    furi_mutex_release(app->mutex);
}

static void input_callback(InputEvent* input, void* ctx) {
    FuriMessageQueue* queue = ctx;
    AppEvent ev = {.type = EventKey, .input = *input};
    furi_message_queue_put(queue, &ev, FuriWaitForever);
}

/* ---- Logic ---- */

/* A line the BW16 sends as status/marker rather than an AP entry. */
static bool is_status_line(const char* l) {
    return strstr(l, "_START") || strstr(l, "_STOPPED") || strstr(l, "_DONE") ||
           strstr(l, "NO SCAN RESULTS") || strcmp(l, "SCAN") == 0;
}

/* Pull the SSID out of a "list-station: <ssid>,<ch>,...,<rssi>" line. */
static void parse_ap(const char* line, char* out, size_t outsz) {
    const char* p = strstr(line, "list-station:");
    p = p ? p + strlen("list-station:") : line;
    while(*p == ' ') p++;
    size_t i = 0;
    while(*p && *p != ',' && i < outsz - 1) out[i++] = *p++;
    while(i > 0 && out[i - 1] == ' ') i--;
    out[i] = '\0';
    if(out[0] == '\0') {
        strncpy(out, line, outsz - 1);
        out[outsz - 1] = '\0';
    }
}

static void handle_line(App* app, const char* line) {
    if(app->screen == ScreenScanning &&
       (strcmp(line, "SCAN_DONE") == 0 || strcmp(line, "LIST_STATION_DONE") == 0)) {
        app->screen = (app->ap_count > 0) ? ScreenApList : ScreenMenu;
        app->ap_sel = 0;
        app->ap_top = 0;
        return;
    }

    if(is_status_line(line)) {
        strncpy(app->status, line, LINE_LEN - 1);
        app->status[LINE_LEN - 1] = '\0';
        if(app->screen == ScreenScanning && strstr(line, "NO SCAN RESULTS")) {
            app->screen = ScreenMenu;
        }
        return;
    }

    if(app->screen == ScreenScanning && app->ap_count < MAX_APS) {
        parse_ap(line, app->aps[app->ap_count], AP_STR_LEN);
        app->ap_count++;
    }
}

/* returns false to exit the app */
static bool handle_key(App* app, InputKey key) {
    switch(app->screen) {
    case ScreenMenu:
        if(key == InputKeyUp) {
            app->menu_sel = (app->menu_sel + MenuCount - 1) % MenuCount;
        } else if(key == InputKeyDown) {
            app->menu_sel = (app->menu_sel + 1) % MenuCount;
        } else if(key == InputKeyOk) {
            app->status[0] = '\0';
            if(app->menu_sel == MenuScan) {
                app->ap_count = 0;
                app->screen = ScreenScanning;
                bw16_send("SCAN");
            } else { /* MenuDeauthAll */
                app->run_mode = RunDeauthAll;
                app->screen = ScreenRunning;
                bw16_send("DEAUTH_ALL");
            }
        } else if(key == InputKeyBack) {
            return false;
        }
        break;

    case ScreenScanning:
        if(key == InputKeyBack) {
            bw16_send("STOP");
            app->screen = ScreenMenu;
        }
        break;

    case ScreenApList:
        if(key == InputKeyUp) {
            if(app->ap_sel > 0) app->ap_sel--;
        } else if(key == InputKeyDown) {
            if(app->ap_sel + 1 < app->ap_count) app->ap_sel++;
        } else if(key == InputKeyOk) {
            if(app->ap_count > 0) {
                char cmd[24];
                snprintf(cmd, sizeof(cmd), "DEAUTH_STATION %u", app->ap_sel);
                app->status[0] = '\0';
                app->run_mode = RunDeauthTarget;
                app->screen = ScreenRunning;
                bw16_send(cmd);
            }
        } else if(key == InputKeyBack) {
            app->screen = ScreenMenu;
        }
        if(app->ap_sel < app->ap_top) {
            app->ap_top = app->ap_sel;
        } else if(app->ap_sel >= app->ap_top + LIST_ROWS) {
            app->ap_top = app->ap_sel - (LIST_ROWS - 1);
        }
        break;

    case ScreenRunning:
        if(key == InputKeyOk || key == InputKeyBack) {
            bw16_send("STOP");
            app->screen = (app->run_mode == RunDeauthTarget) ? ScreenApList : ScreenMenu;
        }
        break;
    }
    return true;
}

/* ---- Entry ---- */

int32_t bw16_r4tkn_app(void* p) {
    UNUSED(p);

    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->queue = furi_message_queue_alloc(16, sizeof(AppEvent));
    app->screen = ScreenMenu;

    bw16_uart_init();
    app->rx_run = true;
    app->rx_thread = furi_thread_alloc_ex("bw16_rx", 2048, bw16_rx_thread, app);
    furi_thread_start(app->rx_thread);

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, render_callback, app);
    view_port_input_callback_set(app->view_port, input_callback, app->queue);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, app->view_port, GuiLayerFullscreen);

    bool running = true;
    AppEvent ev;
    while(running) {
        if(furi_message_queue_get(app->queue, &ev, FuriWaitForever) != FuriStatusOk) continue;

        furi_mutex_acquire(app->mutex, FuriWaitForever);
        if(ev.type == EventKey) {
            if(ev.input.type == InputTypeShort) {
                running = handle_key(app, ev.input.key);
            }
        } else {
            handle_line(app, ev.line);
        }
        furi_mutex_release(app->mutex);

        view_port_update(app->view_port);
    }

    bw16_send("STOP");
    app->rx_run = false;
    furi_thread_join(app->rx_thread);
    furi_thread_free(app->rx_thread);
    bw16_uart_deinit();

    gui_remove_view_port(gui, app->view_port);
    view_port_free(app->view_port);
    furi_record_close(RECORD_GUI);
    furi_message_queue_free(app->queue);
    furi_mutex_free(app->mutex);
    free(app);
    return 0;
}
