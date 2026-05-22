#include "nrf24_jam_presets.h"

#include <stdlib.h>

/* ── Channel lists (channel = frequency in MHz minus 2400) ──────────── */

/* WiFi ch 1/6/11: each spans 22 MHz, sub-channels cover the bandwidth. */
static const uint8_t CH_WIFI[] = {1,  3,  5,  7,  9,  11, 13, 15, 17, 19, 21, 23, /* ch 1 */
                                  26, 28, 30, 32, 34, 36, 38, 40, 42, /* ch 6 */
                                  51, 53, 55, 57, 59, 61, 63, 65, 67, 69, 71, 73}; /* ch 11 */

/* BLE data channels: even nRF24 ch 2..80 cover BLE ch 0..36. */
static const uint8_t CH_BLE[] = {2,  4,  6,  8,  10, 12, 14, 16, 18, 20, 22, 24, 26, 28,
                                 30, 32, 34, 36, 38, 40, 42, 44, 46, 48, 50, 52, 54, 56,
                                 58, 60, 62, 64, 66, 68, 70, 72, 74, 76, 78, 80};

/* BLE advertising: ch37=2402→nRF2, ch38=2426→nRF26, ch39=2480→nRF80. */
static const uint8_t CH_BLE_ADV[] = {2, 26, 80};

/* Classic Bluetooth: all FHSS channels 2..80. */
static const uint8_t CH_BLUETOOTH[] = {
    2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
    21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39,
    40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58,
    59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80};

/* Wireless USB dongles. */
static const uint8_t CH_USB[] = {40, 50, 60};

/* Video streaming (upper ISM band). */
static const uint8_t CH_VIDEO[] = {70, 75, 80};

/* RC controllers (low channels). */
static const uint8_t CH_RC[] = {1, 3, 5, 7};

/* Zigbee ch 11..26: 3 nRF sub-channels per Zigbee channel (±1 MHz). */
static const uint8_t CH_ZIGBEE[] = {4,  5,  6,  9,  10, 11, 14, 15, 16, 19, 20, 21,
                                    24, 25, 26, 29, 30, 31, 34, 35, 36, 39, 40, 41,
                                    44, 45, 46, 49, 50, 51, 54, 55, 56, 59, 60, 61,
                                    64, 65, 66, 69, 70, 71, 74, 75, 76, 79, 80, 81};

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

const char* nrf24_jam_preset_name(Nrf24JamPreset preset) {
    switch(preset) {
    case Nrf24JamPresetFull:
        return "Full Spectrum";
    case Nrf24JamPresetWifi:
        return "WiFi 2.4GHz";
    case Nrf24JamPresetBle:
        return "BLE Data";
    case Nrf24JamPresetBleAdv:
        return "BLE Advertising";
    case Nrf24JamPresetBluetooth:
        return "BT Classic";
    case Nrf24JamPresetUsb:
        return "USB Dongles";
    case Nrf24JamPresetVideo:
        return "Video/FPV";
    case Nrf24JamPresetRc:
        return "RC Controllers";
    case Nrf24JamPresetZigbee:
        return "Zigbee";
    case Nrf24JamPresetDrone:
        return "Drone FHSS";
    default:
        return "?";
    }
}

const char* nrf24_jam_preset_short(Nrf24JamPreset preset) {
    switch(preset) {
    case Nrf24JamPresetFull:
        return "Full Spec";
    case Nrf24JamPresetWifi:
        return "WiFi 2.4";
    case Nrf24JamPresetBle:
        return "BLE Data";
    case Nrf24JamPresetBleAdv:
        return "BLE Adv";
    case Nrf24JamPresetBluetooth:
        return "BT Classic";
    case Nrf24JamPresetUsb:
        return "USB Dongle";
    case Nrf24JamPresetVideo:
        return "Video FPV";
    case Nrf24JamPresetRc:
        return "RC Ctrl";
    case Nrf24JamPresetZigbee:
        return "Zigbee";
    case Nrf24JamPresetDrone:
        return "Drone";
    default:
        return "?";
    }
}

const uint8_t* nrf24_jam_preset_channels(Nrf24JamPreset preset, size_t* count) {
    switch(preset) {
    case Nrf24JamPresetWifi:
        *count = ARRAY_COUNT(CH_WIFI);
        return CH_WIFI;
    case Nrf24JamPresetBle:
        *count = ARRAY_COUNT(CH_BLE);
        return CH_BLE;
    case Nrf24JamPresetBleAdv:
        *count = ARRAY_COUNT(CH_BLE_ADV);
        return CH_BLE_ADV;
    case Nrf24JamPresetBluetooth:
        *count = ARRAY_COUNT(CH_BLUETOOTH);
        return CH_BLUETOOTH;
    case Nrf24JamPresetUsb:
        *count = ARRAY_COUNT(CH_USB);
        return CH_USB;
    case Nrf24JamPresetVideo:
        *count = ARRAY_COUNT(CH_VIDEO);
        return CH_VIDEO;
    case Nrf24JamPresetRc:
        *count = ARRAY_COUNT(CH_RC);
        return CH_RC;
    case Nrf24JamPresetZigbee:
        *count = ARRAY_COUNT(CH_ZIGBEE);
        return CH_ZIGBEE;
    case Nrf24JamPresetFull:
    case Nrf24JamPresetDrone:
    default:
        *count = 0;
        return NULL;
    }
}

uint16_t nrf24_jam_preset_default_dwell_us(Nrf24JamPreset preset) {
    switch(preset) {
    case Nrf24JamPresetBleAdv: /* only 3 channels */
    case Nrf24JamPresetRc: /* 4 channels */
        return 1500;
    case Nrf24JamPresetUsb: /* 3 channels */
    case Nrf24JamPresetVideo: /* 3 channels */
        return 1000;
    case Nrf24JamPresetWifi:
    case Nrf24JamPresetBle:
    case Nrf24JamPresetZigbee:
        return 300;
    case Nrf24JamPresetFull:
    case Nrf24JamPresetBluetooth:
    case Nrf24JamPresetDrone:
    default:
        return 200; /* fast sweep across many channels */
    }
}

uint8_t nrf24_jam_preset_next_channel(Nrf24JamPreset preset, uint32_t* hop_index) {
    if(preset == Nrf24JamPresetDrone) {
        /* FHSS target — random hop across the whole band. */
        return (uint8_t)(rand() % 125);
    }

    size_t count = 0;
    const uint8_t* list = nrf24_jam_preset_channels(preset, &count);

    if(list == NULL || count == 0) {
        /* Full spectrum — sequential sweep 0..124. */
        uint8_t ch = (uint8_t)(*hop_index % 125);
        (*hop_index)++;
        return ch;
    }

    uint8_t ch = list[*hop_index % count];
    (*hop_index)++;
    return ch;
}
