/**
 * @file furi_hal_nrf24.h
 * NRF24L01(+) radio routing: the board's module or an external one on the header.
 *
 * Boards with BOARD_NRF24_EXT_MODULE (paradoxt only) can wire a second NRF24 module to the
 * radio SPI bus (the same MISO/MOSI/SCK as the board radios) and to two of the header GPIOs
 * (CSN and CE) picked by the user. The header pins belong to the module only for the length
 * of an NRF24 session (furi_hal_nrf24_session_begin() .. _end(), i.e. while an NRF24 scene
 * runs); outside a session they are ordinary free GPIOs. On every other board the external
 * module is not supported and the session is a plain pass-through of the board's module.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <furi_hal_gpio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FuriHalNrf24ExtOk,
    FuriHalNrf24ExtUnsupported, /* board has no external-module support */
    FuriHalNrf24ExtBadConfig, /* CSN/CE not two different header GPIOs */
    FuriHalNrf24ExtBusy, /* the NRF24 SPI handle is in the middle of a transaction */
    FuriHalNrf24ExtPinInUse, /* a chosen pin is reserved by another feature */
    FuriHalNrf24ExtNotDetected, /* no NRF24 answered on the chosen pins */
} FuriHalNrf24ExtResult;

typedef struct {
    bool use_external; /* preferred module: true = external, false = the board's */
    uint8_t csn_pin; /* header GPIO number used as the module's CSN */
    uint8_t ce_pin; /* header GPIO number used as the module's CE */
} FuriHalNrf24ExtConfig;

bool furi_hal_nrf24_ext_supported(void);
/** Persistent (NVS) configuration; defaults are returned until it was stored once. */
void furi_hal_nrf24_ext_get_config(FuriHalNrf24ExtConfig* config);
/** Validate + store. Refused (false) when the pins are not two different header GPIOs, when
 *  an external Sub-GHz module already uses one of them, or while a session holds the pins. */
bool furi_hal_nrf24_ext_set_config(const FuriHalNrf24ExtConfig* config);
bool furi_hal_nrf24_ext_pin_allowed(uint8_t pin);
/** true when the external module is the chosen one and `pin` is its CSN or CE. */
bool furi_hal_nrf24_ext_uses_pin(uint8_t pin);
const char* furi_hal_nrf24_ext_result_text(FuriHalNrf24ExtResult result);

/** Start using the NRF24: routes the SPI handle and the CE pin to the chosen module and
 *  leaves CE configured as an output driven low. Holds light sleep inhibition until the
 *  matching session_end because CE may be active while the shared SPI host is released.
 *  For the external module this also runs a register round-trip probe, like
 *  furi_hal_subghz_ext_attach(); a failure unwinds routing and releases the pins.
 *  Successful nested begins must be balanced with the same number of session_end calls. */
FuriHalNrf24ExtResult furi_hal_nrf24_session_begin(void);
/** Hand everything back: module powered down, CE idle, header pins released and unreserved.
 *  No-op when no session is open. */
void furi_hal_nrf24_session_end(void);
bool furi_hal_nrf24_session_is_active(void);
bool furi_hal_nrf24_session_is_external(void);
/** CE pin of the module routed by the current session (the board's CE outside one). */
const GpioPin* furi_hal_nrf24_get_ce_pin(void);

#ifdef __cplusplus
}
#endif
