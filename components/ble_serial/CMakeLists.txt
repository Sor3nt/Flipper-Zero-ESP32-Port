idf_component_register(
    SRCS
        "ble_serial.c"
    INCLUDE_DIRS "."
    REQUIRES esp_hid bt
    PRIV_REQUIRES nvs_flash furi_ble furi_hal
)
