<<<<<<< HEAD
# All protocol encoders/decoders live under lib/infrared/** (Momentum-aligned).
# Note: CONFIGURE_DEPENDS is not used — ESP-IDF runs this file in script mode for
# dependency scanning, where CONFIGURE_DEPENDS is invalid on file(GLOB).
file(GLOB_RECURSE INFRARED_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/../../lib/infrared/*.c"
=======
file(GLOB_RECURSE INFRARED_SOURCES
    "../../lib/infrared/*.c"
>>>>>>> 05c91cb486590019377b94b79a37919e1c650685
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
