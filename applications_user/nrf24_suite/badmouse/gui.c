#include "badmouse.h"
#include "../helper.h"

#include <gui/elements.h>

static const char* FILE_PATH_ADDR = APP_DATA_PATH("addresses.txt");
uint8_t addr_qty = 0;
uint8_t layout_qty = 0;
char addr_list[MAX_CONFIRMED_ADDR][HEX_MAC_LEN + 1];
char layout_list[MAX_KB_LAYOUT][LAYOUT_NAME_LENGHT];
VariableItem* bm_item[BADMOUSE_SETTING_COUNT];
FuriString* bm_payload_path;
static const uint8_t start_x = 0;
static const uint8_t start_y = 0;
static const uint8_t text_box_width = 128;

static void start_tool_thread(Nrf24Tool* app) {
    app->tool_thread = furi_thread_alloc_ex("BadMouse", 8192, nrf24_badmouse, app);
    furi_thread_set_state_context(app->tool_thread, app);
    furi_thread_set_state_callback(app->tool_thread, bm_thread_state_callback);
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
    BadMouseStatus* status = (BadMouseStatus*)model;

    char disp_char[256] = {0};
    size_t disp_char_size = sizeof(disp_char);
    char* ptr = disp_char;

    FuriString* temp_str = furi_string_alloc_set_str(status->file_name);
    uint8_t text_box_height = 52;

    elements_string_fit_width(canvas, temp_str, text_box_width);
    ptr += snprintf(
        ptr,
        disp_char_size - (ptr - disp_char),
        "%.31s\n\e#Layout:\e# \e*(%.31s)\e*\n",
        furi_string_get_cstr(temp_str),
        (strlen(status->layout) == 0) ? "default" : status->layout);
    furi_string_free(temp_str);

    ptr += snprintf(ptr, disp_char_size - (ptr - disp_char), "\e#Status: \e#");

    char run_string[] = "\e!RUN\e!  \e*(%u/%u)\e*\n";

    switch(status->execState) {
    case BadMouseState_Idle:
        ptr += snprintf(ptr, disp_char_size - (ptr - disp_char), "\e!IDLE\e!\n");
        break;

    case BadMouseState_Init:
        ptr += snprintf(ptr, disp_char_size - (ptr - disp_char), "\e!INIT\e!\n");
        break;

    case BadMouseState_Run:
        ptr += snprintf(
            ptr,
            disp_char_size - (ptr - disp_char),
            run_string,
            status->state.line_cur,
            status->state.line_nb);
        ptr += snprintf(
            ptr, disp_char_size - (ptr - disp_char), "\e*%.42s\e*", status->state.line_cur_str);
        break;

    case BadMouseState_StringPrint:
        ptr += snprintf(
            ptr,
            disp_char_size - (ptr - disp_char),
            run_string,
            status->state.line_cur,
            status->state.line_nb);
        ptr += snprintf(
            ptr, disp_char_size - (ptr - disp_char), "\e*%.42s\e*", status->state.line_cur_str);
        break;

    case BadMouseState_AltStringPrint:
        ptr += snprintf(
            ptr,
            disp_char_size - (ptr - disp_char),
            run_string,
            status->state.line_cur,
            status->state.line_nb);
        ptr += snprintf(
            ptr, disp_char_size - (ptr - disp_char), "\e*%.42s\e*", status->state.line_cur_str);
        break;

    case BadMouseState_WaitForBtn:
        ptr += snprintf(
            ptr,
            disp_char_size - (ptr - disp_char),
            run_string,
            status->state.line_cur,
            status->state.line_nb);
        ptr += snprintf(
            ptr, disp_char_size - (ptr - disp_char), "\e*%.42s\e*", status->state.line_cur_str);
        break;

    case BadMouseState_Done:
        ptr += snprintf(ptr, disp_char_size - (ptr - disp_char), "\e!DONE\e!");
        break;

    case BadMouseState_Error:
        ptr += snprintf(
            ptr,
            disp_char_size - (ptr - disp_char),
            "\e!ERROR\e! \e*line: %u\e*\n",
            status->state.error_line);
        ptr +=
            snprintf(ptr, disp_char_size - (ptr - disp_char), "\e*%.42s\e*", status->state.error);
        break;
    }

    if((status->execState == BadMouseState_Idle || status->execState == BadMouseState_Done) &&
       status->thread_state == FuriThreadStateStopped) {
        elements_button_center(canvas, "Start");
    } else if(
        status->execState != BadMouseState_WaitForBtn &&
        status->execState != BadMouseState_Error &&
        status->thread_state == FuriThreadStateRunning) {
        elements_button_center(canvas, "Stop");
    } else {
        text_box_height = 64;
    }

    elements_text_box(
        canvas,
        start_x,
        start_y,
        text_box_width,
        text_box_height,
        AlignLeft,
        AlignTop,
        disp_char,
        false);
}

