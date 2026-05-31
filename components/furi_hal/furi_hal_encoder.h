#pragma once

#include "boards/board.h"

#ifdef BOARD_HAS_ENCODER

void furi_hal_encoder_init(void);
void furi_hal_encoder_deinit(void);

#endif // BOARD_HAS_ENCODER
