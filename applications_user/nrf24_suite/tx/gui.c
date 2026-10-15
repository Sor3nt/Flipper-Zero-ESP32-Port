#include "gui/elements.h"
#include "tx.h"
#include "../helper.h"

VariableItem* tx_item[TX_SETTING_COUNT];
static uint8_t bytes_input[BYTE_MAC_LEN];
static uint8_t bytes_input_buffer_size;
static uint8_t bytes_input_current_addr_index;
static VariableItem* bytes_input_item;
static const uint8_t start_x = 0;
static const uint8_t start_y = 25;

static void start_tool_thread(Nrf24Tool* app) {
    app->tool_running = true;
    app->tool_thread = furi_thread_alloc_ex("TX", 8192, nrf24_tx, app);
    furi_thread_set_state_context(app->tool_thread, app);
    furi_thread_set_state_callback(app->tool_thread, tx_thread_state_callback);
    furi_thread_start(app->tool_thread);
}

static void stop_tool_thread(Nrf24Tool* app) {
    app->tool_running = false;
    if(app->tool_thread) {
        furi_thread_join(app->tool_thread);
        furi_thread_free(app->tool_thread);
        app->tool_thread = NULL;
    }
}

static void run_draw_callback(Canvas* canvas, void* model) {
    TxStatus* status = (TxStatus*)model;
    char channel_str[16];
    char addr_str[32];
    char pl_hex[TX_PAYLOAD_SIZE_HEX];
    char pl_ascii[TX_PAYLOAD_SIZE_ASCII];

    // application status
    canvas_set_font(canvas, FontSecondary);
    if(status->thread_state == FuriThreadStateRunning) {
        canvas_draw_str_aligned(canvas, 0, 0, AlignLeft, AlignTop, "Status : RUN");
    } else {
        canvas_draw_str_aligned(canvas, 0, 0, AlignLeft, AlignTop, "Status : IDLE");
    }
    snprintf(channel_str, sizeof(channel_str), "Channel : %u", status->nrf24_config->channel);
    canvas_draw_str_aligned(canvas, canvas_width(canvas), 0, AlignRight, AlignTop, channel_str);

    strcpy(addr_str, "TX addr: ");
    hexlify(status->nrf24_config->tx_addr, ADDR_WIDTH_5_BYTES, addr_str + 9);
    canvas_draw_str_aligned(canvas, 0, 9, AlignLeft, AlignTop, addr_str);

    // tx data
    hexlify(status->payload, status->payload_size, pl_hex);
    buffer_to_ascii(status->payload, status->payload_size, pl_ascii);
    canvas_set_custom_u8g2_font(canvas, u8g2_font_5x7_tr);
    const char* lines[10];
    uint8_t line_index = 0;

    lines[line_index++] = "Payload (HEX):";
    lines[line_index++] = pl_hex + status->scroll_hori;
    lines[line_index++] = "Payload (ASCII):";
    lines[line_index++] = (status->scroll_hori < TX_PAYLOAD_SIZE_ASCII) ?
                              pl_ascii + status->scroll_hori :
                              pl_ascii + TX_PAYLOAD_SIZE_ASCII;
    if(status->send_ok || status->send_failed) {
        lines[line_index++] = (status->send_ok) ? "Result: SUCCESS" : "Result: FAILED";
        if(status->ack_payload_enabled) {
            if(status->ack_payload_size > 0) {
                char ack_pl_hex[TX_PAYLOAD_SIZE_HEX];
                char ack_pl_ascii[TX_PAYLOAD_SIZE_ASCII];
                hexlify(status->ack_payload, status->ack_payload_size, ack_pl_hex);
                buffer_to_ascii(status->ack_payload, status->ack_payload_size, ack_pl_ascii);
                lines[line_index++] = "ACK payload (HEX):";
                lines[line_index++] = ack_pl_hex + status->scroll_hori;
                lines[line_index++] = "ACK payload (ASCII):";
                lines[line_index++] = (status->scroll_hori < TX_PAYLOAD_SIZE_ASCII) ?
                                          ack_pl_ascii + status->scroll_hori :
                                          ack_pl_ascii + TX_PAYLOAD_SIZE_ASCII;
            } else {
                lines[line_index++] = "No ack payload received...";
            }
        }
    }
    if(line_index <= TX_MAX_DISPLAY_LINES) {
        status->scroll_vert = 0;
    } else if(status->scroll_vert > (line_index - TX_MAX_DISPLAY_LINES)) {
        status->scroll_vert = line_index - TX_MAX_DISPLAY_LINES;
    }

    // draw lines
    uint8_t y = start_y;
    for(uint8_t i = status->scroll_vert;
        i < MIN(line_index, status->scroll_vert + TX_MAX_DISPLAY_LINES);
        i++) {
        canvas_draw_str(canvas, start_x, y, lines[i]);
        y += TX_LINE_HEIGHT;
    }
}

