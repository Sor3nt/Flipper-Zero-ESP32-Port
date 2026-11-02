# Board selection: pass -DFLIPPER_BOARD=<board_name> to idf.py build
# Available boards (see boards/ directory and targets/ directory):
#   waveshare_c6_1.9        - Waveshare ESP32-C6-LCD-1.9 (default, touch input)
#   waveshare_c6_1.47       - Waveshare ESP32-C6-LCD-1.47 (touch input + IMU)
#   lilygo_t_embed_cc1101   - LilyGo T-Embed CC1101 (ESP32-S3, encoder input, CC1101/NFC/IR)
#   smoochie                 - Smoochie (ESP32-S3, 5-button input, CC1101/NRF24/NFC/IR)
#   paradoxt                 - Paradoxt (ESP32-S3 N16R8, 6-way input, CC1101/NRF24/NFC/IR)
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/flipper_board.cmake")
message(STATUS "Flipper board: ${FLIPPER_BOARD}")

if(IDF_TARGET STREQUAL "esp32s3" AND FLIPPER_BOARD MATCHES "^waveshare_c6")
    message(FATAL_ERROR
        "Board '${FLIPPER_BOARD}' is an ESP32-C6 profile but IDF_TARGET is esp32s3. "
        "Select the correct board with -DFLIPPER_BOARD=smoochie (or another ESP32-S3 board).")
elseif(IDF_TARGET STREQUAL "esp32c6" AND
       FLIPPER_BOARD MATCHES "^(smoochie|paradoxt|lilygo_t_embed_cc1101|esp32s3_generic|thormini)$")
    message(FATAL_ERROR
        "Board '${FLIPPER_BOARD}' is an ESP32-S3 profile but IDF_TARGET is esp32c6.")
endif()

set(FURI_HAL_SRCS
    "furi_hal.c"
    "furi_hal_cortex.c"
    "furi_hal_memory.c"
    "furi_hal_rtc.c"
    "furi_hal_power.c"
    "furi_hal_debug.c"
    "furi_hal_interrupt.c"
    "furi_hal_bt.c"
    "furi_hal_crypto.c"
    "furi_hal_flash.c"
    "furi_hal_info.c"
    "furi_hal_region.c"
    "furi_hal_version.c"
    "furi_hal_gpio.c"
    "furi_hal_uart_shared.c"
    "furi_hal_serial.c"
    "furi_hal_resources.c"
    "furi_hal_usb.c"
    "furi_hal_usb_cdc.c"
    "furi_hal_light.c"
    "furi_hal_vibro.c"
    "furi_hal_speaker.c"
    "furi_hal_pwm.c"
    "furi_hal_spi_bus.c"
    "furi_hal_spi.c"
    "furi_hal_subghz.c"
    "furi_hal_nrf24.c"
    "furi_hal_nvs_blob.c"
    "furi_hal_aw9523.c"
    "furi_hal_nfc.c"
    "furi_hal_random.c"
    "furi_hal_sd.c"
    "furi_hal_touch.c"
    "furi_hal_infrared.c"
    "furi_hal_rfid.c"
    "furi_hal_bq27220.c"
    "furi_hal_bq25896.c"
    "fatfs_compat.c"
)

#OLED MOD
if(FLIPPER_BOARD STREQUAL "thormini")
    list(APPEND FURI_HAL_SRCS "furi_hal_display_sh1106.c")
else()
     list(APPEND FURI_HAL_SRCS "furi_hal_display.c")
endif()


set(FURI_HAL_REQUIRES
    furi toolbox input driver esp_lcd fatfs sdmmc datetime
    ble_core ble_hid ble_serial ble_profile furi_ble flipper_protobuf
    esp_adc esp_driver_rmt esp_driver_i2s nvs_flash
    app_update esp_partition esp_app_format mbedtls efuse esp_hw_support
    hal soc
)

# HID backend: TinyUSB on SoCs with USB-OTG, stub elsewhere.
if(IDF_TARGET STREQUAL "esp32s3")
    list(APPEND FURI_HAL_SRCS
        "furi_hal_usb_hid_tinyusb.c"
        "furi_hal_usb_tinyusb_composite.c"
        "furi_hal_usb_msc.c"
        "furi_hal_usb_u2f.c"
    )
    list(APPEND FURI_HAL_REQUIRES espressif__esp_tinyusb)
elseif(IDF_TARGET STREQUAL "esp32s2")
    list(APPEND FURI_HAL_SRCS
        "furi_hal_usb_hid_tinyusb.c"
        "furi_hal_usb_tinyusb_composite.c"
        "furi_hal_usb_msc.c"
    )
    list(APPEND FURI_HAL_REQUIRES espressif__esp_tinyusb)
else()
    list(APPEND FURI_HAL_SRCS "furi_hal_usb_hid_stub.c")
endif()

idf_component_register(
    SRCS ${FURI_HAL_SRCS}
    INCLUDE_DIRS "." ".." "../.." "boards"
    REQUIRES ${FURI_HAL_REQUIRES}
)

# Set board include path as compile definition
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    BOARD_INCLUDE="board_${FLIPPER_BOARD}.h"
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=implicit-function-declaration
    -Wno-error=builtin-declaration-mismatch
)
