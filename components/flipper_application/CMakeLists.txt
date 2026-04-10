idf_component_register(
    SRCS
        "flipper_application/application_manifest.c"
        "flipper_application/elf/elf_file.c"
        "flipper_application/flipper_application.c"
        "flipper_application/plugins/composite_resolver.c"
        "flipper_application/plugins/plugin_manager.c"
        "flipper_application/firmware_api.c"
        "flipper_application/nfc_app_api.c"
        "flipper_application/api_hashtable/api_hashtable.c"
    INCLUDE_DIRS "."
    REQUIRES furi furi_hal storage toolbox gui notification dialogs flipper_format subghz assets
)
