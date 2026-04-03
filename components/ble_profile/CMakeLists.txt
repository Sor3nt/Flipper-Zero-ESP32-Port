idf_component_register(
    SRCS
        "extra_profiles/hid_profile.c"
        "extra_profiles/serial_profile.c"
    INCLUDE_DIRS
        "."
        ".."
    REQUIRES furi furi_hal ble_hid ble_serial
)
