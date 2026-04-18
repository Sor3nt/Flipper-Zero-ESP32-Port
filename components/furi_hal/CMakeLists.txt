# Board selection: pass -DFLIPPER_BOARD=<board_name> to idf.py build
# Available boards (see boards/ directory and targets/ directory):
#   waveshare_c6_1.9        - Waveshare ESP32-C6-LCD-1.9 (default, touch input)
#   waveshare_c6_1.47       - Waveshare ESP32-C6-LCD-1.47 (touch input + IMU)
#   lilygo_t_embed_cc1101   - LilyGo T-Embed CC1101 (ESP32-S3, encoder input, CC1101/NFC/IR)
if(NOT DEFINED FLIPPER_BOARD)
    set(FLIPPER_BOARD "waveshare_c6_1.9" CACHE STRING "Target board for Flipper ESP32 port")
endif()
message(STATUS "Flipper board: ${FLIPPER_BOARD}")

idf_component_register(
    SRCS
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
        "furi_hal_resources.c"
        "furi_hal_usb.c"
        "furi_hal_usb_msc.c"
        "furi_hal_usb_cdc.c"
        "furi_hal_light.c"
        "furi_hal_vibro.c"
        "furi_hal_speaker.c"
        "furi_hal_display.c"
        "furi_hal_spi_bus.c"
        "furi_hal_spi.c"
        "furi_hal_subghz.c"
        "furi_hal_nfc.c"
        "furi_hal_random.c"
        "furi_hal_sd.c"
        "furi_hal_touch.c"
        "furi_hal_infrared.c"
        "furi_hal_bq27220.c"
        "furi_hal_bq25896.c"
        "fatfs_compat.c"
    INCLUDE_DIRS "." ".." "../.." "boards"
    REQUIRES furi toolbox input driver esp_lcd fatfs sdmmc datetime ble_hid ble_serial ble_profile furi_ble flipper_protobuf esp_adc esp_driver_rmt esp_driver_i2s nvs_flash esp_tinyusb
)

# Set board include path as compile definition
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    BOARD_INCLUDE="board_${FLIPPER_BOARD}.h"
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=implicit-function-declaration
    -Wno-error=builtin-declaration-mismatch
)