static bool run_input_callback(InputEvent* event, void* context) {
    Nrf24Tool* app = (Nrf24Tool*)context;
    BadMouseStatus* view_status = view_get_model(app->badmouse_run);

    if(event->type != InputTypePress) return false;

    switch(event->key) {
    case InputKeyBack:
        if(!app->tool_running) {
            if(view_status->execState != BadMouseState_Error) {
                view_dispatcher_switch_to_view(app->view_dispatcher, VIEW_BM_SETTINGS);
            } else {
                view_status->execState = BadMouseState_Idle;
                view_commit_model(app->badmouse_run, true);
            }
        } else {
            stop_tool_thread(app);
            app->bm_instance->execState = BadMouseState_Idle;
            view_status->execState = app->bm_instance->execState;
            view_commit_model(app->badmouse_run, true);
        }
        break;

    case InputKeyOk:
        if(!app->tool_running) {
            if(view_status->execState != BadMouseState_Error) {
                if(!nrf24_test_connection(app, VIEW_BM_RUN, VIEW_BM_RUN)) {
                    return false;
                }
                app->tool_running = true;
                start_tool_thread(app);
            }
        } else {
            if(app->bm_instance->execState == BadMouseState_WaitForBtn) {
                app->bm_instance->execState = BadMouseState_Run;
            } else {
                stop_tool_thread(app);
                app->bm_instance->execState = BadMouseState_Idle;
                view_status->execState = app->bm_instance->execState;
                view_commit_model(app->badmouse_run, true);
            }
        }
        break;

    case InputKeyUp:
    case InputKeyDown:
    case InputKeyLeft:
    case InputKeyRight:
        if(app->tool_running) {
            if(app->bm_instance->execState == BadMouseState_WaitForBtn) {
                app->bm_instance->execState = BadMouseState_Run;
            }
        }
        break;

    default:
        break;
    }
    return true;
}

static void set_item_list_name(Setting* setting, VariableItem* item) {
    char buffer[64] = {0};

    if(item == bm_item[BADMOUSE_SETTING_ADDR_INDEX]) {
        if(addr_qty > 0) {
            strcpy(buffer, addr_list[setting->value.u8]);
        } else {
            strcpy(buffer, "NO ADDR");
        }
    } else if(item == bm_item[BADMOUSE_SETTING_KB_LAYOUT]) {
        if(layout_qty > 0) {
            strcpy(buffer, layout_list[setting->value.u8]);
        } else {
            strcpy(buffer, "NO LAYOUT");
        }
    } else {
        setting_value_to_string(setting, buffer, sizeof(buffer));
    }
    variable_item_set_current_value_text(item, buffer);
}

static void item_change_callback(VariableItem* item) {
    Nrf24Tool* app = (Nrf24Tool*)variable_item_get_context(item);

    uint8_t current_setting = variable_item_list_get_selected_item_index(app->badmouse_settings);
    uint8_t current_index = variable_item_get_current_value_index(item);

    if(current_setting >= BADMOUSE_SETTING_COUNT) return;
    Setting* setting = &app->settings->badmouse_settings[current_setting];
    set_setting_value(setting, (current_index * setting->step) + setting->min);

    set_item_list_name(setting, item);
}

static void settings_enter_callback(void* context, uint32_t index) {
    UNUSED(index);
    Nrf24Tool* app = (Nrf24Tool*)context;
    nrf24_test_connection(app, VIEW_BM_SETTINGS, VIEW_BM_BROWSE_FILE);
}

static uint32_t settings_exit_callback(void* context) {
    UNUSED(context);
    return VIEW_MENU;
}

static void no_channel_result_callback(DialogExResult result, void* context) {
    if(result != DialogExResultLeft) return;
    Nrf24Tool* app = (Nrf24Tool*)context;
    view_dispatcher_switch_to_view(app->view_dispatcher, VIEW_BM_RUN);
}

