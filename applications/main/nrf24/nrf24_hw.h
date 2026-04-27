#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Low-level NRF24L01(+) helper.
 * Bus + CS are shared with the CC1101 / LCD / SD-Card on T-Embed -- callers must
 * surround SPI access with nrf24_hw_acquire() / nrf24_hw_release(). The CE pin
 * is handled exclusively by this module.
 */

void nrf24_hw_init(void);
void nrf24_hw_deinit(void);

void nrf24_hw_acquire(void);
void nrf24_hw_release(void);

void nrf24_hw_write_reg(uint8_t reg, uint8_t value);
uint8_t nrf24_hw_read_reg(uint8_t reg);

void nrf24_hw_ce(bool high);

/* Power up the chip and run a register round-trip test.
 * Returns true if a module is present and responsive.
 * Must be wrapped in acquire/release. */
bool nrf24_hw_probe(void);

/* Power down (CONFIG = 0). Must be wrapped in acquire/release. */
void nrf24_hw_power_down(void);

/* Spectrum helper: tune to channel, listen briefly in RX, return RPD bit.
 * Must be wrapped in acquire/release. */
uint8_t nrf24_hw_listen_rpd(uint8_t channel);

/* Constant-carrier jammer: enable continuous wave on the given channel
 * at maximum PA power, 2 Mbps data rate, PLL locked.
 * Must be wrapped in acquire/release. CE is left HIGH on return. */
void nrf24_hw_jammer_start(uint8_t channel);

/* Retune the running carrier to a new channel without reconfiguring RF_SETUP.
 * Must be wrapped in acquire/release. */
void nrf24_hw_jammer_set_channel(uint8_t channel);

/* Stop the constant carrier and put the chip back into standby.
 * Must be wrapped in acquire/release. */
void nrf24_hw_jammer_stop(void);
