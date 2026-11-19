#include "furi_hal_board_info.h"

#include "boards/board.h"

#include <sdkconfig.h>
#include <stdint.h>
#include <stdio.h>

#include <esp_chip_info.h>
#include <esp_mac.h>

/* A board opts in by defining BOARD_PORTED_APPS_SUPPORTED to 1 in its header. */
bool furi_hal_board_supports_ported_apps(void) {
#if defined(BOARD_PORTED_APPS_SUPPORTED) && BOARD_PORTED_APPS_SUPPORTED
    return true;
#else
    return false;
#endif
}

const char* furi_hal_board_get_name(void) {
    return BOARD_NAME;
}

/* The image only runs on the chip it was built for, so the build target is the MCU. */
const char* furi_hal_board_get_mcu_name(void) {
#if CONFIG_IDF_TARGET_ESP32S3
    return "ESP32-S3";
#elif CONFIG_IDF_TARGET_ESP32C6
    return "ESP32-C6";
#elif CONFIG_IDF_TARGET_ESP32S2
    return "ESP32-S2";
#elif CONFIG_IDF_TARGET_ESP32C3
    return "ESP32-C3";
#elif CONFIG_IDF_TARGET_ESP32C5
    return "ESP32-C5";
#elif CONFIG_IDF_TARGET_ESP32H2
    return "ESP32-H2";
#elif CONFIG_IDF_TARGET_ESP32P4
    return "ESP32-P4";
#elif CONFIG_IDF_TARGET_ESP32
    return "ESP32";
#else
    return CONFIG_IDF_TARGET;
#endif
}

static const char* furi_hal_board_get_chip_model_name(esp_chip_model_t model) {
    switch(model) {
    case CHIP_ESP32:
        return "ESP32";
    case CHIP_ESP32S2:
        return "ESP32-S2";
    case CHIP_ESP32S3:
        return "ESP32-S3";
    case CHIP_ESP32C3:
        return "ESP32-C3";
    case CHIP_ESP32C2:
        return "ESP32-C2";
    case CHIP_ESP32C6:
        return "ESP32-C6";
    case CHIP_ESP32H2:
        return "ESP32-H2";
    case CHIP_ESP32P4:
        return "ESP32-P4";
    default:
        /* A model this list does not know: the build target names it. */
        return furi_hal_board_get_mcu_name();
    }
}

void furi_hal_board_get_mcu_description(char* buffer, size_t size) {
    if(!buffer || size == 0) return;

    esp_chip_info_t info;
    esp_chip_info(&info);
    /* revision is MXX: M = wafer major version, XX = wafer minor version. */
    snprintf(
        buffer,
        size,
        "%s rev v%u.%u",
        furi_hal_board_get_chip_model_name(info.model),
        (unsigned)(info.revision / 100),
        (unsigned)(info.revision % 100));
}

bool furi_hal_board_get_factory_mac(uint8_t mac[6]) {
    return mac && esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY) == ESP_OK;
}

const char* furi_hal_board_get_hw_version(void) {
#ifdef BOARD_HW_VERSION
    return BOARD_HW_VERSION;
#else
    return NULL;
#endif
}

FuriHalBoardInputLayout furi_hal_board_get_input_layout(void) {
#if defined(BOARD_INPUT_DPAD) && BOARD_INPUT_DPAD
    return FuriHalBoardInputLayoutDpad;
#elif defined(BOARD_HAS_ENCODER) && BOARD_HAS_ENCODER
    return FuriHalBoardInputLayoutEncoder;
#elif defined(BOARD_PIN_TOUCH_SDA)
    /* Boards without a panel set the touch pins to UINT16_MAX. */
    return (BOARD_PIN_TOUCH_SDA != UINT16_MAX) ? FuriHalBoardInputLayoutTouch :
                                                 FuriHalBoardInputLayoutButtons;
#else
    return FuriHalBoardInputLayoutButtons;
#endif
}
