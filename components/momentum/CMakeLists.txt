idf_component_register(
    SRCS
        "../../lib/momentum/settings.c"
        "../../lib/momentum/namespoof.c"
        "../../lib/momentum/asset_packs.c"
        "rgb_backlight_shim.c"
    INCLUDE_DIRS "." "../../lib/momentum" "../../lib"
    REQUIRES furi furi_hal storage toolbox flipper_format gui assets u8g2
)
