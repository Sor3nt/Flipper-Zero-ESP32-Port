/**
 * OTA-Updater (System-App, nicht im Menue).
 *
 * Flasht eine bereits auf der SD liegende furi_esp32.bin in die inaktive
 * OTA-Partition, zeigt dabei Vollbild-Fortschritt und rebootet danach.
 * Gestartet vom RPC-Handler (qT-Embed: system_update_request +
 * system_reboot_request(UPDATE)) — Launch-Arg:
 *
 *     "rpc:/ext/update/furi_esp32.bin"   Update kam per USB-RPC → nach dem
 *                                        Reboot qFlipper-Bridge wieder starten
 *     "/ext/update/furi_esp32.bin"       nur flashen + rebooten
 *
 * Der eigentliche Flash-Vorgang laeuft in einem xTaskCreate-Task mit INTERNEM
 * DRAM-Stack (fw_ota_flash_file, siehe fw_ota.h). Der Worker publiziert nur
 * volatile Felder (Phase/Prozent/Version/Fehler); der App-Thread pollt sie im
 * ViewDispatcher-Tick und aktualisiert Anzeige + LED (kein Furi-Queue-Zugriff
 * aus dem rohen FreeRTOS-Task — gleiches Muster wie wlan_fw_update.c).
 * Waehrend des Flashens wird Back ignoriert; bei einem Fehler bleibt die
 * Meldung stehen, bis der Nutzer Back drueckt.
 */

#include <furi.h>
#include <furi_hal_power.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <gui/elements.h>
#include <input/input.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>
#include <fw_ota/fw_ota.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_system.h>

#include <string.h>
#include <stdio.h>

#define TAG "OtaUpdater"

#define OTA_ARG_RPC_PREFIX "rpc:"
#define OTA_VIEW_ID 0
#define OTA_WORKER_STACK 8192
#define OTA_REBOOT_DELAY_MS 1500
#define OTA_TICK_MS 100

typedef enum {
    OtaPhasePreparing,
    OtaPhaseFlashing,
    OtaPhaseDone,
    OtaPhaseError,
} OtaPhase;

typedef enum {
    OtaEventExit,
} OtaEvent;

typedef struct {
    OtaPhase phase;
    uint8_t percent;
    char version[32];
    char err[64];
} OtaModel;

typedef struct {
    Gui* gui;
    NotificationApp* notification;
    ViewDispatcher* view_dispatcher;
    View* view;

    const char* path;
    bool from_rpc;

    /* Worker → App (volatile, nur der Worker schreibt; App pollt im Tick). */
    volatile OtaPhase phase;
    volatile uint8_t percent;
    volatile bool worker_running;
    char version[32];
    char err[64];

    /* Zuletzt angezeigter Stand (nur App-Thread). */
    OtaPhase shown_phase;
    uint8_t shown_percent;
} OtaUpdater;

/* Backlight waehrend des Updates zwingend an, danach wieder Automatik. */
static const NotificationSequence seq_backlight_enforce_on = {
    &message_display_backlight_enforce_on,
    NULL,
};
static const NotificationSequence seq_backlight_enforce_auto = {
    &message_display_backlight_enforce_auto,
    NULL,
};

/* ------------------------------------------------------------------------ */
/* View                                                                     */
/* ------------------------------------------------------------------------ */

