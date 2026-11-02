if(CONFIG_BT_NIMBLE_ENABLED)
    set(BLE_CORE_SRCS "ble_core_nimble.c" "ble_nimble_host.c")
else()
    set(BLE_CORE_SRCS "ble_core.c")
endif()

idf_component_register(
    SRCS ${BLE_CORE_SRCS}
    INCLUDE_DIRS "."
    REQUIRES bt
    PRIV_REQUIRES nvs_flash esp_timer
)
