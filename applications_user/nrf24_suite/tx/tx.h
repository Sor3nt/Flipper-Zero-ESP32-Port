#pragma once

#include <furi.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/byte_input.h>
#include <gui/modules/text_input.h>

#include "../nrf24tool.h"
#include "../libnrf24/nrf24.h"
#include "../settings.h"
#include "../helper.h"

#define TX_MAX_REPEAT         32
#define TX_PAYLOAD_SIZE_ASCII (MAX_PAYLOAD_SIZE + 1)
#define TX_PAYLOAD_SIZE_HEX   (2 * MAX_PAYLOAD_SIZE + 1)
#define TX_PAYLOAD_EXT        ".txt"
#define TX_MAX_DISPLAY_LINES  5
#define TX_MAX_DISPLAY_CHAR   39
#define TX_LINE_HEIGHT        9

typedef enum {
    TX_PAYLOAD_TYPE_ASCII = 1,
    TX_PAYLOAD_TYPE_HEX,
    TX_PAYLOAD_TYPE_FILE,
} TxPayloadType;

typedef struct TxStatus {
    NRF24L01_Config* nrf24_config;
    FuriThreadState thread_state;
    uint8_t payload[MAX_PAYLOAD_SIZE];
    uint8_t payload_size;
    uint8_t ack_payload[MAX_PAYLOAD_SIZE];
    uint8_t ack_payload_size;
    uint8_t scroll_hori;
    uint8_t scroll_vert;
    bool send_ok;
    bool send_failed;
    bool ack_payload_enabled;
} TxStatus;

extern VariableItem* tx_item[TX_SETTING_COUNT];
extern Setting tx_defaults[TX_SETTING_COUNT];
extern NRF24L01_Config tx_nrf24_config;
extern char tx_payload_ascii[TX_PAYLOAD_SIZE_ASCII];
extern uint8_t tx_payload_hex[MAX_PAYLOAD_SIZE];
extern FuriString* tx_payload_path;
extern TxPayloadType tx_payload_type;

int32_t nrf24_tx(void* ctx);
void tx_alloc(Nrf24Tool* app);
void tx_free(Nrf24Tool* app);
