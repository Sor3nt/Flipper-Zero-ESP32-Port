#include "sniff.h"
#include "../helper.h"

static uint8_t confirmed_idx_draw = 0;
static const uint8_t start_x = 2;
static const uint8_t start_y = 14;
static const uint8_t step = 13;
VariableItem* sniff_item[SNIFF_SETTING_COUNT];

static void start_tool_thread(Nrf24Tool* app) {
    app->tool_running = true;
    app->tool_thread = furi_thread_alloc_ex("Sniff", 8192, nrf24_sniff, app);
    furi_thread_set_state_context(app->tool_thread, app);
    furi_thread_set_state_callback(app->tool_thread, sniff_thread_state_callback);
    furi_thread_set_priority(app->tool_thread, FuriThreadPriorityNormal);
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
    SniffStatus* status = (SniffStatus*)model;

    //const size_t middle_x = canvas_width(canvas) / 2U;
    canvas_set_color(canvas, ColorBlack);

    // draw title
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 0, 0, AlignLeft, AlignTop, "Mode : Sniffing");
    canvas_draw_line(canvas, 0, 10, canvas_width(canvas), 10);

    // draw status
    canvas_set_font(canvas, FontSecondary);
    // application status
    uint8_t line_num = 0;
    if(status->thread_state == FuriThreadStateRunning)
        canvas_draw_str_aligned(
            canvas, start_x, start_y + line_num * step, AlignLeft, AlignTop, "Status : RUN");
    else
        canvas_draw_str_aligned(
            canvas, start_x, start_y + line_num * step, AlignLeft, AlignTop, "Status : IDLE");
    // current channel
    line_num++;
    char status_str[32];
    if(status->current_channel != 0)
        snprintf(status_str, sizeof(status_str), "Scanning channel : %u", status->current_channel);
    else
        snprintf(status_str, sizeof(status_str), "Testing ADDR : %s", status->tested_addr);
    canvas_draw_str_aligned(
        canvas, start_x, start_y + line_num * step, AlignLeft, AlignTop, status_str);
    // find address
    line_num++;
    if(confirmed_idx == 0) {
        strcpy(status_str, "Address found : NONE");
        confirmed_idx_draw = 0;
    } else {
        char hex_addr[11];
        hexlify(confirmed[confirmed_idx_draw], MAX_MAC_SIZE, hex_addr);
        snprintf(status_str, sizeof(status_str), "Address found : %s", hex_addr);
    }
    canvas_draw_str_aligned(
        canvas, start_x, start_y + line_num * step, AlignLeft, AlignTop, status_str);
    line_num++;
    // counters
    snprintf(
        status_str,
        sizeof(status_str),
        "Found : %u    |    New : %u",
        status->addr_find_count,
        status->addr_new_count);
    canvas_draw_str_aligned(
        canvas, start_x, start_y + line_num * step, AlignLeft, AlignTop, status_str);
}

static bool run_input_callback(InputEvent* event, void* context) {
    Nrf24Tool* app = (Nrf24Tool*)context;

    if(event->type != InputTypePress) return false;

    switch(event->key) {
    case InputKeyLeft:
        if(confirmed_idx_draw > 0) confirmed_idx_draw--;
        break;

    case InputKeyRight:
        if(confirmed_idx_draw < (confirmed_idx - 1)) confirmed_idx_draw++;
        break;

    case InputKeyOk:
        if(!app->tool_running) {
            if(!nrf24_test_connection(app, VIEW_SNIFF_RUN, VIEW_SNIFF_RUN)) {
                return false;
            }
            start_tool_thread(app);
        }
        break;

    case InputKeyBack:
        if(app->tool_running) {
            stop_tool_thread(app);
        } else {
            view_dispatcher_switch_to_view(app->view_dispatcher, VIEW_SNIFF_SETTINGS);
        }
        break;

    default:
        break;
    }
    if(confirmed_idx_draw > (confirmed_idx - 1)) confirmed_idx_draw = (confirmed_idx - 1);
    view_commit_model(app->sniff_run, true);
    return true;
}

static void set_item_list_name(Setting* setting, VariableItem* item) {
    char buffer[32];

    setting_value_to_string(setting, buffer, sizeof(buffer));
    variable_item_set_current_value_text(item, buffer);
}

static void item_change_callback(VariableItem* item) {
    Nrf24Tool* app = (Nrf24Tool*)variable_item_get_context(item);

    uint8_t current_setting = variable_item_list_get_selected_item_index(app->sniff_settings);
    uint8_t current_index = variable_item_get_current_value_index(item);

    if(current_setting >= SNIFF_SETTING_COUNT) return;
    Setting* setting = &app->settings->sniff_settings[current_setting];
    set_setting_value(setting, (current_index * setting->step) + setting->min);

    set_item_list_name(setting, item);
}

static void settings_enter_callback(void* context, uint32_t index) {
    UNUSED(index);
    Nrf24Tool* app = (Nrf24Tool*)context;
    nrf24_test_connection(app, VIEW_SNIFF_SETTINGS, VIEW_SNIFF_RUN);
    //view_dispatcher_switch_to_view(app->view_dispatcher, VIEW_SNIFF_RUN);
}

static uint32_t settings_exit_callback(void* context) {
    UNUSED(context);
    return VIEW_MENU;
}

void sniff_alloc(Nrf24Tool* app) {
    app->sniff_settings = variable_item_list_alloc();
    //variable_item_list_set_header(app->sniff_settings, "Sniffing Settings");
    for(uint8_t i = 0; i < SNIFF_SETTING_COUNT; i++) {
        Setting* setting = &app->settings->sniff_settings[i];
        sniff_item[i] = variable_item_list_add(
            app->sniff_settings, setting->name, MAX_SETTINGS(setting), item_change_callback, app);
        variable_item_set_current_value_index(sniff_item[i], get_setting_index(setting));
        set_item_list_name(setting, sniff_item[i]);
    }
    variable_item_list_set_enter_callback(app->sniff_settings, settings_enter_callback, app);
    View* view = variable_item_list_get_view(app->sniff_settings);
    view_set_previous_callback(view, settings_exit_callback);
    view_dispatcher_add_view(app->view_dispatcher, VIEW_SNIFF_SETTINGS, view);

    app->sniff_run = view_alloc();
    view_set_context(app->sniff_run, app);
    view_set_draw_callback(app->sniff_run, run_draw_callback);
    view_set_input_callback(app->sniff_run, run_input_callback);
    view_allocate_model(app->sniff_run, ViewModelTypeLockFree, sizeof(SniffStatus));
    view_dispatcher_add_view(app->view_dispatcher, VIEW_SNIFF_RUN, app->sniff_run);
}

void sniff_free(Nrf24Tool* app) {
    view_dispatcher_remove_view(app->view_dispatcher, VIEW_SNIFF_SETTINGS);
    view_dispatcher_remove_view(app->view_dispatcher, VIEW_SNIFF_RUN);
    variable_item_list_free(app->sniff_settings);

    view_free(app->sniff_run);
}