static void script_browser_callback(void* context) {
    Nrf24Tool* app = (Nrf24Tool*)context;
    char file_path[MAX_NAME_LEN];
    strncpy(file_path, furi_string_get_cstr(bm_payload_path), MAX_NAME_LEN - 1);
    char* file_name = strrchr(file_path, '/');
    if(file_name != NULL) {
        file_name++;
    } else {
        file_name = file_path;
    }
    with_view_model(
        app->badmouse_run,
        BadMouseStatus * status,
        strncpy(status->file_name, file_name, MAX_NAME_LEN - 1);
        strncpy(
            status->layout,
            layout_list[app->settings->badmouse_settings[BADMOUSE_SETTING_KB_LAYOUT].value.u8],
            MAX_LAYOUT_LEN - 1);
        status->execState = BadMouseState_Idle;
        , true);
    view_dispatcher_switch_to_view(app->view_dispatcher, VIEW_BM_RUN);
}

static uint32_t script_browser_exit_callback(void* context) {
    UNUSED(context);
    return VIEW_BM_SETTINGS;
}

void badmouse_alloc(Nrf24Tool* app) {
    // Settings view
    app->badmouse_settings = variable_item_list_alloc();
    //variable_item_list_set_header(app->badmouse_settings, "Badmouse Settings");
    for(uint8_t i = 0; i < BADMOUSE_SETTING_COUNT; i++) {
        Setting* setting = &app->settings->badmouse_settings[i];
        bm_item[i] = variable_item_list_add(
            app->badmouse_settings,
            setting->name,
            MAX_SETTINGS(setting),
            item_change_callback,
            app);
        variable_item_set_current_value_index(bm_item[i], get_setting_index(setting));
        set_item_list_name(setting, bm_item[i]);
    }
    variable_item_list_set_enter_callback(app->badmouse_settings, settings_enter_callback, app);
    View* view = variable_item_list_get_view(app->badmouse_settings);
    view_set_previous_callback(view, settings_exit_callback);
    view_dispatcher_add_view(app->view_dispatcher, VIEW_BM_SETTINGS, view);

    // No channel view
    app->bm_no_channel = dialog_ex_alloc();
    dialog_ex_set_context(app->bm_no_channel, app);
    dialog_ex_set_result_callback(app->bm_no_channel, no_channel_result_callback);
    dialog_ex_set_header(app->bm_no_channel, "ERROR", 64, 2, AlignCenter, AlignTop);
    dialog_ex_set_text(app->bm_no_channel, "No channel found", 64, 32, AlignCenter, AlignCenter);
    dialog_ex_set_left_button_text(app->bm_no_channel, "Return");
    view_dispatcher_add_view(
        app->view_dispatcher, VIEW_BM_NO_CHANNEL, dialog_ex_get_view(app->bm_no_channel));

    // Browse script file
    bm_payload_path = furi_string_alloc_set_str(BAD_USB_APP_BASE_FOLDER);
    app->bm_script_browser = file_browser_alloc(bm_payload_path);
    file_browser_configure(
        app->bm_script_browser, SCRIPT_EXTENSION, NULL, true, true, NULL, false);
    file_browser_start(app->bm_script_browser, bm_payload_path);
    //file_browser_set_item_callback(app->bm_script_browser, script_browser_item_callback, app);
    file_browser_set_callback(app->bm_script_browser, script_browser_callback, app);
    view_set_previous_callback(
        file_browser_get_view(app->bm_script_browser), script_browser_exit_callback);
    view_dispatcher_add_view(
        app->view_dispatcher, VIEW_BM_BROWSE_FILE, file_browser_get_view(app->bm_script_browser));

    // Bad mouse RUN view
    app->badmouse_run = view_alloc();
    view_set_context(app->badmouse_run, app);
    view_allocate_model(app->badmouse_run, ViewModelTypeLockFree, sizeof(BadMouseStatus));
    view_set_draw_callback(app->badmouse_run, run_draw_callback);
    view_set_input_callback(app->badmouse_run, run_input_callback);
    view_dispatcher_add_view(app->view_dispatcher, VIEW_BM_RUN, app->badmouse_run);
}