static bool run_input_callback(InputEvent* event, void* context) {
    Nrf24Tool* app = (Nrf24Tool*)context;
    TxStatus* status = (TxStatus*)view_get_model(app->tx_run);

    if(event->type != InputTypePress && event->type != InputTypeRepeat) return false;

    switch(event->key) {
    case InputKeyBack:
        if(app->tool_running) {
            stop_tool_thread(app);
        } else {
            view_dispatcher_switch_to_view(app->view_dispatcher, VIEW_TX_SETTINGS);
        }
        break;

    case InputKeyOk:
        if(!app->tool_running) {
            if(!nrf24_test_connection(app, VIEW_TX_RUN, VIEW_TX_RUN)) {
                return false;
            }
            start_tool_thread(app);
        }
        break;

    case InputKeyUp:
        if(status->scroll_vert > 0) {
            status->scroll_vert--;
        }
        break;
    case InputKeyDown:
        status->scroll_vert++;
        break;
    case InputKeyLeft:
        if(status->scroll_hori > 0) {
            status->scroll_hori--;
        }
        break;
    case InputKeyRight:
        if(status->scroll_hori < TX_MAX_DISPLAY_CHAR) {
            status->scroll_hori++;
        }
        break;

    default:
        break;
    }
    view_commit_model(app->tx_run, true);
    return true;
}

static void launch_run_view(Nrf24Tool* app) {
    TxStatus* status = (TxStatus*)view_get_model(app->tx_run);
    status->nrf24_config = &tx_nrf24_config;
    status->send_ok = false;
    status->send_failed = false;
    status->scroll_hori = 0;
    status->scroll_vert = 0;
    view_commit_model(app->tx_run, true);
    nrf24_test_connection(app, VIEW_TX_SETTINGS, VIEW_TX_RUN);
}

static void payload_browser_callback(void* context) {
    Nrf24Tool* app = (Nrf24Tool*)context;
    tx_payload_type = TX_PAYLOAD_TYPE_FILE;
    launch_run_view(app);
}

static uint32_t payload_browser_exit_callback(void* context) {
    UNUSED(context);
    return VIEW_TX_SETTINGS;
}

static void payload_hex_input_callback(void* context) {
    Nrf24Tool* app = (Nrf24Tool*)context;
    tx_payload_type = TX_PAYLOAD_TYPE_HEX;
    launch_run_view(app);
}

static void payload_text_input_callback(void* context) {
    Nrf24Tool* app = (Nrf24Tool*)context;
    tx_payload_type = TX_PAYLOAD_TYPE_ASCII;
    launch_run_view(app);
}

static void payload_type_select_result_callback(DialogExResult result, void* context) {
    Nrf24Tool* app = (Nrf24Tool*)context;

    if(result == DialogExResultLeft) {
        view_dispatcher_switch_to_view(app->view_dispatcher, VIEW_TX_PAYLOAD_ASCII_INPUT);
    } else if(result == DialogExResultRight) {
        view_dispatcher_switch_to_view(app->view_dispatcher, VIEW_TX_PAYLOAD_HEX_INPUT);
    } else if(result == DialogExResultCenter) {
        view_dispatcher_switch_to_view(app->view_dispatcher, VIEW_TX_SETTINGS);
    }
}

static void set_item_list_name(Setting* setting, VariableItem* item) {
    Nrf24Tool* app = (Nrf24Tool*)variable_item_get_context(item);
    char buffer[32];

    setting_value_to_string(setting, buffer, sizeof(buffer));
    if(setting->type == SETTING_TYPE_ADDR) {
        uint8_t addr_lenght = app->settings->tx_settings[TX_SETTING_ADDR_WIDTH].value.a_w;
        buffer[addr_lenght * 2] = '\0';
    }
    // special case if send count == 0 => infinite send
    if(setting == &app->settings->tx_settings[TX_SETTING_SEND_COUNT]) {
        if(setting->value.u8 == 0) {
            strcpy(buffer, "INF.");
        }
    }
    variable_item_set_current_value_text(item, buffer);
}

