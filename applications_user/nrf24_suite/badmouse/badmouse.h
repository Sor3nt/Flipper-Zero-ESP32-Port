#pragma once

#include <furi.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <gui/modules/variable_item_list.h>
#include <stream/file_stream.h>
#include <storage/storage.h>
#include <gui/modules/dialog_ex.h>

#include "helpers/badmouse_hid.h"
#include "helpers/ducky_script_i.h"

#include "../nrf24tool.h"
#include "../libnrf24/nrf24.h"
#include "../settings.h"
#include "../helper.h"

#define BAD_USB_APP_BASE_FOLDER EXT_PATH("badusb")
#define FOLDER_PATH_LAYOUT      BAD_USB_APP_BASE_FOLDER "/assets/layouts"
#define SCRIPT_EXTENSION        ".txt"
#define LAYOUT_EXTENSION        ".kl"
#define MAX_CONFIRMED_ADDR      32
#define MAX_KB_LAYOUT           32
#define LAYOUT_NAME_LENGHT      16
#define MAX_NAME_LEN            256
#define MAX_LAYOUT_LEN          16
#define ALT_STRING_DELAY        20

typedef struct BadMouse {
    NRF24L01_Config* nrf24_config;
    Nrf24Tool* app_instance;

    FuriString* file_path;
    Stream* payload_stream;

    uint32_t defdelay;
    uint32_t stringdelay;
    uint32_t defstringdelay;
    uint16_t layout[128];

    FuriString* line;
    FuriString* line_prev;
    uint32_t repeat_cnt;
    uint8_t key_hold_nb;

    FuriString* string_print;
    size_t string_print_pos;
    BadMouseState state;
    BadMouseExecState execState;
} BadMouse;

typedef struct BadMouseStatus {
    char file_name[MAX_NAME_LEN];
    char layout[MAX_LAYOUT_LEN];
    BadMouseState state;
    BadMouseExecState execState;
    FuriThreadState thread_state;
    uint8_t channel;
} BadMouseStatus;

extern Setting badmouse_defaults[BADMOUSE_SETTING_COUNT];
extern FuriString* bm_payload_path;
extern uint8_t addr_qty;
extern char addr_list[MAX_CONFIRMED_ADDR][HEX_MAC_LEN + 1];
extern uint8_t layout_qty;
extern char layout_list[MAX_KB_LAYOUT][LAYOUT_NAME_LENGHT];
extern VariableItem* bm_item[BADMOUSE_SETTING_COUNT];

int32_t nrf24_badmouse(void* ctx);
void badmouse_alloc(Nrf24Tool* app);
void badmouse_free(Nrf24Tool* app);
uint8_t bm_read_address(Nrf24Tool* context);
void bm_read_layouts(Nrf24Tool* context);
