file(GLOB_RECURSE SUBGHZ_SOURCES
    "../../lib/subghz/*.c"
    "../../lib/drivers/cc1101.c"
    "../../applications/drivers/subghz/cc1101_ext/cc1101_ext.c"
    "../../applications/drivers/subghz/cc1101_ext/cc1101_ext_interconnect.c"
)

idf_component_register(
    SRCS ${SUBGHZ_SOURCES}
    INCLUDE_DIRS
        ".."
        "../../lib/drivers"
        "../../lib/subghz"
        "../../lib/subghz/blocks"
        "../../lib/subghz/devices"
        "../../lib/subghz/protocols"
    REQUIRES furi furi_hal toolbox mlib flipper_application storage flipper_format rpc
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=implicit-function-declaration
    -Wno-error=incompatible-pointer-types
    -Wno-error=int-conversion
)