static void byteInputCallback(void* context) {
    Nrf24Tool* app = (Nrf24Tool*)context;
    view_dispatcher_switch_to_view(app->view_dispatcher, VIEW_TX_SETTINGS);
}

static void byteChangedCallback(void* context) {
    Nrf24Tool* app = (Nrf24Tool*)context;
    Setting* setting = &app->settings->tx_settings[bytes_input_current_addr_index];
    memcpy(setting->value.addr, bytes_input, bytes_input_buffer_size);
    set_item_list_name(setting, bytes_input_item);
}

static void item_change_callback(VariableItem* item) {
    Nrf24Tool* app = (Nrf24Tool*)variable_item_get_context(item);

    uint8_t current_setting = variable_item_list_get_selected_item_index(app->tx_settings);
    uint8_t current_index = variable_item_get_current_value_index(item);

    if(current_setting >= TX_SETTING_COUNT) return;
    Setting* setting = &app->settings->tx_settings[current_setting];

    set_setting_value(setting, (current_index * setting->step) + setting->min);
    set_item_list_name(setting, item);
}

static void item_addr_change_callback(VariableItem* item) {
    Nrf24Tool* app = (Nrf24Tool*)variable_item_get_context(item);
    bytes_input_buffer_size = app->settings->tx_settings[TX_SETTING_ADDR_WIDTH].value.a_w;

    bytes_input_current_addr_index = variable_item_list_get_selected_item_index(app->tx_settings);
    Setting* setting = &app->settings->tx_settings[bytes_input_current_addr_index];

    memcpy(bytes_input, setting->value.addr, bytes_input_buffer_size);
    bytes_input_item = item;

    variable_item_set_current_value_index(bytes_input_item, 1);

    byte_input_set_header_text(app->tx_addr_input, setting->name);
    byte_input_set_result_callback(
        app->tx_addr_input,
        byteInputCallback,
        byteChangedCallback,
        app,
        bytes_input,
        bytes_input_buffer_size);
    view_dispatcher_switch_to_view(app->view_dispatcher, VIEW_TX_ADDR_INPUT);
}

static void settings_enter_callback(void* context, uint32_t index) {
    UNUSED(index);
    Nrf24Tool* app = (Nrf24Tool*)context;

    if(app->settings->tx_settings[TX_SETTING_FROM_FILE].value.b) {
        nrf24_test_connection(app, VIEW_TX_SETTINGS, VIEW_TX_BROWSE_FILE);
    } else {
        nrf24_test_connection(app, VIEW_TX_SETTINGS, VIEW_TX_PAYLOAD_TYPE_SELECT);
    }
}

static uint32_t settings_exit_callback(void* context) {
    UNUSED(context);
    return VIEW_MENU;
}

