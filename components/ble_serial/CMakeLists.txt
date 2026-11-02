if(CONFIG_BT_NIMBLE_ENABLED)
    set(BLE_SERIAL_SRCS "ble_serial_nimble.c")
else()
    set(BLE_SERIAL_SRCS "ble_serial.c")
endif()

idf_component_register(
    SRCS ${BLE_SERIAL_SRCS}
    INCLUDE_DIRS "."
    REQUIRES bt
    PRIV_REQUIRES nvs_flash furi_ble furi_hal flipper_protobuf ble_core esp_timer
)
