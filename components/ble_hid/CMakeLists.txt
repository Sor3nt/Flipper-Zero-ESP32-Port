if(CONFIG_BT_NIMBLE_ENABLED)
    set(BLE_HID_SRCS "ble_hid_nimble.c")
    set(BLE_HID_REQUIRES bt)
else()
    set(BLE_HID_SRCS "ble_hid.c")
    set(BLE_HID_REQUIRES esp_hid bt)
endif()

idf_component_register(
    SRCS ${BLE_HID_SRCS}
    INCLUDE_DIRS "."
    REQUIRES ${BLE_HID_REQUIRES}
    PRIV_REQUIRES nvs_flash ble_core
)