void tx_alloc(Nrf24Tool* app) {
    app->tx_settings = variable_item_list_alloc();
    for(uint8_t i = 0; i < TX_SETTING_COUNT; i++) {
        Setting* setting = &app->settings->tx_settings[i];
        if(setting->type != SETTING_TYPE_ADDR) {
            tx_item[i] = variable_item_list_add(
                app->tx_settings, setting->name, MAX_SETTINGS(setting), item_change_callback, app);
            variable_item_set_current_value_index(tx_item[i], get_setting_index(setting));
        } else {
            tx_item[i] = variable_item_list_add(
                app->tx_settings, setting->name, 3, item_addr_change_callback, app);
            variable_item_set_current_value_index(tx_item[i], 1);
        }
        set_item_list_name(setting, tx_item[i]);
    }
    variable_item_list_set_enter_callback(app->tx_settings, settings_enter_callback, app);
    View* view = variable_item_list_get_view(app->tx_settings);
    view_set_previous_callback(view, settings_exit_callback);
    view_dispatcher_add_view(app->view_dispatcher, VIEW_TX_SETTINGS, view);

    // TX address input
    app->tx_addr_input = byte_input_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, VIEW_TX_ADDR_INPUT, byte_input_get_view(app->tx_addr_input));

    // Payload type select (ASCII or HEX)
    app->tx_payload_type_select = dialog_ex_alloc();
    dialog_ex_set_context(app->tx_payload_type_select, app);
    dialog_ex_set_result_callback(
        app->tx_payload_type_select, payload_type_select_result_callback);
    dialog_ex_set_text(
        app->tx_payload_type_select, "Select input format", 64, 25, AlignCenter, AlignCenter);
    dialog_ex_set_left_button_text(app->tx_payload_type_select, "ASCII");
    dialog_ex_set_right_button_text(app->tx_payload_type_select, "HEX");
    dialog_ex_set_center_button_text(app->tx_payload_type_select, "BACK");
    view_dispatcher_add_view(
        app->view_dispatcher,
        VIEW_TX_PAYLOAD_TYPE_SELECT,
        dialog_ex_get_view(app->tx_payload_type_select));

    // Payload text input view
    app->tx_payload_text_input = text_input_alloc();
    text_input_set_header_text(app->tx_payload_text_input, "Payload text input");
    text_input_set_result_callback(
        app->tx_payload_text_input,
        payload_text_input_callback,
        app,
        tx_payload_ascii,
        TX_PAYLOAD_SIZE_ASCII,
        true);
    view_dispatcher_add_view(
        app->view_dispatcher,
        VIEW_TX_PAYLOAD_ASCII_INPUT,
        text_input_get_view(app->tx_payload_text_input));

    // Payload hex input view
    app->tx_payload_hex_input = byte_input_alloc();
    byte_input_set_header_text(app->tx_payload_hex_input, "Payload HEX input");
    byte_input_set_result_callback(
        app->tx_payload_hex_input,
        payload_hex_input_callback,
        NULL,
        app,
        tx_payload_hex,
        MAX_PAYLOAD_SIZE);
    view_dispatcher_add_view(
        app->view_dispatcher,
        VIEW_TX_PAYLOAD_HEX_INPUT,
        byte_input_get_view(app->tx_payload_hex_input));

    // Payload file input view (browser)
    tx_payload_path = furi_string_alloc_set_str(APP_DATA_PATH());
    app->tx_payload_browser = file_browser_alloc(tx_payload_path);
    file_browser_configure(app->tx_payload_browser, TX_PAYLOAD_EXT, NULL, true, true, NULL, false);
    file_browser_start(app->tx_payload_browser, tx_payload_path);
    file_browser_set_callback(app->tx_payload_browser, payload_browser_callback, app);
    view_set_previous_callback(
        file_browser_get_view(app->tx_payload_browser), payload_browser_exit_callback);
    view_dispatcher_add_view(
        app->view_dispatcher, VIEW_TX_BROWSE_FILE, file_browser_get_view(app->tx_payload_browser));

    // RUN view
    app->tx_run = view_alloc();
    view_set_context(app->tx_run, app);
    view_allocate_model(app->tx_run, ViewModelTypeLockFree, sizeof(TxStatus));
    view_set_draw_callback(app->tx_run, run_draw_callback);
    view_set_input_callback(app->tx_run, run_input_callback);
    view_dispatcher_add_view(app->view_dispatcher, VIEW_TX_RUN, app->tx_run);
}

void tx_free(Nrf24Tool* app) {
    view_dispatcher_remove_view(app->view_dispatcher, VIEW_TX_SETTINGS);
    view_dispatcher_remove_view(app->view_dispatcher, VIEW_TX_ADDR_INPUT);
    view_dispatcher_remove_view(app->view_dispatcher, VIEW_TX_RUN);
    view_dispatcher_remove_view(app->view_dispatcher, VIEW_TX_PAYLOAD_TYPE_SELECT);
    view_dispatcher_remove_view(app->view_dispatcher, VIEW_TX_PAYLOAD_ASCII_INPUT);
    view_dispatcher_remove_view(app->view_dispatcher, VIEW_TX_PAYLOAD_HEX_INPUT);
    view_dispatcher_remove_view(app->view_dispatcher, VIEW_TX_BROWSE_FILE);
    variable_item_list_free(app->tx_settings);
    byte_input_free(app->tx_addr_input);
    view_free(app->tx_run);
    dialog_ex_free(app->tx_payload_type_select);
    text_input_free(app->tx_payload_text_input);
    byte_input_free(app->tx_payload_hex_input);
    furi_string_free(tx_payload_path);
    file_browser_stop(app->tx_payload_browser);
    file_browser_free(app->tx_payload_browser);
}
