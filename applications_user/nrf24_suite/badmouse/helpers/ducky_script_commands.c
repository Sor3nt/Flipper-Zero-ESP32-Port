#include <furi_hal.h>
#include "ducky_script.h"
#include "ducky_script_i.h"
#include "../badmouse.h"

#define TAG        "BadUsb"
#define WORKER_TAG TAG "Worker"

typedef int32_t (
    *DuckyCmdCallback)(BadMouse* badmouse, const char* line, int32_t param, size_t cmd_length);

typedef struct {
    char* name;
    size_t length;
    DuckyCmdCallback callback;
    int32_t param;
} DuckyCmd;

static int32_t
    ducky_fnc_delay(BadMouse* badmouse, const char* line, int32_t param, size_t cmd_length) {
    UNUSED(param);

    line = &line[cmd_length + 1];
    uint32_t delay_val = 0;
    bool state = ducky_get_number(line, &delay_val);
    bm_release_all(badmouse->app_instance);
    if((state) && (delay_val > 0)) {
        return (int32_t)delay_val;
    }

    return ducky_error(badmouse, "Invalid number %s", line);
}

static int32_t
    ducky_fnc_defdelay(BadMouse* badmouse, const char* line, int32_t param, size_t cmd_length) {
    UNUSED(param);

    line = &line[cmd_length + 1];
    uint32_t delay = 0;
    bool state = ducky_get_number(line, &delay);
    if(!state) {
        return ducky_error(badmouse, "Invalid number %s", line);
    } else {
        badmouse->defdelay = delay;
    }
    return 0;
}

static int32_t
    ducky_fnc_strdelay(BadMouse* badmouse, const char* line, int32_t param, size_t cmd_length) {
    UNUSED(param);

    line = &line[cmd_length + 1];
    bool state = ducky_get_number(line, &badmouse->stringdelay);
    if(!state) {
        return ducky_error(badmouse, "Invalid number %s", line);
    }
    return 0;
}

static int32_t
    ducky_fnc_defstrdelay(BadMouse* badmouse, const char* line, int32_t param, size_t cmd_length) {
    UNUSED(param);

    line = &line[cmd_length + 1];
    bool state = ducky_get_number(line, &badmouse->defstringdelay);
    if(!state) {
        return ducky_error(badmouse, "Invalid number %s", line);
    }
    return 0;
}

static int32_t
    ducky_fnc_string(BadMouse* badmouse, const char* line, int32_t param, size_t cmd_length) {
    line = &line[cmd_length + 1];
    furi_string_set_str(badmouse->string_print, line);
    if(param == 1) {
        furi_string_cat(badmouse->string_print, "\n");
    }
    badmouse->string_print_pos = 0;
    if(param == 2) {
        return SCRIPT_STATE_ALTSTRING_START;
    } else {
        return SCRIPT_STATE_STRING_START;
    }
}

static int32_t
    ducky_fnc_repeat(BadMouse* badmouse, const char* line, int32_t param, size_t cmd_length) {
    UNUSED(param);

    line = &line[cmd_length + 1];
    bool state = ducky_get_number(line, &badmouse->repeat_cnt);
    if((!state) || (badmouse->repeat_cnt == 0)) {
        return ducky_error(badmouse, "Invalid number %s", line);
    }
    return 0;
}

static int32_t
    ducky_fnc_sysrq(BadMouse* badmouse, const char* line, int32_t param, size_t cmd_length) {
    UNUSED(param);

    line = &line[cmd_length + 1];
    uint16_t key = ducky_get_keycode(badmouse, line, true);
    //badmouse->hid->kb_press(badmouse->hid_inst, KEY_MOD_LEFT_ALT | HID_KEYBOARD_PRINT_SCREEN);
    bm_press_key(badmouse->app_instance, (KEY_MOD_LEFT_ALT | HID_KEYBOARD_PRINT_SCREEN));
    //badmouse->hid->kb_press(badmouse->hid_inst, key);
    bm_press_key(badmouse->app_instance, key);
    //badmouse->hid->release_all(badmouse->hid_inst);
    bm_release_all(badmouse->app_instance);
    return 0;
}

static int32_t
    ducky_fnc_altchar(BadMouse* badmouse, const char* line, int32_t param, size_t cmd_length) {
    UNUSED(param);

    line = &line[cmd_length + 1];
    uint32_t char_code;

    bool state = ducky_get_number(line, &char_code);
    if(!state || char_code > MAX_ALT_CODE) {
        return ducky_error(badmouse, "Invalid altchar %s", line);
    }
    state = ducky_altchar(badmouse, (uint16_t)char_code);
    if(!state) {
        return ducky_error(badmouse, "Invalid altchar %s", line);
    }
    return 0;
}

/* static int32_t
    ducky_fnc_altstring(BadMouse* badmouse, const char* line, int32_t param, size_t cmd_length) {
    UNUSED(param);

    line = &line[cmd_length + 1];
    bool state = ducky_altstring(badmouse, line);
    if(!state) {
        return ducky_error(badmouse, "Invalid altstring %s", line);
    }
    return 0;
} */

static int32_t
    ducky_fnc_hold(BadMouse* badmouse, const char* line, int32_t param, size_t cmd_length) {
    UNUSED(param);

    line = &line[cmd_length + 1];
    uint16_t key = ducky_get_keycode(badmouse, line, true);
    if(key == HID_KEYBOARD_NONE) {
        return ducky_error(badmouse, "No keycode defined for %s", line);
    }
    badmouse->key_hold_nb++;
    if(badmouse->key_hold_nb > (HID_KB_MAX_KEYS - 1)) {
        return ducky_error(badmouse, "Too many keys are hold");
    }
    //badmouse->hid->kb_press(badmouse->hid_inst, key);
    bm_press_key(badmouse->app_instance, key);
    return 0;
}

