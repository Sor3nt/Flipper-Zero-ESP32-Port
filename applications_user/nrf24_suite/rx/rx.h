#pragma once

#include <furi.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/byte_input.h>

#include "../nrf24tool.h"
#include "../libnrf24/nrf24.h"
#include "../settings.h"
#include "../helper.h"

#define RX_BUFFER_SIZE   50
#define RX_STRING_SIZE   64
#define RX_DISPLAY_LINES 7
#define RX_DISPLAY_STEP  8
#define RX_START_Y       16

typedef struct RxBuffer {
    char data_hex[RX_BUFFER_SIZE][RX_STRING_SIZE + 1];
    char data_ascii[RX_BUFFER_SIZE][MAX_PAYLOAD_SIZE + 1];
    uint8_t size[RX_BUFFER_SIZE];
    uint8_t from_pipe[RX_BUFFER_SIZE];
    char from_addr[RX_BUFFER_SIZE][HEX_MAC_LEN + 1];
    uint8_t index;
    uint8_t fill_level;
} RxBuffer;

typedef struct RxStatus {
    RxBuffer* rx_buffer;
    NRF24L01_Config* nrf24_config;
    FuriThreadState thread_state;
    bool display_ascii;
} RxStatus;

extern VariableItem* rx_item[RX_SETTING_COUNT];
extern Setting rx_defaults[RX_SETTING_COUNT];
extern NRF24L01_Config rx_nrf24_config;
extern RxBuffer rx_buffer;

int32_t nrf24_rx(void* ctx);
void rx_alloc(Nrf24Tool* app);
void rx_free(Nrf24Tool* app);