static void ota_view_draw(Canvas* canvas, void* _model) {
    OtaModel* m = _model;
    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 10, AlignCenter, AlignBottom, "FIRMWARE UPDATE");
    canvas_draw_line(canvas, 0, 13, 127, 13);

    canvas_set_font(canvas, FontSecondary);
    char line[48];

    if(m->phase == OtaPhaseError) {
        canvas_draw_str_aligned(canvas, 64, 27, AlignCenter, AlignBottom, "Update failed");
        canvas_draw_str_aligned(canvas, 64, 40, AlignCenter, AlignBottom, m->err);
        canvas_draw_str_aligned(canvas, 64, 60, AlignCenter, AlignBottom, "Press Back to exit");
        return;
    }

    if(m->version[0]) {
        snprintf(line, sizeof(line), "Installing %s", m->version);
    } else {
        snprintf(line, sizeof(line), "Preparing...");
    }
    canvas_draw_str_aligned(canvas, 64, 27, AlignCenter, AlignBottom, line);

    char ptxt[8];
    snprintf(ptxt, sizeof(ptxt), "%u%%", m->percent);
    float p = m->percent > 100 ? 1.0f : (float)m->percent / 100.0f;
    elements_progress_bar_with_text(canvas, 4, 32, 120, p, ptxt);

    canvas_draw_str_aligned(
        canvas,
        64,
        56,
        AlignCenter,
        AlignBottom,
        (m->phase == OtaPhaseDone) ? "Done, rebooting..." : "Do not power off");
}

static bool ota_view_input(InputEvent* event, void* context) {
    OtaUpdater* app = context;
    /* Alles schlucken; Back beendet die App nur im Fehlerfall. */
    if(event->type == InputTypeShort && event->key == InputKeyBack && app->phase == OtaPhaseError) {
        view_dispatcher_send_custom_event(app->view_dispatcher, OtaEventExit);
    }
    return true;
}

static void ota_model_sync(OtaUpdater* app) {
    with_view_model(
        app->view,
        OtaModel * m,
        {
            m->phase = app->phase;
            m->percent = app->percent;
            strncpy(m->version, app->version, sizeof(m->version) - 1);
            m->version[sizeof(m->version) - 1] = '\0';
            strncpy(m->err, app->err, sizeof(m->err) - 1);
            m->err[sizeof(m->err) - 1] = '\0';
        },
        true);
}

static bool ota_custom_event(void* context, uint32_t event) {
    OtaUpdater* app = context;
    if(event == OtaEventExit) {
        view_dispatcher_stop(app->view_dispatcher);
        return true;
    }
    return false;
}

/* Tick: Worker-Zustand in die View spiegeln, LED bei Phasenwechsel. */
static void ota_tick_event(void* context) {
    OtaUpdater* app = context;
    OtaPhase phase = app->phase;
    uint8_t percent = app->percent;

    if(phase == app->shown_phase && percent == app->shown_percent) return;

    if(phase != app->shown_phase) {
        if(phase == OtaPhaseError) {
            notification_message(app->notification, &sequence_error);
        } else if(phase == OtaPhaseDone) {
            notification_message(app->notification, &sequence_success);
        }
    }
    app->shown_phase = phase;
    app->shown_percent = percent;
    ota_model_sync(app);
}

static bool ota_navigation_event(void* context) {
    UNUSED(context);
    /* Back wird bereits im View-Input behandelt; hier nie beenden. */
    return true;
}

/* ------------------------------------------------------------------------ */
/* Worker (interner Stack)                                                  */
/* ------------------------------------------------------------------------ */

static void ota_worker_fail(OtaUpdater* app, const char* msg) {
    strncpy(app->err, msg, sizeof(app->err) - 1);
    app->err[sizeof(app->err) - 1] = '\0';
    FURI_LOG_E(TAG, "%s", app->err);
    app->phase = OtaPhaseError; /* zuletzt setzen: der Tick liest err nach phase */
}

static void ota_worker_progress(uint32_t done, uint32_t total, void* ctx) {
    OtaUpdater* app = ctx;
    app->percent = total ? (uint8_t)((uint64_t)done * 100u / total) : 0;
}