static int32_t
    ducky_fnc_release(BadMouse* badmouse, const char* line, int32_t param, size_t cmd_length) {
    UNUSED(param);

    line = &line[cmd_length + 1];
    uint16_t key = ducky_get_keycode(badmouse, line, true);
    if(key == HID_KEYBOARD_NONE) {
        return ducky_error(badmouse, "No keycode defined for %s", line);
    }
    if(badmouse->key_hold_nb == 0) {
        return ducky_error(badmouse, "No keys are hold");
    }
    badmouse->key_hold_nb--;
    //badmouse->hid->kb_release(badmouse->hid_inst, key);
    bm_release_key(badmouse->app_instance, key);
    return 0;
}

static int32_t
    ducky_fnc_media(BadMouse* badmouse, const char* line, int32_t param, size_t cmd_length) {
    UNUSED(param);

    line = &line[cmd_length + 1];
    uint16_t key = ducky_get_media_keycode_by_name(line);
    if(key == HID_CONSUMER_UNASSIGNED) {
        return ducky_error(badmouse, "No keycode defined for %s", line);
    }
    //badmouse->hid->consumer_press(badmouse->hid_inst, key);
    bm_press_key(badmouse->app_instance, key);
    //badmouse->hid->consumer_release(badmouse->hid_inst, key);
    bm_release_key(badmouse->app_instance, key);
    return 0;
}

static int32_t
    ducky_fnc_globe(BadMouse* badmouse, const char* line, int32_t param, size_t cmd_length) {
    UNUSED(param);

    line = &line[cmd_length + 1];
    uint16_t key = ducky_get_keycode(badmouse, line, true);
    if(key == HID_KEYBOARD_NONE) {
        return ducky_error(badmouse, "No keycode defined for %s", line);
    }

    //badmouse->hid->consumer_press(badmouse->hid_inst, HID_CONSUMER_FN_GLOBE);
    bm_press_key(badmouse->app_instance, HID_CONSUMER_FN_GLOBE);
    //badmouse->hid->kb_press(badmouse->hid_inst, key);
    bm_press_key(badmouse->app_instance, key);
    //badmouse->hid->kb_release(badmouse->hid_inst, key);
    bm_release_key(badmouse->app_instance, key);
    //badmouse->hid->consumer_release(badmouse->hid_inst, HID_CONSUMER_FN_GLOBE);
    bm_release_key(badmouse->app_instance, HID_CONSUMER_FN_GLOBE);
    return 0;
}

static int32_t ducky_fnc_waitforbutton(
    BadMouse* badmouse,
    const char* line,
    int32_t param,
    size_t cmd_length) {
    UNUSED(param);
    UNUSED(badmouse);
    UNUSED(line);
    UNUSED(cmd_length);

    return SCRIPT_STATE_WAIT_FOR_BTN;
}

static const DuckyCmd ducky_commands[] = {
    {"REM", 3, NULL, -1},
    {"ID", 2, NULL, -1},
    {"DELAY", 5, ducky_fnc_delay, -1},
    {"STRING", 6, ducky_fnc_string, 0},
    {"STRINGLN", 8, ducky_fnc_string, 1},
    {"DEFAULT_DELAY", 13, ducky_fnc_defdelay, -1},
    {"DEFAULTDELAY", 12, ducky_fnc_defdelay, -1},
    {"STRINGDELAY", 11, ducky_fnc_strdelay, -1},
    {"STRING_DELAY", 12, ducky_fnc_strdelay, -1},
    {"DEFAULT_STRING_DELAY", 20, ducky_fnc_defstrdelay, -1},
    {"DEFAULTSTRINGDELAY", 18, ducky_fnc_defstrdelay, -1},
    {"REPEAT", 6, ducky_fnc_repeat, -1},
    {"SYSRQ", 5, ducky_fnc_sysrq, -1},
    {"ALTCHAR", 7, ducky_fnc_altchar, -1},
    {"ALTSTRING", 9, ducky_fnc_string, 2},
    {"ALTCODE", 7, ducky_fnc_string, 2},
    {"HOLD", 4, ducky_fnc_hold, -1},
    {"RELEASE", 7, ducky_fnc_release, -1},
    {"WAIT_FOR_BUTTON_PRESS", 21, ducky_fnc_waitforbutton, -1},
    {"MEDIA", 5, ducky_fnc_media, -1},
    {"GLOBE", 5, ducky_fnc_globe, -1},
};

int32_t ducky_execute_cmd(BadMouse* badmouse, const char* line) {
    size_t cmd_word_len = strcspn(line, " ");
    for(uint8_t i = 0; i < COUNT_OF(ducky_commands); i++) {
        size_t cmd_compare_len = ducky_commands[i].length;
        if(cmd_compare_len != cmd_word_len) {
            continue;
        }

        if(strncmp(line, ducky_commands[i].name, cmd_compare_len) == 0) {
            if(ducky_commands[i].callback == NULL) {
                return SCRIPT_STATE_EMPTY_LINE;
            } else {
                return (ducky_commands[i].callback)(
                    badmouse, line, ducky_commands[i].param, ducky_commands[i].length);
            }
        }
    }

    return SCRIPT_STATE_CMD_UNKNOWN;
}
