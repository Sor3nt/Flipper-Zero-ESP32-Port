#include "../esp_flasher_app_i.h"

#include "../esp_flasher_worker.h"

void esp_flasher_scene_console_output_on_enter(void* context) {
    EspFlasherApp* app = context;

    // Reset text box and set font
    TextBox* text_box = app->text_box;
    text_box_reset(text_box);
    text_box_set_font(text_box, TextBoxFontText);

    // Set focus on end
    text_box_set_focus(text_box, TextBoxFocusEnd);

    // Every job starts with an empty console
    esp_flasher_console_clear(app);
    esp_flasher_console_show(app);

    scene_manager_set_scene_state(app->scene_manager, EspFlasherSceneConsoleOutput, 0);
    view_dispatcher_switch_to_view(app->view_dispatcher, EspFlasherAppViewConsoleOutput);

    // A refresh dropped by another scene would otherwise block every later one.
    furi_check(furi_mutex_acquire(app->console_mutex, FuriWaitForever) == FuriStatusOk);
    app->console_refresh_pending = false;
    app->console_active = true;
    furi_check(furi_mutex_release(app->console_mutex) == FuriStatusOk);

    // Target output shows up here whenever the flasher is not using the UART
    esp_flasher_uart_set_handle_rx_data_cb(app->uart, esp_flasher_worker_handle_rx_data_cb);

    esp_flasher_worker_start_thread(app);
}

bool esp_flasher_scene_console_output_on_event(void* context, SceneManagerEvent event) {
    EspFlasherApp* app = context;

    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == EspFlasherEventRefreshConsoleOutput) {
            furi_check(furi_mutex_acquire(app->console_mutex, FuriWaitForever) == FuriStatusOk);
            app->console_refresh_pending = false;
            furi_check(furi_mutex_release(app->console_mutex) == FuriStatusOk);
            esp_flasher_console_show(app);
        }
        consumed = true;
    } else if(event.type == SceneManagerEventTypeTick) {
        consumed = true;
    } else if(app->flash_worker_busy) {
        // ignore button presses while flashing
        consumed = true;
    }

    return consumed;
}

void esp_flasher_scene_console_output_on_exit(void* context) {
    EspFlasherApp* app = context;

    esp_flasher_worker_stop_thread(app);

    // Unregister rx callback
    esp_flasher_uart_set_handle_rx_data_cb(app->uart, NULL);
    furi_check(furi_mutex_acquire(app->console_mutex, FuriWaitForever) == FuriStatusOk);
    app->console_active = false;
    furi_check(furi_mutex_release(app->console_mutex) == FuriStatusOk);
}
