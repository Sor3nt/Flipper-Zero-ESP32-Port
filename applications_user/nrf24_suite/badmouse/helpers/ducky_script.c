#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <lib/toolbox/args.h>
#include <storage/storage.h>
#include "ducky_script.h"
#include "ducky_script_i.h"
#include <dolphin/dolphin.h>
#include "badmouse/badmouse.h"
#include "badmouse_hid.h"
#include <errno.h>

#define WORKER_TAG "Badmouse"

#define BADUSB_ASCII_TO_KEY(script, x) \
    (((uint8_t)x < 128) ? (script->layout[(uint8_t)x]) : HID_KEYBOARD_NONE)

typedef enum {
    WorkerEvtStartStop = (1 << 0),
    WorkerEvtPauseResume = (1 << 1),
    WorkerEvtEnd = (1 << 2),
    WorkerEvtConnect = (1 << 3),
    WorkerEvtDisconnect = (1 << 4),
} WorkerEvtFlags;

static const uint8_t numpad_keys[10] = {
    HID_KEYPAD_0,
    HID_KEYPAD_1,
    HID_KEYPAD_2,
    HID_KEYPAD_3,
    HID_KEYPAD_4,
    HID_KEYPAD_5,
    HID_KEYPAD_6,
    HID_KEYPAD_7,
    HID_KEYPAD_8,
    HID_KEYPAD_9,
};

bool ducky_is_line_end(const char chr) {
    return (chr == ' ') || (chr == '\0') || (chr == '\r') || (chr == '\n');
}

uint16_t ducky_get_keycode(BadMouse* badmouse, const char* param, bool accept_chars) {
    uint16_t keycode = ducky_get_keycode_by_name(param);
    if(keycode != HID_KEYBOARD_NONE) {
        return keycode;
    }

    if((accept_chars) && (strlen(param) > 0)) {
        return BADUSB_ASCII_TO_KEY(badmouse, param[0]);
    }
    return 0;
}

/* bool ducky_get_number(const char* param, uint32_t* val) {
    uint32_t value = 0;
    if(strint_to_uint32(param, NULL, &value, 10) == StrintParseNoError) {
        *val = value;
        return true;
    }
    return false;
} */

bool ducky_get_number(const char* param, uint32_t* val) {
    errno = 0;

    unsigned long value = strtoul(param, NULL, 10);

    if(errno != 0 || value > UINT32_MAX) {
        return false;
    }

    *val = (uint32_t)value;
    return true;
}

bool ducky_numpad_press(BadMouse* badmouse, uint8_t num) {
    if((num > 9)) return false;

    //badmouse->hid->kb_press(badmouse->hid_inst, key);
    bm_press_key(badmouse->app_instance, numpad_keys[num]);
    //badmouse->hid->kb_release(badmouse->hid_inst, key);
    bm_release_key(badmouse->app_instance, numpad_keys[num]);

    return true;
}

bool ducky_altchar(BadMouse* badmouse, uint16_t charcode) {
    bool state = false;

    if(charcode > MAX_ALT_CODE) {
        return false;
    }

    //badmouse->hid->kb_press(badmouse->hid_inst, KEY_MOD_LEFT_ALT);
    bm_press_key(badmouse->app_instance, KEY_MOD_LEFT_ALT);

    uint16_t divisor = 1000;
    while(divisor > 0) {
        if(charcode >= divisor) {
            uint8_t digit = (charcode / divisor) % 10;
            state = ducky_numpad_press(badmouse, digit);
            if(state == false) break;
        }
        divisor /= 10;
    }

    //badmouse->hid->kb_release(badmouse->hid_inst, KEY_MOD_LEFT_ALT);
    bm_release_key(badmouse->app_instance, KEY_MOD_LEFT_ALT);
    return state;
}

bool ducky_altstring_next(BadMouse* badmouse) {
    if(badmouse->string_print_pos >= furi_string_size(badmouse->string_print)) {
        return true;
    }

    char print_char = furi_string_get_char(badmouse->string_print, badmouse->string_print_pos);
    uint8_t ascii_code = (uint8_t)(unsigned char)print_char;

    ducky_altchar(badmouse, ascii_code);

    badmouse->string_print_pos++;
    return false;
}

int32_t ducky_error(BadMouse* badmouse, const char* text, ...) {
    va_list args;
    va_start(args, text);

    vsnprintf(badmouse->state.error, sizeof(badmouse->state.error), text, args);
    badmouse->execState = BadMouseState_Error;

    va_end(args);
    return SCRIPT_STATE_ERROR;
}

bool ducky_string_next(BadMouse* badmouse) {
    if(badmouse->string_print_pos >= furi_string_size(badmouse->string_print)) {
        return true;
    }

    char print_char = furi_string_get_char(badmouse->string_print, badmouse->string_print_pos);

    if(print_char != '\n') {
        uint16_t keycode = BADUSB_ASCII_TO_KEY(badmouse, print_char);
        if(keycode != HID_KEYBOARD_NONE) {
            //badmouse->hid->kb_press(badmouse->hid_inst, keycode);
            bm_press_key(badmouse->app_instance, keycode);
            //badmouse->hid->kb_release(badmouse->hid_inst, keycode);
            bm_release_key(badmouse->app_instance, keycode);
        }
    } else {
        //badmouse->hid->kb_press(badmouse->hid_inst, HID_KEYBOARD_RETURN);
        bm_press_key(badmouse->app_instance, HID_KEYBOARD_RETURN);
        //badmouse->hid->kb_release(badmouse->hid_inst, HID_KEYBOARD_RETURN);
        bm_release_key(badmouse->app_instance, HID_KEYBOARD_RETURN);
    }

    badmouse->string_print_pos++;

    return false;
}

