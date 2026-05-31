idf_component_register(
    SRCS "settings.c" "asset_packs.c" "namespoof.c"
    INCLUDE_DIRS "."
    REQUIRES furi furi_hal flipper_format storage gui
)
