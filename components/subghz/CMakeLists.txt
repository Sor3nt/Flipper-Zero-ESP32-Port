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
        # NICHT "../../lib/subghz/blocks": dessen math.h enthaelt nur subghz-CRC-Helfer
        # (keine libm-Prototypen) und wuerde, global exportiert, jedes <math.h> ueberschatten
        # -> implizite Deklarationen von log/sqrt/sinf/floorf/... in furi_hal/js_app/nfc.
        # subghz-intern wird der Header relativ inkludiert ("math.h" / "../blocks/math.h"),
        # externe Nutzer ueber <lib/subghz/blocks/...> bzw. firmware_api ueber "blocks/math.h"
        # (relativ zu lib/subghz) -- keiner braucht diesen Pfad als -I.
        "../../lib/subghz/devices"
        "../../lib/subghz/protocols"
    REQUIRES furi furi_hal toolbox mlib flipper_application storage flipper_format rpc
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=implicit-function-declaration
    -Wno-error=incompatible-pointer-types
    -Wno-error=int-conversion
)
