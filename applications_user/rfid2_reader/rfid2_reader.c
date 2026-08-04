// RFID2 Reader — M5Stack Unit RFID2 (WS1850S, 13.56 MHz, I2C @0x28).
// Continuously polls for ISO14443-A cards and shows the UID. Back exits.
//
// NOT hardware-tested — validate on the unit. If nothing reads, confirm the Grove
// port is on the shared I2C bus (see RFID2_I2C_PORT in components/rfid2/rfid2.h).

#include <furi.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/view.h>

#include <rfid2/rfid2.h>

#include <string.h>
#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define R2_VIEW 0

typedef struct {
    ViewDispatcher* view_dispatcher;
    View* view;
    Gui* gui;
    FuriMutex* mutex;

    volatile bool running;
    volatile bool worker_alive;
    TaskHandle_t worker;
    bool radio_ok;
    uint8_t version;

    // shared
    uint32_t read_count;
    size_t uid_len;
    uint8_t uid[10];
    bool card_present;
    uint8_t scan[16]; // I2C addresses that ACKed on the Grove bus
    int scan_n;
} R2App;

static void r2_scan_str(R2App* app, char* out, size_t n) {
    int off = snprintf(out, n, "I2C:");
    if(app->scan_n <= 0) {
        snprintf(out + off, n - off, app->scan_n == 0 ? " (empty)" : " bus err");
        return;
    }
    for(int i = 0; i < app->scan_n && off < (int)n - 4; i++)
        off += snprintf(out + off, n - off, " %02X", app->scan[i]);
}

static void r2_worker(void* ctx) {
    R2App* app = ctx;
    while(app->running) {
        uint8_t uid[10];
        size_t len = 0;
        uint8_t sak = 0;
        bool found = rfid2_read_uid(uid, sizeof(uid), &len, &sak);
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        if(found && len) {
            if(app->uid_len != len || memcmp(app->uid, uid, len) != 0) app->read_count++;
            app->uid_len = len;
            memcpy(app->uid, uid, len);
            app->card_present = true;
        } else {
            app->card_present = false;
        }
        furi_mutex_release(app->mutex);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    app->worker_alive = false;
    vTaskDelete(NULL);
}

static void r2_draw(Canvas* canvas, void* model) {
    R2App* app = *(R2App**)model;
    uint32_t rc;
    size_t len;
    uint8_t uid[10];
    bool present;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    rc = app->read_count;
    len = app->uid_len;
    memcpy(uid, app->uid, sizeof(uid));
    present = app->card_present;
    furi_mutex_release(app->mutex);

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 11, "RFID2 Reader");
    canvas_set_font(canvas, FontSecondary);

    char buf[52];
    if(!app->radio_ok) {
        canvas_draw_str(canvas, 2, 24, "WS1850S not found");
        snprintf(buf, sizeof(buf), "ver 0x%02X (G2/G1, 0x28)", app->version);
        canvas_draw_str(canvas, 2, 35, buf);
        r2_scan_str(app, buf, sizeof(buf));
        canvas_draw_str(canvas, 2, 47, buf); // bus scan — read this to me
        canvas_draw_str(canvas, 2, 62, "Back=exit");
        return;
    }
    snprintf(buf, sizeof(buf), "Reads: %lu   %s", (unsigned long)rc, present ? "CARD" : "-");
    canvas_draw_str(canvas, 2, 26, buf);

    if(len) {
        char hex[33];
        for(size_t i = 0; i < len && i < 10; i++) snprintf(&hex[i * 3], 4, "%02X ", uid[i]);
        hex[len * 3] = 0;
        canvas_draw_str(canvas, 2, 40, "UID:");
        canvas_draw_str(canvas, 24, 40, hex);
        snprintf(buf, sizeof(buf), "%u-byte UID", (unsigned)len);
        canvas_draw_str(canvas, 2, 51, buf);
    } else {
        canvas_draw_str(canvas, 2, 40, "present a card...");
    }
    canvas_draw_str(canvas, 2, 62, "Back=exit");
}

static bool r2_input(InputEvent* event, void* ctx) {
    R2App* app = ctx;
    if(event->type == InputTypeShort && event->key == InputKeyBack) {
        view_dispatcher_stop(app->view_dispatcher);
        return true;
    }
    return false;
}

static void r2_tick(void* ctx) {
    R2App* app = ctx;
    with_view_model(app->view, R2App** m, { *m = app; }, true);
}

int32_t rfid2_reader_app(void* p) {
    UNUSED(p);
    R2App* app = malloc(sizeof(R2App));
    if(!app) return 0;
    memset(app, 0, sizeof(*app));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);

    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_tick_event_callback(app->view_dispatcher, r2_tick, 200);

    app->view = view_alloc();
    view_allocate_model(app->view, ViewModelTypeLockFree, sizeof(R2App*));
    with_view_model(app->view, R2App** m, { *m = app; }, false);
    view_set_context(app->view, app);
    view_set_draw_callback(app->view, r2_draw);
    view_set_input_callback(app->view, r2_input);
    view_dispatcher_add_view(app->view_dispatcher, R2_VIEW, app->view);
    view_dispatcher_switch_to_view(app->view_dispatcher, R2_VIEW);

    app->scan_n = rfid2_scan(app->scan, sizeof(app->scan));
    app->radio_ok = rfid2_init();
    app->version = rfid2_version();
    if(app->radio_ok) {
        app->running = true;
        app->worker_alive = true;
        if(xTaskCreate(r2_worker, "rfid2", 4096, app, tskIDLE_PRIORITY + 4, &app->worker) != pdPASS) {
            app->worker_alive = false;
        }
    }

    view_dispatcher_run(app->view_dispatcher);

    app->running = false;
    // The worker can be mid-I2C-transaction in rfid2_read_uid() past any fixed
    // cap; freeing app/mutex while it runs is a use-after-free. It clears
    // worker_alive before vTaskDelete, so wait unconditionally.
    while(app->worker_alive) furi_delay_ms(20);

    view_dispatcher_remove_view(app->view_dispatcher, R2_VIEW);
    view_free(app->view);
    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);
    furi_mutex_free(app->mutex);
    free(app);
    return 0;
}
