/**
 * @file furi_hal_subghz.h
 * SubGhz HAL API
 */

#pragma once

#include <lib/subghz/devices/preset.h>
#include <lib/drivers/cc1101.h>

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
bool furi_hal_subghz_rx(void);
bool furi_hal_subghz_tx(void);
bool furi_hal_subghz_wait_for_state(CC1101State state, uint32_t timeout_us);
/* Caller must hold furi_hal_spi_bus_handle_subghz with furi_hal_spi_acquire. */
bool furi_hal_subghz_wait_for_state_locked(CC1101State state, uint32_t timeout_us);
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
/** Start asynchronous RX. Returns false (and leaves the radio idle) if the radio
 * is busy, not connected, or did not enter RX. */
bool furi_hal_subghz_start_async_rx(FuriHalSubGhzCaptureCallback callback, void* context);
void furi_hal_subghz_stop_async_rx(void);

typedef LevelDuration (*FuriHalSubGhzAsyncTxCallback)(void* context);
bool furi_hal_subghz_start_async_tx(FuriHalSubGhzAsyncTxCallback callback, void* context);
bool furi_hal_subghz_is_async_tx_complete(void);
void furi_hal_subghz_stop_async_tx(void);

void furi_hal_subghz_set_ext_leds_and_amp(bool enabled);
bool furi_hal_subghz_get_ext_leds_and_amp(void);

/* ---- External CC1101 module (boards with BOARD_SUBGHZ_EXT_MODULE) ----
 * Shares the board radio's SPI bus; CS and GDO0 are two of the header GPIOs picked
 * by the user. The module is "attached" for the duration of a Sub-GHz session (the
 * cc1101_ext device begin/end): only then are its pins reserved and driven. */

typedef enum {
    FuriHalSubGhzExtOk,
    FuriHalSubGhzExtUnsupported, /* board has no external-module support */
    FuriHalSubGhzExtBadConfig, /* CS/GDO0 not two different header GPIOs */
    FuriHalSubGhzExtBusy, /* the radio is receiving/transmitting or its SPI handle is in use */
    FuriHalSubGhzExtPinInUse, /* a chosen pin is reserved by another feature (IR, PWM, ...) */
    FuriHalSubGhzExtNotDetected, /* no CC1101 answered on the chosen CS */
} FuriHalSubGhzExtResult;

typedef struct {
    bool use_external; /* preferred radio: true = external module, false = board radio */
    uint8_t cs_pin; /* header GPIO number used as the module's CSn */
    uint8_t g0_pin; /* header GPIO number used as the module's GDO0 (data) */
} FuriHalSubGhzExtConfig;

bool furi_hal_subghz_ext_supported(void);
/** Persistent (NVS) configuration; defaults are returned until it was stored once. */
void furi_hal_subghz_ext_get_config(FuriHalSubGhzExtConfig* config);
/** Validate + store. Changing the pins while the module is attached is refused (detach first). */
bool furi_hal_subghz_ext_set_config(const FuriHalSubGhzExtConfig* config);
/** true when `pin` is one of the header GPIOs the module may use. */
bool furi_hal_subghz_ext_pin_allowed(uint8_t pin);
/** true when the module is the chosen radio and `pin` is its CS or GDO0 (conflict checks). */
bool furi_hal_subghz_ext_uses_pin(uint8_t pin);

FuriHalSubGhzExtResult furi_hal_subghz_ext_attach(void);
void furi_hal_subghz_ext_detach(void);
bool furi_hal_subghz_ext_is_attached(void);
/** Briefly attach + detach to see whether a module answers (no-op result Ok when attached). */
FuriHalSubGhzExtResult furi_hal_subghz_ext_probe(void);
/** supported && the user chose the external module && it answers. */
bool furi_hal_subghz_ext_is_available(void);
const char* furi_hal_subghz_ext_result_text(FuriHalSubGhzExtResult result);

#ifdef __cplusplus
}
#endif
