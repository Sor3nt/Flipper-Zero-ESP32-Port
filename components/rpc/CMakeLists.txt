idf_component_register(
    SRCS
        "rpc.c"
        "rpc_system.c"
        "rpc_storage.c"
        "rpc_app.c"
        "rpc_gui.c"
    INCLUDE_DIRS "."
    REQUIRES furi furi_hal toolbox storage nanopb flipper_protobuf notification btshim datetime gui loader input assets
)
