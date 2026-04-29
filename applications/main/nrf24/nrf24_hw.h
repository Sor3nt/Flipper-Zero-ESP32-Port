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

/* Multi-byte register write (e.g. RX_ADDR_Px, TX_ADDR). */
void nrf24_hw_write_buf(uint8_t reg, const uint8_t* data, uint8_t len);

/* Issue a single-byte command (FLUSH_RX / FLUSH_TX / NOP / REUSE). */
void nrf24_hw_cmd(uint8_t cmd);

void nrf24_hw_ce(bool high);

/* Power up the chip and run a register round-trip test.
 * Returns true if a module is present and responsive.
 * Must be wrapped in acquire/release. */
bool nrf24_hw_probe(void);

/* Power down (CONFIG = 0). Must be wrapped in acquire/release. */
void nrf24_hw_power_down(void);

/* Set RF channel (0..125). Must be wrapped in acquire/release. */
void nrf24_hw_set_channel(uint8_t channel);

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

/* ---- MouseJack: Promiscuous RX ---- */

/* Configure the chip for promiscuous reception:
 *   - 2-byte address width (illegal SETUP_AW=0 trick)
 *   - AutoAck off, CRC off, 2 Mbps, 32-byte fixed payload
 *   - All 6 RX pipes opened with noise-addresses to maximize hit chance.
 * After this call CE is HIGH and the chip is listening on the given channel.
 * Must be wrapped in acquire/release. */
void nrf24_hw_rx_start_promiscuous(uint8_t channel);

/* True if the RX FIFO has at least one packet ready.
 * Must be wrapped in acquire/release. */
bool nrf24_hw_rx_data_ready(void);

/* Read up to maxlen bytes from the RX FIFO. Returns the number of bytes read
 * (always equal to the configured payload size, typically 32).
 * Must be wrapped in acquire/release. */
uint8_t nrf24_hw_rx_read_payload(uint8_t* buf, uint8_t maxlen);

/* Stop listening, drop CE, leave chip in PWR_UP standby.
 * Must be wrapped in acquire/release. */
void nrf24_hw_rx_stop(void);

/* ---- MouseJack: Targeted TX ---- */

/* Configure for unicast TX to a given target address (5-byte ESB):
 *   - AutoAck off, no retries, 2 Mbps, max PA, CRC off
 *   - addr_len bytes copied to TX_ADDR and RX_ADDR_P0
 *   - Sets RX_PW_P0 to payload_len.
 * After this call the chip is in TX-standby (CE low, ready for write+pulse).
 * Must be wrapped in acquire/release. */
void nrf24_hw_tx_setup(
    const uint8_t* addr,
    uint8_t addr_len,
    uint8_t channel,
    uint8_t payload_len);

/* Send a single payload (no ACK, no retransmit). Caller is responsible for
 * vendor-specific retransmit looping. Returns true if the TX FIFO accepted
 * the write and the PLL settled.
 * Must be wrapped in acquire/release. */
bool nrf24_hw_tx_send(const uint8_t* data, uint8_t len);

/* Power down, drop CE.
 * Must be wrapped in acquire/release. */
void nrf24_hw_tx_stop(void);

/* Flush helpers — useful between TX bursts. */
void nrf24_hw_flush_rx(void);
void nrf24_hw_flush_tx(void);
