#pragma once

#include <furi.h>
#include <furi_hal.h>
#include "badmouse_hid.h"

typedef enum {
    BadMouseState_Idle,
    BadMouseState_Init,
    BadMouseState_Run,
    BadMouseState_StringPrint,
    BadMouseState_AltStringPrint,
    BadMouseState_WaitForBtn,
    BadMouseState_Done,
    BadMouseState_Error,
} BadMouseExecState;

typedef struct {
    size_t line_cur;
    size_t line_nb;
    uint32_t delay_remain;
    size_t error_line;
    char error[64];
    char line_cur_str[64];
} BadMouseState;

typedef struct BadMouse BadMouse;

BadMouse* badmouse_script_open(FuriString* file_path);

void badmouse_script_close(BadMouse* bad_usb);

int32_t ducky_script_execute_line(BadMouse* badmouse, FuriString* line);

bool ducky_string_next(BadMouse* badmouse);

bool ducky_altstring_next(BadMouse* badmouse);

void badmouse_script_set_keyboard_layout(BadMouse* bad_usb, FuriString* layout_path);
