idf_component_register(
    SRCS
        "../../lib/music_worker/music_worker.c"
    INCLUDE_DIRS "../../lib/music_worker"
    REQUIRES furi furi_hal storage flipper_format toolbox mlib
)

target_link_libraries(${COMPONENT_LIB} PRIVATE m)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=implicit-function-declaration
    -Wno-error=builtin-declaration-mismatch
)
