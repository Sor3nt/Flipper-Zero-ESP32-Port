idf_component_register(
    SRCS
        "ble_hid.c"
    INCLUDE_DIRS "."
    REQUIRES esp_hid bt
    PRIV_REQUIRES nvs_flash
)