static int32_t ducky_parse_line(BadMouse* badmouse, FuriString* line) {
    /* size_t line_len = furi_string_size(line);
    if(line_len <= 1) {
        return SCRIPT_STATE_EMPTY_LINE; // Skip empty lines
    } */
    if(furi_string_empty(line)) return SCRIPT_STATE_EMPTY_LINE; // Skip empty lines

    const char* line_tmp = furi_string_get_cstr(line);

    // Ducky Lang Functions
    int32_t cmd_result = ducky_execute_cmd(badmouse, line_tmp);
    if(cmd_result != SCRIPT_STATE_CMD_UNKNOWN) {
        return cmd_result;
    }

    // Special keys + modifiers
    uint16_t key = ducky_get_keycode(badmouse, line_tmp, false);
    if(key == HID_KEYBOARD_NONE) {
        return ducky_error(badmouse, "No keycode defined for %s", line_tmp);
    }
    if((key & 0xFF00) != 0) {
        // It's a modifier key
        line_tmp = &line_tmp[strcspn(line_tmp, " ") + 1];
        key |= ducky_get_keycode(badmouse, line_tmp, true);
    }
    //badmouse->hid->kb_press(badmouse->hid_inst, key);
    bm_press_key(badmouse->app_instance, key);
    //badmouse->hid->kb_release(badmouse->hid_inst, key);
    bm_release_key(badmouse->app_instance, key);
    return 0;
}

int32_t ducky_script_execute_line(BadMouse* badmouse, FuriString* line) {
    int32_t return_val = 0;

    return_val = ducky_parse_line(badmouse, line);
    if(return_val == SCRIPT_STATE_EMPTY_LINE) { // Empty line
        return return_val;
    } else if(return_val == SCRIPT_STATE_STRING_START) { // Print string
        return return_val;
    } else if(return_val == SCRIPT_STATE_ALTSTRING_START) { // Print string using Alt+Numpad
        return return_val;
    } else if(return_val == SCRIPT_STATE_WAIT_FOR_BTN) { // wait for button
        return return_val;
    } else if(return_val < 0) { // Script error
        badmouse->state.error_line = badmouse->state.line_cur;
        FURI_LOG_E(WORKER_TAG, "Unknown command at line %zu", badmouse->state.line_cur);
        return SCRIPT_STATE_ERROR;
    }
    return return_val;
}

static void badmouse_script_set_default_keyboard_layout(BadMouse* badmouse) {
    furi_assert(badmouse);
    memset(badmouse->layout, HID_KEYBOARD_NONE, sizeof(badmouse->layout));
    memcpy(badmouse->layout, hid_asciimap, MIN(sizeof(hid_asciimap), sizeof(badmouse->layout)));
}

BadMouse* badmouse_script_open(FuriString* file_path) {
    furi_assert(file_path);

    BadMouse* badmouse = malloc(sizeof(BadMouse));
    badmouse->file_path = furi_string_alloc();
    badmouse->line = furi_string_alloc();
    badmouse->line_prev = furi_string_alloc();
    badmouse->string_print = furi_string_alloc();
    furi_string_set(badmouse->file_path, file_path);
    badmouse_script_set_default_keyboard_layout(badmouse);

    badmouse->execState = BadMouseState_Init;
    badmouse->state.error[0] = '\0';

    return badmouse;
} //-V773

void badmouse_script_close(BadMouse* badmouse) {
    furi_assert(badmouse);

    furi_string_free(badmouse->file_path);
    furi_string_free(badmouse->line);
    furi_string_free(badmouse->line_prev);
    furi_string_free(badmouse->string_print);
    free(badmouse);
}

void badmouse_script_set_keyboard_layout(BadMouse* badmouse, FuriString* layout_path) {
    furi_assert(badmouse);

    if(badmouse->execState == BadMouseState_Run) {
        // do not update keyboard layout while a script is running
        return;
    }

    File* layout_file = storage_file_alloc(furi_record_open(RECORD_STORAGE));
    if(!furi_string_empty(layout_path)) { //-V1051
        if(storage_file_open(
               layout_file, furi_string_get_cstr(layout_path), FSAM_READ, FSOM_OPEN_EXISTING)) {
            uint16_t layout[128];
            if(storage_file_read(layout_file, layout, sizeof(layout)) == sizeof(layout)) {
                memcpy(badmouse->layout, layout, sizeof(layout));
            } else {
                ducky_error(badmouse, "Keyboard layout file error");
            }
        }
        storage_file_close(layout_file);
    } else {
        badmouse_script_set_default_keyboard_layout(badmouse);
        ducky_error(badmouse, "Error opening keyboard layout file");
    }
    storage_file_free(layout_file);
    furi_record_close(RECORD_STORAGE);
}