static void ota_worker_task(void* arg) {
    OtaUpdater* app = arg;

    FwOtaImageInfo info;
    FwOtaImageStatus st = fw_ota_inspect_image(app->path, &info);
    if(st != FwOtaImageOk) {
        ota_worker_fail(app, fw_ota_image_status_str(st));
        app->worker_running = false;
        vTaskDelete(NULL);
        return;
    }
    strncpy(app->version, info.version, sizeof(app->version) - 1);
    app->version[sizeof(app->version) - 1] = '\0';
    app->percent = 0;
    app->phase = OtaPhaseFlashing;

    FURI_LOG_I(
        TAG,
        "flashing %s (%s, %lu bytes, from %s)",
        app->path,
        info.version,
        (unsigned long)info.size,
        app->from_rpc ? "rpc" : "local");

    char err[64] = {0};
    if(!fw_ota_flash_file(app->path, ota_worker_progress, app, err, sizeof(err))) {
        ota_worker_fail(app, err[0] ? err : "flash failed");
        app->worker_running = false;
        vTaskDelete(NULL);
        return;
    }

    /* Marker auf die neue Version, Staging-Datei weg, Resume-Flag setzen. */
    if(info.version[0]) fw_ota_marker_write(info.version);
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_common_remove(storage, app->path);
    furi_record_close(RECORD_STORAGE);
    fw_ota_set_resume_qflipper(app->from_rpc);

    app->percent = 100;
    app->phase = OtaPhaseDone;

    FURI_LOG_I(TAG, "done, rebooting in %d ms", OTA_REBOOT_DELAY_MS);
    vTaskDelay(pdMS_TO_TICKS(OTA_REBOOT_DELAY_MS));
    /* Direkt aus diesem Task (interner Stack) — nie von einem PSRAM-Stack. */
    esp_restart();
}

/* ------------------------------------------------------------------------ */
/* Entry                                                                    */
/* ------------------------------------------------------------------------ */

int32_t ota_updater_app(void* p) {
    const char* args = p;
    OtaUpdater* app = malloc(sizeof(OtaUpdater));
    memset(app, 0, sizeof(*app));

    if(args && strncmp(args, OTA_ARG_RPC_PREFIX, strlen(OTA_ARG_RPC_PREFIX)) == 0) {
        app->from_rpc = true;
        app->path = args + strlen(OTA_ARG_RPC_PREFIX);
    } else if(args && args[0]) {
        app->path = args;
    } else {
        app->path = FW_OTA_STAGE_BIN;
    }

    app->gui = furi_record_open(RECORD_GUI);
    app->notification = furi_record_open(RECORD_NOTIFICATION);

    app->view_dispatcher = view_dispatcher_alloc();
    app->view = view_alloc();
    view_allocate_model(app->view, ViewModelTypeLocking, sizeof(OtaModel));
    view_set_context(app->view, app);
    view_set_draw_callback(app->view, ota_view_draw);
    view_set_input_callback(app->view, ota_view_input);

    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, ota_custom_event);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, ota_navigation_event);
    view_dispatcher_set_tick_event_callback(app->view_dispatcher, ota_tick_event, OTA_TICK_MS);
    view_dispatcher_add_view(app->view_dispatcher, OTA_VIEW_ID, app->view);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_switch_to_view(app->view_dispatcher, OTA_VIEW_ID);

    app->phase = OtaPhasePreparing;
    app->shown_phase = OtaPhasePreparing;
    app->shown_percent = 0;
    ota_model_sync(app);

    furi_hal_power_insomnia_enter();
    notification_message(app->notification, &seq_backlight_enforce_on);

    app->worker_running = true;
    if(xTaskCreate(ota_worker_task, "OtaWorker", OTA_WORKER_STACK, app, 4, NULL) != pdPASS) {
        app->worker_running = false;
        ota_worker_fail(app, "worker spawn failed"); /* App-Thread: Tick zeigt es an */
    }

    view_dispatcher_run(app->view_dispatcher);

    /* Nur im Fehlerfall erreichbar (Erfolg endet in esp_restart). */
    while(app->worker_running) {
        furi_delay_ms(50);
    }

    notification_message(app->notification, &seq_backlight_enforce_auto);
    furi_hal_power_insomnia_exit();

    view_dispatcher_remove_view(app->view_dispatcher, OTA_VIEW_ID);
    view_free(app->view);
    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);
    free(app);
    return 0;
}
