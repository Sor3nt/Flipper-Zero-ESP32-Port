file(GLOB_RECURSE MJS_SOURCES "../../lib/mjs/*.c")

idf_component_register(
    SRCS ${MJS_SOURCES}
    INCLUDE_DIRS
        "../../lib/mjs"
        "../../lib/mjs/common"
        "../../lib/mjs/ffi"
    REQUIRES furi furi_hal toolbox mlib storage
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-redundant-decls
    -Wno-unused-function
    -Wno-calloc-transposed-args # vendored mjs: calloc(sizeof(x), 1) ist hier semantisch ok
    -Wno-error=implicit-function-declaration
    -Wno-error=incompatible-pointer-types
)
