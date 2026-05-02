/**
 * @file board_esp32s3_generic.h
 * Generic ESP32-S3 board definition.
 *
 * Hardware-identical to the LilyGo T-Embed CC1101 — the only ESP32-S3 board
 * currently supported.  Used as the CI build target for generic S3 validation.
 */

#pragma once

#include "board_lilygo_t_embed_cc1101.h"

/* Override identity so logs and version strings show the generic name */
#undef BOARD_NAME
#define BOARD_NAME "ESP32-S3 Generic"
