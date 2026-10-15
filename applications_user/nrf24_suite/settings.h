#pragma once

#include <furi.h>

#include "libnrf24/nrf24.h"
#include "helper.h"

typedef enum {
    RX_SETTING_CHANNEL,
    RX_SETTING_DATA_RATE,
    RX_SETTING_ADDR_WIDTH,
    RX_SETTING_CRC,
    RX_SETTING_RPD,
    RX_SETTING_ACK_PAY,
    RX_SETTING_P0_PAYLOAD,
    RX_SETTING_P0_AUTO_ACK,
    RX_SETTING_P0_ADDR,
    RX_SETTING_P1_PAYLOAD,
    RX_SETTING_P1_AUTO_ACK,
    RX_SETTING_P1_ADDR,
    RX_SETTING_P2_PAYLOAD,
    RX_SETTING_P2_AUTO_ACK,
    RX_SETTING_P2_ADDR,
    RX_SETTING_P3_PAYLOAD,
    RX_SETTING_P3_AUTO_ACK,
    RX_SETTING_P3_ADDR,
    RX_SETTING_P4_PAYLOAD,
    RX_SETTING_P4_AUTO_ACK,
    RX_SETTING_P4_ADDR,
    RX_SETTING_P5_PAYLOAD,
    RX_SETTING_P5_AUTO_ACK,
    RX_SETTING_P5_ADDR,
    RX_SETTING_LOGGING,
    RX_SETTING_COUNT
} RxSettingIndex;

typedef enum {
    TX_SETTING_CHANNEL,
    TX_SETTING_DATA_RATE,
    TX_SETTING_ADDR_WIDTH,
    TX_SETTING_TX_ADDR,
    TX_SETTING_PAYLOAD_SIZE,
    TX_SETTING_FROM_FILE,
    TX_SETTING_AUTO_ACK,
    TX_SETTING_SEND_COUNT,
    TX_SETTING_TX_INTERVAL,
    TX_SETTING_ACK_PAY,
    TX_SETTING_CRC,
    TX_SETTING_TX_POWER,
    TX_SETTING_ARC,
    TX_SETTING_ARD,
    TX_SETTING_LOGGING,
    TX_SETTING_COUNT
} TxSettingIndex;

typedef enum {
    SNIFF_SETTING_MIN_CHANNEL,
    SNIFF_SETTING_MAX_CHANNEL,
    SNIFF_SETTING_SCAN_TIME,
    SNIFF_SETTING_DATA_RATE,
    SNIFF_SETTING_RPD,
    SNIFF_SETTING_COUNT
} SniffSettingIndex;

typedef enum {
    BADMOUSE_SETTING_ADDR_INDEX,
    BADMOUSE_SETTING_KB_LAYOUT,
    BADMOUSE_SETTING_DATA_RATE,
    BADMOUSE_SETTING_TX_POWER,
    BADMOUSE_SETTING_TX_RETRY,
    BADMOUSE_SETTING_KEY_DELAY,
    BADMOUSE_SETTING_COUNT
} BadmouseSettingIndex;

#define SETTINGS_QTY \
    (RX_SETTING_COUNT + TX_SETTING_COUNT + SNIFF_SETTING_COUNT + BADMOUSE_SETTING_COUNT)

struct Nrf24Tool;
extern struct Nrf24Tool* nrf24Tool_app;

typedef enum {
    SETTING_TYPE_UINT8,
    SETTING_TYPE_UINT16,
    SETTING_TYPE_UINT32,
    SETTING_TYPE_BOOL,
    SETTING_TYPE_DATA_RATE,
    SETTING_TYPE_TX_POWER,
    SETTING_TYPE_ADDR_WIDTH,
    SETTING_TYPE_ADDR,
    SETTING_TYPE_ADDR_1BYTE,
    SETTING_TYPE_CRC_LENGHT,
    SETTING_TYPE_PAYLOAD_SIZE,
    SETTING_TYPE_PIPE_NUM,
} SettingType;

typedef union {
    uint8_t u8;
    uint16_t u16;
    uint32_t u32;
    int8_t i8;
    bool b;
    nrf24_data_rate d_r;
    nrf24_tx_power t_p;
    nrf24_addr_width a_w;
    nrf24_crc_lenght crc;
    uint8_t addr[BYTE_MAC_LEN];
} SettingValue;

typedef struct Setting {
    char name[30];
    SettingType type;
    SettingValue value;
    int32_t min;
    int32_t max;
    int32_t step;
} Setting;

typedef struct Settings {
    Setting rx_settings[RX_SETTING_COUNT];
    Setting tx_settings[TX_SETTING_COUNT];
    Setting sniff_settings[SNIFF_SETTING_COUNT];
    Setting badmouse_settings[BADMOUSE_SETTING_COUNT];
} Settings;

typedef struct {
    const char* key;
    void* target;
    SettingType type;
} SettingMapping;

extern SettingMapping settings_map[SETTINGS_QTY];
extern Settings nrf24Tool_settings;
