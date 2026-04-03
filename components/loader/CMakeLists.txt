idf_component_register(
    SRCS
        "loader.c"
        "loader_menu.c"
        "loader_applications.c"
        "apps/about_app.c"
    INCLUDE_DIRS "."
    REQUIRES furi gui toolbox assets storage archive dialogs flipper_application
)
