file(GLOB_RECURSE INFRARED_SOURCES
    "../../lib/infrared/*.c"
)

idf_component_register(
    SRCS ${INFRARED_SOURCES}
    INCLUDE_DIRS
        "../../lib/infrared"
        "../../lib/infrared/encoder_decoder"
        "../../lib/infrared/signal"
        "../../lib/infrared/worker"
    REQUIRES furi furi_hal toolbox mlib storage flipper_format notification
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=implicit-function-declaration
    -Wno-error=incompatible-pointer-types
    -Wno-error=int-conversion
    -include "core/memmgr.h"
)