void badmouse_free(Nrf24Tool* app) {
    view_dispatcher_remove_view(app->view_dispatcher, VIEW_BM_SETTINGS);
    view_dispatcher_remove_view(app->view_dispatcher, VIEW_BM_RUN);
    view_dispatcher_remove_view(app->view_dispatcher, VIEW_BM_BROWSE_FILE);
    view_dispatcher_remove_view(app->view_dispatcher, VIEW_BM_NO_CHANNEL);

    variable_item_list_free(app->badmouse_settings);
    dialog_ex_free(app->bm_no_channel);
    file_browser_stop(app->bm_script_browser);
    file_browser_free(app->bm_script_browser);
    view_free(app->badmouse_run);

    furi_string_free(bm_payload_path);
}

uint8_t bm_read_address(Nrf24Tool* app) {
    addr_qty = 0;

    app->storage = furi_record_open(RECORD_STORAGE);
    Stream* stream = file_stream_alloc(app->storage);
    size_t file_size = 0;

    if(file_stream_open(stream, FILE_PATH_ADDR, FSAM_READ, FSOM_OPEN_EXISTING)) {
        file_size = stream_size(stream);
        stream_seek(stream, 0, StreamOffsetFromStart);

        if(file_size > 0) {
            FuriString* line = furi_string_alloc();
            // Lire le fichier ligne par ligne
            while(stream_read_line(stream, line)) {
                furi_string_trim(line);
                if(is_hex_address_furi(line) && addr_qty < (MAX_CONFIRMED_ADDR - 1)) {
                    strncpy(addr_list[addr_qty++], furi_string_get_cstr(line), HEX_MAC_LEN);
                    addr_list[addr_qty - 1][HEX_MAC_LEN] = '\0';
                }
            }
            furi_string_free(line);
        }
    }

    app->settings->badmouse_settings[BADMOUSE_SETTING_ADDR_INDEX].min = 0;
    app->settings->badmouse_settings[BADMOUSE_SETTING_ADDR_INDEX].max =
        (addr_qty > 0) ? (addr_qty - 1) : 0;

    file_stream_close(stream);
    stream_free(stream);
    furi_record_close(RECORD_STORAGE);

    variable_item_set_values_count(bm_item[BADMOUSE_SETTING_ADDR_INDEX], addr_qty);
    set_item_list_name(
        &app->settings->badmouse_settings[BADMOUSE_SETTING_ADDR_INDEX],
        bm_item[BADMOUSE_SETTING_ADDR_INDEX]);

    return addr_qty;
}

void bm_read_layouts(Nrf24Tool* app) {
    layout_qty = 0;
    memset(layout_list, 0, sizeof(layout_list));

    app->storage = furi_record_open(RECORD_STORAGE);
    File* layouts = storage_file_alloc(app->storage);

    if(!storage_dir_open(layouts, FOLDER_PATH_LAYOUT)) {
        FURI_LOG_E(LOG_TAG, "Badmouse : error opening kb layout dir");
        return;
    }

    char file_name[64] = {0};
    FileInfo fileinfo;
    // Loop through directory entries
    while(storage_dir_read(layouts, &fileinfo, file_name, sizeof(file_name))) {
        // Check if it's a file (not a directory)
        if(fileinfo.flags != FSF_DIRECTORY) {
            // Check if the file ends with the desired extension
            const char* file_ext = strrchr(file_name, '.');
            if(file_ext && strcmp(file_ext, LAYOUT_EXTENSION) == 0) {
                size_t length = file_ext - file_name;
                char layout_name[10];

                // Copy the substring before the character into the result buffer
                strncpy(layout_name, file_name, length);
                layout_name[length] = '\0';
                strcpy(layout_list[layout_qty++], layout_name);
            }
        }
    }

    app->settings->badmouse_settings[BADMOUSE_SETTING_KB_LAYOUT].min = 0;
    app->settings->badmouse_settings[BADMOUSE_SETTING_KB_LAYOUT].max =
        (layout_qty > 0) ? (layout_qty - 1) : 0;

    // Close the directory
    storage_dir_close(layouts);
    storage_file_free(layouts);
    furi_record_close(RECORD_STORAGE);

    variable_item_set_values_count(bm_item[BADMOUSE_SETTING_KB_LAYOUT], layout_qty);
    set_item_list_name(
        &app->settings->badmouse_settings[BADMOUSE_SETTING_KB_LAYOUT],
        bm_item[BADMOUSE_SETTING_KB_LAYOUT]);
}
