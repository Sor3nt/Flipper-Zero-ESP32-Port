/**
 * @file furi_hal_subghz.h
 * SubGhz HAL API
 */

#pragma once

#include <lib/subghz/devices/preset.h>

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <toolbox/level_duration.h>
#include <furi_hal_gpio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FURI_HAL_SUBGHZ_ASYNC_TX_BUFFER_FULL (256u)
#define FURI_HAL_SUBGHZ_ASYNC_TX_BUFFER_HALF (FURI_HAL_SUBGHZ_ASYNC_TX_BUFFER_FULL / 2)
#define FURI_HAL_SUBGHZ_ASYNC_TX_GUARD_TIME  (999u)

typedef enum {
    FuriHalSubGhzPathIsolate,
    FuriHalSubGhzPath433,
    FuriHalSubGhzPath315,
    FuriHalSubGhzPath868,
} FuriHalSubGhzPath;

void furi_hal_subghz_set_async_mirror_pin(const GpioPin* pin);
const GpioPin* furi_hal_subghz_get_data_gpio(void);
void furi_hal_subghz_init(void);
void furi_hal_subghz_sleep(void);
void furi_hal_subghz_dump_state(void);
void furi_hal_subghz_load_custom_preset(const uint8_t* preset_data);
void furi_hal_subghz_load_registers(const uint8_t* data);
void furi_hal_subghz_load_patable(const uint8_t data[8]);
void furi_hal_subghz_write_packet(const uint8_t* data, uint8_t size);
bool furi_hal_subghz_rx_pipe_not_empty(void);
bool furi_hal_subghz_is_rx_data_crc_valid(void);
void furi_hal_subghz_read_packet(uint8_t* data, uint8_t* size);
void furi_hal_subghz_flush_rx(void);
void furi_hal_subghz_flush_tx(void);
void furi_hal_subghz_shutdown(void);
void furi_hal_subghz_reset(void);
void furi_hal_subghz_idle(void);
void furi_hal_subghz_rx(void);
bool furi_hal_subghz_tx(void);
float furi_hal_subghz_get_rssi(void);
uint8_t furi_hal_subghz_get_lqi(void);
bool furi_hal_subghz_is_frequency_valid(uint32_t value);
uint32_t furi_hal_subghz_set_frequency_and_path(uint32_t value);
bool furi_hal_subghz_is_tx_allowed(uint32_t value);
int32_t furi_hal_subghz_get_rolling_counter_mult(void);
void furi_hal_subghz_set_rolling_counter_mult(int32_t mult);
uint32_t furi_hal_subghz_set_frequency(uint32_t value);
void furi_hal_subghz_set_path(FuriHalSubGhzPath path);

typedef void (*FuriHalSubGhzCaptureCallback)(bool level, uint32_t duration, void* context);
void furi_hal_subghz_start_async_rx(FuriHalSubGhzCaptureCallback callback, void* context);
void furi_hal_subghz_stop_async_rx(void);

typedef LevelDuration (*FuriHalSubGhzAsyncTxCallback)(void* context);
bool furi_hal_subghz_start_async_tx(FuriHalSubGhzAsyncTxCallback callback, void* context);
bool furi_hal_subghz_is_async_tx_complete(void);
void furi_hal_subghz_stop_async_tx(void);

void furi_hal_subghz_set_ext_leds_and_amp(bool enabled);
bool furi_hal_subghz_get_ext_leds_and_amp(void);

#ifdef __cplusplus
}
#endif
