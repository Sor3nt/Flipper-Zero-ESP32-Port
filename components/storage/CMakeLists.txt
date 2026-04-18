idf_component_register(
    SRCS "storage_ext_seed.c" "ext_defaults_embedded.c"
    INCLUDE_DIRS "."
    REQUIRES furi furi_hal toolbox
)
