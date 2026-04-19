idf_component_register(
    SRCS
        "dialogs/dialogs.c"
        "dialogs/dialogs_api.c"
        "dialogs/dialogs_module_file_browser.c"
        "dialogs/dialogs_module_message.c"
    INCLUDE_DIRS "."
    REQUIRES furi gui toolbox assets storage
)
