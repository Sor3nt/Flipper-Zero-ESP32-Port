#pragma once

#include <stdint.h>

#define ESP_NOW_PKT_MAX_DATA 250

typedef struct {
    uint8_t mac[6];
    uint8_t data[ESP_NOW_PKT_MAX_DATA];
    uint8_t data_len;
    uint32_t timestamp;
} EspNowPacket;
