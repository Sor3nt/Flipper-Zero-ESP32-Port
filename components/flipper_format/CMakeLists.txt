idf_component_register(
    SRCS
        "flipper_format.c"
        "flipper_format_stream.c"
    INCLUDE_DIRS "."
    REQUIRES furi toolbox storage
)
