/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define BWI_HW_FRAME_SIZE 64U
#define BWI_HW_ADDRESS 0x42U
#define BWI_HW_TRANSACTION_MS 100U
/* Called in IRQ context: fixed-size copy/codec only, never blocking I/O or radio. */
typedef void (*BwiHwWrite)(const uint8_t frame[BWI_HW_FRAME_SIZE]);
typedef void (*BwiHwRead)(uint8_t frame[BWI_HW_FRAME_SIZE]);
void bwi_hw_begin(BwiHwWrite write_callback, BwiHwRead read_callback);
/* Run every main-loop iteration. Recovery waits by returning, never spinning. */
void bwi_hw_poll(uint32_t now);
#ifdef __cplusplus
}
#endif
