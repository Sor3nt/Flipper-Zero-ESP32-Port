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
    REQUIRES furi furi_hal storage toolbox gui notification dialogs flipper_format subghz assets mjs nfc bit_lib datetime mbedtls locale
)

# firmware_api.c ist die FAP-ABI-Symboltabelle und exportiert bewusst auch deprecated APIs
# (z.B. view_dispatcher_enable_queue) -> deprecated-Warnung hier gezielt unterdruecken.
# Ein #pragma scheidet aus: die Tabelle ist ein Array-Initializer, dort sind keine Statements erlaubt.
set_source_files_properties(
    "flipper_application/firmware_api.c"
    PROPERTIES COMPILE_OPTIONS "-Wno-deprecated-declarations"
)
