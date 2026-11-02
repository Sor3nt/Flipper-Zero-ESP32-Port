#include "../bw16_app_i.h"

void bw16_scene_console_on_enter(void* context) {
    Bw16App* app = context;

    TextBox* text_box = app->text_box;
    text_box_reset(text_box);
    text_box_set_font(text_box, TextBoxFontText);
    text_box_set_focus(text_box, TextBoxFocusEnd);

    /* Seed the console with a title / hint line, then snapshot it for the TextBox.
     * All of text_box_store is shared with the UART worker, so touch it under the mutex. */
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    furi_string_reset(app->text_box_store);
    if(app->console_title) {
        furi_string_cat_printf(app->text_box_store, "== %s ==\n", app->console_title);
    }
    furi_string_cat_str(
        app->text_box_store,
        app->console_continuous ? "Back: stop & exit\n\n" : "Back: exit\n\n");
    app->text_box_store_strlen = furi_string_size(app->text_box_store);
    /* Snapshot into the idle buffer, then point the TextBox at it (see text_box_view note). */
    const uint8_t next = app->text_box_view_index ^ 1;
    furi_string_set(app->text_box_view[next], app->text_box_store);
    app->console_dirty = false;
    furi_mutex_release(app->mutex);

    text_box_set_text(text_box, furi_string_get_cstr(app->text_box_view[next]));
    app->text_box_view_index = next;

    view_dispatcher_switch_to_view(app->view_dispatcher, Bw16AppViewTextBox);

    /* Fire the command. */
    if(app->tx_command[0] != '\0') {
        bw16_uart_send_line(app->uart, app->tx_command);
    }
}

bool bw16_scene_console_on_event(void* context, SceneManagerEvent event) {
    Bw16App* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeTick) {
        bool updated = false;
        const uint8_t next = app->text_box_view_index ^ 1;
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        if(app->console_dirty) {
            furi_string_set(app->text_box_view[next], app->text_box_store);
            app->console_dirty = false;
            updated = true;
        }
        furi_mutex_release(app->mutex);
        if(updated) {
            text_box_set_text(app->text_box, furi_string_get_cstr(app->text_box_view[next]));
            app->text_box_view_index = next;
        }
        consumed = true;
    }

    return consumed;
}

void bw16_scene_console_on_exit(void* context) {
    Bw16App* app = context;

    /* Best-effort stop: the R4TKN attack modes toggle on a repeated command, so sending
     * the same command once more tells the BW16 to stop before we leave. One-shot modes
     * (scan) need nothing. */
    if(app->console_continuous && app->tx_command[0] != '\0') {
        bw16_uart_send_line(app->uart, app->tx_command);
        furi_delay_ms(50);
    }

    text_box_reset(app->text_box);
}
