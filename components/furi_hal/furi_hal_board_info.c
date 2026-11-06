#include "furi_hal_board_info.h"

#include "boards/board.h"

#include <stdint.h>

/* A board opts in by defining BOARD_PORTED_APPS_SUPPORTED to 1 in its header. */
bool furi_hal_board_supports_ported_apps(void) {
#if defined(BOARD_PORTED_APPS_SUPPORTED) && BOARD_PORTED_APPS_SUPPORTED
    return true;
#else
    return false;
#endif
}

const char* furi_hal_board_get_name(void) {
    return BOARD_NAME;
}

FuriHalBoardInputLayout furi_hal_board_get_input_layout(void) {
#if defined(BOARD_INPUT_DPAD) && BOARD_INPUT_DPAD
    return FuriHalBoardInputLayoutDpad;
#elif defined(BOARD_HAS_ENCODER) && BOARD_HAS_ENCODER
    return FuriHalBoardInputLayoutEncoder;
#elif defined(BOARD_PIN_TOUCH_SDA)
    /* Boards without a panel set the touch pins to UINT16_MAX. */
    return (BOARD_PIN_TOUCH_SDA != UINT16_MAX) ? FuriHalBoardInputLayoutTouch :
                                                 FuriHalBoardInputLayoutButtons;
#else
    return FuriHalBoardInputLayoutButtons;
#endif
}
