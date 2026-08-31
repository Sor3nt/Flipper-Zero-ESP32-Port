idf_component_register(
    SRCS
        "wlan_hal.c"
        "wlan_passwords.c"
        "wifi_settings.c"
        "wifi_service.c"
    INCLUDE_DIRS "."
    REQUIRES
        furi
        toolbox
        storage
        btshim
        gui
        esp_wifi
        esp_netif
        esp_event
        lwip
)
