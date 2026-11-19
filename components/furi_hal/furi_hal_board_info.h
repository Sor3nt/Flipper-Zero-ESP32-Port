/**
 * @file furi_hal_board_info.h
 * Board identity and support policy for the ported apps (FAPs).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** True on the boards the ported apps are supported on: LilyGo T-Embed CC1101, Paradoxt,
 *  ThorChimera and Thormini (all ESP32-S3). Other boards (DIY, generic S3, the C6 boards)
 *  have a different pin / power / USB layout the apps were never checked against: they must
 *  refuse to start there instead of driving pins that mean something else. */
bool furi_hal_board_supports_ported_apps(void);

/** Human readable board name for messages (never NULL). */
const char* furi_hal_board_get_name(void);

/** Name of the MCU this firmware was built for, e.g. "ESP32-S3" or "ESP32-C6" (never NULL).
 *  Comes from the ESP-IDF target, so every board reports its own chip. */
const char* furi_hal_board_get_mcu_name(void);

/** Chip model and silicon revision read from the chip (esp_chip_info()),
 *  e.g. "ESP32-S3 rev v0.2". Always terminated; truncated to size. */
void furi_hal_board_get_mcu_description(char* buffer, size_t size);

/** Factory MAC address burned into eFuse by Espressif (esp_read_mac(...,
 *  ESP_MAC_EFUSE_FACTORY)): unique per chip and unaffected by MAC spoofing.
 *  @return false when it could not be read. */
bool furi_hal_board_get_factory_mac(uint8_t mac[6]);

/** PCB version of the board, e.g. "v1.0 rev 3.0", or NULL when the board
 *  header does not define BOARD_HW_VERSION. */
const char* furi_hal_board_get_hw_version(void);

/** Physical input of the running board. FAPs are built once (with the T-Embed header), so a
 *  FAP must ask this at run time instead of testing BOARD_* macros. */
typedef enum {
    /** Discrete buttons; the firmware GUI still applies the encoder-style Left/Right remap
     *  (Thormini, DIY). */
    FuriHalBoardInputLayoutButtons = 0,
    /** Rotary encoder: turn = Up/Down, click = OK, hold OK + turn = Left/Right
     *  (LilyGo T-Embed, ThorChimera, generic ESP32-S3). */
    FuriHalBoardInputLayoutEncoder,
    /** Real 4-way D-pad + OK + Back, stock Flipper event stream (Paradoxt). */
    FuriHalBoardInputLayoutDpad,
    /** Touch panel (Waveshare). */
    FuriHalBoardInputLayoutTouch,
} FuriHalBoardInputLayout;

/** Input layout of the running board (see FuriHalBoardInputLayout). */
FuriHalBoardInputLayout furi_hal_board_get_input_layout(void);

#ifdef __cplusplus
}
#endif
