idf_component_register(
    SRCS
        "btshim.c"
        "bt_settings.c"
        "bt_keys_storage.c"
        "bt_settings_app.c"
    INCLUDE_DIRS "."
    REQUIRES furi gui toolbox ble_hid ble_serial ble_profile furi_hal furi_ble notification storage rpc
)
