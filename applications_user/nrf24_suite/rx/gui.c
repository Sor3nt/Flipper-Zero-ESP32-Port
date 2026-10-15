#include "rx.h"
#include "../helper.h"

VariableItem* rx_item[RX_SETTING_COUNT];
static uint8_t bytes_input[BYTE_MAC_LEN];
static uint8_t bytes_input_buffer_size;
static uint8_t bytes_input_current_addr_index;
static VariableItem* bytes_input_item;
static int32_t start_x = 0;
static uint32_t start_y = RX_START_Y;
static int8_t draw_index = 0;

static void start_tool_thread(Nrf24Tool* app) {
    app->tool_running = true;
    app->tool_thread = furi_thread_alloc_ex("RX", 8192, nrf24_rx, app);
    furi_thread_set_state_context(app->tool_thread, app);
    furi_thread_set_state_callback(app->tool_thread, rx_thread_state_callback);
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
    RxStatus* status = (RxStatus*)model;
    char disp_str[96];

    // application status
    canvas_set_font(canvas, FontSecondary);
    if(status->thread_state == FuriThreadStateRunning) {
        canvas_draw_str_aligned(canvas, 0, 0, AlignLeft, AlignTop, "Status : RUN");
    } else {
        canvas_draw_str_aligned(canvas, 0, 0, AlignLeft, AlignTop, "Status : IDLE");
    }
    snprintf(disp_str, sizeof(disp_str), "Channel : %u", status->nrf24_config->channel);
    canvas_draw_str_aligned(canvas, canvas_width(canvas), 0, AlignRight, AlignTop, disp_str);

    // rx data
    canvas_set_custom_u8g2_font(canvas, u8g2_font_5x7_tr);
    uint8_t buffer_size = status->rx_buffer->fill_level;
    uint8_t last_draw_line = 0;
    if(buffer_size < RX_DISPLAY_LINES) {
        draw_index = 0;
        last_draw_line = buffer_size;
    } else {
        if(draw_index > (buffer_size - RX_DISPLAY_LINES)) {
            draw_index = buffer_size - RX_DISPLAY_LINES;
        }
        last_draw_line = draw_index + RX_DISPLAY_LINES;
    }

    if(buffer_size > 0) {
        uint8_t num_draw_line = 0;
        char* disp_ptr;
        for(uint8_t i = draw_index; i < last_draw_line; i++) {
            disp_ptr = (status->display_ascii) ? status->rx_buffer->data_ascii[i] :
                                                 status->rx_buffer->data_hex[i];
            snprintf(
                disp_str,
                sizeof(disp_str),
                "%02u: P%u %s",
                i,
                status->rx_buffer->from_pipe[i],
                disp_ptr);
            canvas_draw_str(
                canvas, start_x, start_y + (num_draw_line * RX_DISPLAY_STEP), disp_str);
            num_draw_line++;
        }
    } else {
        canvas_draw_str(canvas, 0, start_y, "NO DATA RECEIVED");
    }
}

static bool run_input_callback(InputEvent* event, void* context) {
    Nrf24Tool* app = (Nrf24Tool*)context;
    RxStatus* status = (RxStatus*)view_get_model(app->rx_run);

    if(event->type != InputTypePress && event->type != InputTypeRepeat) return false;

    switch(event->key) {
    case InputKeyBack:
        if(app->tool_running) {
            stop_tool_thread(app);
        } else {
            view_dispatcher_switch_to_view(app->view_dispatcher, VIEW_RX_SETTINGS);
        }
        break;

    case InputKeyOk:
        if(!app->tool_running) {
            if(!nrf24_test_connection(app, VIEW_RX_RUN, VIEW_RX_RUN)) {
                return false;
            }
            start_tool_thread(app);
        } else {
            status->display_ascii = !status->display_ascii;
            if(start_x < -70 && status->display_ascii) start_x = -70;
            view_commit_model(app->rx_run, true);
        }
        break;

    case InputKeyUp:
        if(draw_index > 0) draw_index--;
        break;
    case InputKeyDown:
        draw_index++;
        break;
    case InputKeyLeft:
        start_x += 5;
        if(start_x > 0) start_x = 0;
        break;
    case InputKeyRight:
        start_x -= 5;
        if(start_x < -230 && !status->display_ascii) start_x = -230;
        if(start_x < -70 && status->display_ascii) start_x = -70;
        break;

    default:
        break;
    }
    view_commit_model(app->rx_run, true);
    return true;
}

static void set_item_list_name(Setting* setting, VariableItem* item) {
    Nrf24Tool* app = (Nrf24Tool*)variable_item_get_context(item);
    char buffer[32];

    setting_value_to_string(setting, buffer, sizeof(buffer));
    if(setting->type == SETTING_TYPE_ADDR) {
        uint8_t addr_lenght = app->settings->rx_settings[RX_SETTING_ADDR_WIDTH].value.a_w;
        buffer[addr_lenght * 2] = '\0';
    }
    variable_item_set_current_value_text(item, buffer);
}

static void byteInputCallback(void* context) {
    Nrf24Tool* app = (Nrf24Tool*)context;
    view_dispatcher_switch_to_view(app->view_dispatcher, VIEW_RX_SETTINGS);
}

