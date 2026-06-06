idf_component_register(
    SRCS
        "buddy_main.c"
        "buddy_espnow.c"
        "buddy_store.c"
        "buddy_node.c"
        "buddy_features.c"
        "buddy_hs_store.c"
        "buddy_hs_parser.c"
        "features/feat_identify.c"
        "features/feat_capture_hs.c"
    INCLUDE_DIRS
        "."
        "features"
    REQUIRES
        esp_wifi      # also provides esp_now.h
        esp_netif
        esp_event
        nvs_flash
        driver        # gpio
)
