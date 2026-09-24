#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <esp_err.h>

/* T-Embed only: UART TX=44/NRF CSN, RX=43/NRF CE. No radio TX commands.
 * Same task must call every operation. Hold the shared-pin lease and snapshot.
 * BW16 signal wires MUST be absent during open/close and boot/reset. */
typedef struct {
    const void* owner;
    void* device;
    bool locked, ready, failed, config_saved;
    uint8_t config_down;
} FuriHalBw16Guard;

/* Leaves SPI exclusively acquired until ready/close, including on failure. */
esp_err_t furi_hal_bw16_guard_open(FuriHalBw16Guard* guard, const void* owner);
esp_err_t furi_hal_bw16_guard_ready(FuriHalBw16Guard* guard);
/* Send only SCAN\n; exclude all hardware SPI traffic until TX is idle HIGH.
 * On failure disconnect the UART output and park CSN HIGH before unlocking. */
esp_err_t furi_hal_bw16_guard_scan(FuriHalBw16Guard* guard);
/* Signal wires must be disconnected. Parks both pins before removing the SPI
 * device; leaves NRF powered down and restores other CONFIG bits. Caller then
 * deletes the UART driver and restores GPIOs. */
esp_err_t furi_hal_bw16_guard_close(FuriHalBw16Guard* guard);
