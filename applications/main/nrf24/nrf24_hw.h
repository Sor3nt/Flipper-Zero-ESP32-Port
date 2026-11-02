#pragma once

#include <stdbool.h>
#include <stdint.h>

#define NRF24_RPD_MIN_VALID_DWELL_US 170U
#define NRF24_RPD_SCAN_DWELL_US 200U

/* Low-level NRF24L01(+) helper.
 * Bus + CS are shared with the CC1101 / LCD / SD-Card on Paradoxt -- callers must
 * surround SPI access with nrf24_hw_acquire() / nrf24_hw_release(). The CE pin
 * is handled exclusively by this module.
 */

/* nrf24_hw_init() opens an NRF24 session (furi_hal_nrf24): the SPI handle and CE are routed
 * to the module chosen in NRF24 > Radio Settings (the board's, or an external one on two
 * header pins) and nrf24_hw_deinit() hands every pin back. Boards other than paradoxt
 * always use their own module. If routing fails, nrf24_hw_probe() returns false and
 * nrf24_hw_route_error() says why. */
void nrf24_hw_init(void);
void nrf24_hw_deinit(void);
/* Text of the last nrf24_hw_init() routing failure, NULL when it succeeded. */
const char* nrf24_hw_route_error(void);

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

/* Spectrum helper: tune to channel and listen in RX. The dwell is clamped to
 * the RPD-valid minimum. Returns false when an SPI transaction fails. */
bool nrf24_hw_listen_rpd(uint8_t channel, uint8_t* rpd);

/* Split RPD listen for shared-bus boards: _begin tunes the channel and raises
 * CE (SPI → bus required), then keeps the NRF24 operation lock and sleep
 * inhibition until the paired _end. The caller may release the shared SPI host
 * during the dwell so LCD/SD/CC1101 can run. _end drops CE, reads the latched
 * result, and releases the operation/sleep hold. Both must run in the same
 * task; _begin failure releases its hold and leaves CE low. */
bool nrf24_hw_listen_rpd_begin(uint8_t channel);
/* Read RPD while CE remains high. Returns false if the SPI transaction fails. */
bool nrf24_hw_listen_rpd_sample(uint8_t* rpd);
bool nrf24_hw_listen_rpd_end(uint8_t* rpd);

/* Listen on `channel` for `dwell_us` (receiver settling + sample window), then
 * read RPD. Dwell is clamped to NRF24_RPD_SCAN_DWELL_US. Must be wrapped in
 * acquire/release. */
bool nrf24_hw_listen_rpd_dwell(uint8_t channel, uint32_t dwell_us, uint8_t* rpd);

/* Put the radio in a defined 2 Mbps, no-ack, no-CRC state for RPD/carrier
 * scanning. Must be wrapped in acquire/release. Leaves the chip powered in
 * RX standby with CE low. */
bool nrf24_hw_rpd_setup(void);

/* Constant-carrier jammer: enable continuous wave on the given channel
 * at maximum PA power, 2 Mbps data rate, PLL locked.
 * Must be wrapped in acquire/release. CE is left HIGH on return. */
void nrf24_hw_jammer_start(uint8_t channel);

/* As nrf24_hw_jammer_start but with a selectable PA level (0..3 =
 * MIN/LOW/HIGH/MAX). Must be wrapped in acquire/release. */
void nrf24_hw_jammer_start_ex(uint8_t channel, uint8_t pa_level);

/* Retune the running carrier to a new channel without reconfiguring RF_SETUP.
 * Must be wrapped in acquire/release. */
void nrf24_hw_jammer_set_channel(uint8_t channel);

/* Stop the constant carrier and put the chip back into standby.
 * Must be wrapped in acquire/release. */
void nrf24_hw_jammer_stop(void);

/* ---- Data-flooding jammer ----
 * Alternative strategy to the constant carrier: instead of an unmodulated
 * tone this floods the channel with full 32-byte ESB packets (AutoAck off,
 * CRC off, fixed address 0xE7E7E7) to cause packet collisions / CRC
 * corruption on the victim. Best for channel-specific protocols
 * (WiFi, BLE, Zigbee). Mirrors Bruce's writeFast() flooding mode. */

/* Configure the chip for TX flooding on the given channel.
 * low_rate selects 250 kbps (longer packets → block bigger time slices,
 * better against slow protocols) instead of the default 2 Mbps.
 * Must be wrapped in acquire/release. CE is left LOW. */
void nrf24_hw_flood_start(uint8_t channel, bool low_rate);

/* As nrf24_hw_flood_start but with explicit data rate (0=1M, 1=2M, 2=250k) and
 * PA level (0..3). Must be wrapped in acquire/release. CE is left LOW. */
void nrf24_hw_flood_start_ex(uint8_t channel, uint8_t data_rate, uint8_t pa_level);

/* Retune to a channel and fire one burst of random garbage packets (fills the
 * 3-deep TX FIFO and pulses CE). Used for channel-hopping flooding.
 * Must be wrapped in acquire/release. */
void nrf24_hw_flood_channel(uint8_t channel);

/* Retune to a channel and stream burst_count packets back-to-back (CE held
 * HIGH, FIFO topped up as it drains). When turbo is true three spectral
 * patterns are cycled (LFSR-varied random, all-ones, alternating 0x55/0xAA) —
 * burst_count packets per pattern — to defeat CRC filters and spread energy.
 * When false a single random-garbage burst is sent. Mirrors Bruce's
 * turboFloodChannel / floodChannel. Must be wrapped in acquire/release. */
void nrf24_hw_flood_burst(uint8_t channel, uint8_t burst_count, bool turbo);

/* Continuous single-channel flooding: top up the TX FIFO with random garbage
 * while it has room and hold CE HIGH, so the radio transmits back-to-back for
 * ~100% duty cycle. Call repeatedly in a tight loop (no channel change).
 * Must be wrapped in acquire/release. */
void nrf24_hw_flood_pump(void);

/* Stop flooding: power down, drop CE, flush TX FIFO.
 * Must be wrapped in acquire/release. */
void nrf24_hw_flood_stop(void);

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
 * (the configured payload size, typically 32), or 0 if the SPI read failed — in
 * which case buf is zeroed and no real packet was received, so the caller must
 * not treat it as data.
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

/* Stop TX: drop CE and flush the TX FIFO. The chip stays PWR_UP — the caller
 * decides when to power down (nrf24_hw_power_down()), mirroring nrf24_hw_rx_stop().
 * Must be wrapped in acquire/release. */
void nrf24_hw_tx_stop(void);

/* Flush helpers — useful between TX bursts. */
void nrf24_hw_flush_rx(void);
void nrf24_hw_flush_tx(void);
