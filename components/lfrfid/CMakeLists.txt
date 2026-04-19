file(GLOB_RECURSE LFRFID_SOURCES
    "../../lib/lfrfid/*.c"
)

idf_component_register(
    SRCS ${LFRFID_SOURCES}
    INCLUDE_DIRS
        "../../lib/lfrfid"
        "../../lib/lfrfid/protocols"
        "../../lib/lfrfid/tools"
        "../../lib"
        "../../applications/main"
        "../.."
    REQUIRES furi furi_hal toolbox mlib storage flipper_format bit_lib notification gui dialogs rpc
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=implicit-function-declaration
    -Wno-error=incompatible-pointer-types
    -Wno-error=int-conversion
    -Wno-error=unused-parameter
    -include "core/memmgr.h"
)
