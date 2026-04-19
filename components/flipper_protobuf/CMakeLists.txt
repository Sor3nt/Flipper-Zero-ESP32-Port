idf_component_register(
    SRCS
        "flipper.pb.c"
        "storage.pb.c"
        "system.pb.c"
        "application.pb.c"
        "gui.pb.c"
        "gpio.pb.c"
        "desktop.pb.c"
        "property.pb.c"
    INCLUDE_DIRS "."
    REQUIRES nanopb
)
