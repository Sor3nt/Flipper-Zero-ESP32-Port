idf_component_register(
    SRCS "momentum_settings.c" "asset_packs.c" "icon_paths.c" "desktop_keybinds.c"
    INCLUDE_DIRS "."
    REQUIRES gui storage flipper_format
)
