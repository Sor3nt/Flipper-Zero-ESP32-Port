/**
 * @file board.h
 * Board selection header — includes the active board definition.
 *
 * Select board via CMake: -DFLIPPER_BOARD=waveshare_c6_1.9
 * The build system defines BOARD_INCLUDE to the correct header path.
 */

#pragma once

#if defined(BOARD_INCLUDE)
/* CMake sets BOARD_INCLUDE to e.g. "boards/board_waveshare_c6_1.9.h" */
#include BOARD_INCLUDE
#elif defined(BOARD_WAVESHARE_C6_1_9)
#include "board_waveshare_c6_1.9.h"
#else
/* Default board */
#include "board_waveshare_c6_1.9.h"
#endif

/* Sanity check: every board must define these */
#ifndef BOARD_NAME
#error "Board header must define BOARD_NAME"
#endif
#ifndef BOARD_PIN_LCD_MOSI
#error "Board header must define LCD pins"
#endif