static void byteChangedCallback(void* context) {
    Nrf24Tool* app = (Nrf24Tool*)context;
    Setting* setting = &app->settings->rx_settings[bytes_input_current_addr_index];
    memcpy(setting->value.addr, bytes_input, bytes_input_buffer_size);
    set_item_list_name(setting, bytes_input_item);
}

static void item_change_callback(VariableItem* item) {
    Nrf24Tool* app = (Nrf24Tool*)variable_item_get_context(item);

    uint8_t current_setting = variable_item_list_get_selected_item_index(app->rx_settings);
    uint8_t current_index = variable_item_get_current_value_index(item);

    if(current_setting >= RX_SETTING_COUNT) return;
    Setting* setting = &app->settings->rx_settings[current_setting];

    set_setting_value(setting, (current_index * setting->step) + setting->min);
    set_item_list_name(setting, item);
}

static void item_addr_change_callback(VariableItem* item) {
    Nrf24Tool* app = (Nrf24Tool*)variable_item_get_context(item);
    uint8_t addr_lenght = app->settings->rx_settings[RX_SETTING_ADDR_WIDTH].value.a_w;

    bytes_input_current_addr_index = variable_item_list_get_selected_item_index(app->rx_settings);
    Setting* setting = &app->settings->rx_settings[bytes_input_current_addr_index];

    bytes_input_buffer_size = (setting->type == SETTING_TYPE_ADDR) ? addr_lenght :
                                                                     BYTE_MAC_LEN_1BYTE;
    memcpy(bytes_input, setting->value.addr, bytes_input_buffer_size);
    bytes_input_item = item;

    variable_item_set_current_value_index(bytes_input_item, 1);

    byte_input_set_header_text(app->rx_addr_input, setting->name);
    byte_input_set_result_callback(
        app->rx_addr_input,
        byteInputCallback,
        byteChangedCallback,
        app,
        bytes_input,
        bytes_input_buffer_size);
    view_dispatcher_switch_to_view(app->view_dispatcher, VIEW_RX_ADDR_INPUT);
}

static void settings_enter_callback(void* context, uint32_t index) {
    UNUSED(index);
    Nrf24Tool* app = (Nrf24Tool*)context;

    with_view_model(app->rx_run, RxStatus * status, status->nrf24_config = &rx_nrf24_config;
                    status->rx_buffer = &rx_buffer;
                    start_x = 0;
                    start_y = RX_START_Y;
                    draw_index = 0;
                    , true);

    nrf24_test_connection(app, VIEW_RX_SETTINGS, VIEW_RX_RUN);
    //view_dispatcher_switch_to_view(app->view_dispatcher, VIEW_SNIFF_RUN);
}

static uint32_t settings_exit_callback(void* context) {
    UNUSED(context);
    return VIEW_MENU;
}

void rx_alloc(Nrf24Tool* app) {
    app->rx_settings = variable_item_list_alloc();
    //variable_item_list_set_header(app->rx_settings, "RX Settings");
    for(uint8_t i = 0; i < RX_SETTING_COUNT; i++) {
        Setting* setting = &app->settings->rx_settings[i];
        if(setting->type != SETTING_TYPE_ADDR && setting->type != SETTING_TYPE_ADDR_1BYTE) {
            rx_item[i] = variable_item_list_add(
                app->rx_settings, setting->name, MAX_SETTINGS(setting), item_change_callback, app);
            variable_item_set_current_value_index(rx_item[i], get_setting_index(setting));
        } else {
            rx_item[i] = variable_item_list_add(
                app->rx_settings, setting->name, 3, item_addr_change_callback, app);
            variable_item_set_current_value_index(rx_item[i], 1);
        }
        set_item_list_name(setting, rx_item[i]);
    }
    variable_item_list_set_enter_callback(app->rx_settings, settings_enter_callback, app);
    View* view = variable_item_list_get_view(app->rx_settings);
    view_set_previous_callback(view, settings_exit_callback);
    view_dispatcher_add_view(app->view_dispatcher, VIEW_RX_SETTINGS, view);

    app->rx_addr_input = byte_input_alloc();
    //byte_input_set_result_callback(app->rx_addr_input, byteInputCallback, byteChangedCallback, app, bytes_input, BYTE_MAC_LEN);
    view_dispatcher_add_view(
        app->view_dispatcher, VIEW_RX_ADDR_INPUT, byte_input_get_view(app->rx_addr_input));

    // RUN view
    app->rx_run = view_alloc();
    view_set_context(app->rx_run, app);
    view_allocate_model(app->rx_run, ViewModelTypeLockFree, sizeof(RxStatus));
    view_set_draw_callback(app->rx_run, run_draw_callback);
    view_set_input_callback(app->rx_run, run_input_callback);
    view_dispatcher_add_view(app->view_dispatcher, VIEW_RX_RUN, app->rx_run);
}

void rx_free(Nrf24Tool* app) {
    view_dispatcher_remove_view(app->view_dispatcher, VIEW_RX_SETTINGS);
    view_dispatcher_remove_view(app->view_dispatcher, VIEW_RX_ADDR_INPUT);
    view_dispatcher_remove_view(app->view_dispatcher, VIEW_RX_RUN);
    variable_item_list_free(app->rx_settings);
    byte_input_free(app->rx_addr_input);
    view_free(app->rx_run);
}
