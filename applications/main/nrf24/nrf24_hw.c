#include "nrf24_hw.h"

#include <furi_hal_gpio.h>
#include <furi_hal_power.h>
#include <furi_hal_resources.h>
#include <furi_hal_spi.h>
#include <furi_hal_spi_bus.h>
#include <furi_hal_nrf24.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <string.h>
#include <stdlib.h>
#include "boards/board.h"

#define TAG "Nrf24Hw"

/* Profiling: set to 1 to log a flood_burst wall-time histogram over serial every
 * 256 bursts. The shared host is yielded during FIFO airtime waits; the separate
 * NRF24 operation mutex remains held so another worker cannot retune the radio. */
#ifndef NRF24_FLOOD_PROFILE
#define NRF24_FLOOD_PROFILE 0
#endif

#define NRF_CMD_R_REGISTER  0x00
#define NRF_CMD_W_REGISTER  0x20
#define NRF_CMD_R_RX_PAYLOAD 0x61
#define NRF_CMD_W_TX_PAYLOAD 0xA0
#define NRF_CMD_FLUSH_TX     0xE1
#define NRF_CMD_FLUSH_RX     0xE2
#define NRF_CMD_REUSE_TX_PL  0xE3
#define NRF_CMD_NOP          0xFF

#define NRF_REG_CONFIG     0x00
#define NRF_REG_EN_AA      0x01
#define NRF_REG_EN_RXADDR  0x02
#define NRF_REG_SETUP_AW   0x03
#define NRF_REG_SETUP_RETR 0x04
#define NRF_REG_RF_CH      0x05
#define NRF_REG_RF_SETUP   0x06
#define NRF_REG_STATUS     0x07
#define NRF_REG_RPD        0x09
#define NRF_REG_RX_ADDR_P0 0x0A
#define NRF_REG_RX_ADDR_P1 0x0B
#define NRF_REG_RX_ADDR_P2 0x0C
#define NRF_REG_RX_ADDR_P3 0x0D
#define NRF_REG_RX_ADDR_P4 0x0E
#define NRF_REG_RX_ADDR_P5 0x0F
#define NRF_REG_TX_ADDR    0x10
#define NRF_REG_RX_PW_P0   0x11
#define NRF_REG_RX_PW_P1   0x12
#define NRF_REG_FIFO_STATUS 0x17
#define NRF_REG_DYNPD      0x1C
#define NRF_REG_FEATURE    0x1D
#define NRF_CONFIG_PWR_UP  0x02
#define NRF_CONFIG_PRIM_RX 0x01
#define NRF_CONFIG_EN_CRC  0x08

#define NRF_STATUS_RX_DR    0x40
#define NRF_STATUS_TX_DS    0x20
#define NRF_STATUS_MAX_RT   0x10

#define NRF_FIFO_RX_EMPTY   0x01
#define NRF_FIFO_TX_FULL    0x20

/* RF_SETUP for constant-carrier jammer:
 *   bit7 CONT_WAVE | bit4 PLL_LOCK | bit3 RF_DR_HIGH (2 Mbps) | bits2-1 RF_PWR=3 (max)
 *   = 0x80 | 0x10 | 0x08 | 0x06  =  0x9E */
#define NRF_RF_SETUP_JAMMER 0x9E
/* Normal 2 Mbps, max PA */
#define NRF_RF_SETUP_2M_MAX 0x0E

static bool nrf24_hw_read_reg_checked(uint8_t reg, uint8_t* out);

/* CE of the module routed for the current session: the board's CE, or the header pin the
 * user picked for an external module (copied at init, so the hot paths below keep a plain
 * &nrf24_ce). */
static GpioPin nrf24_ce = {.port = NULL, .pin = BOARD_PIN_NRF24_CE};
static const char* nrf24_route_error = NULL;
static FuriMutex* nrf24_operation_mutex = NULL;

static FuriMutex* nrf24_hw_get_operation_mutex(void) {
    static portMUX_TYPE create_lock = portMUX_INITIALIZER_UNLOCKED;
    FuriMutex* mutex = nrf24_operation_mutex;
    if(mutex) return mutex;

    FuriMutex* candidate = furi_mutex_alloc(FuriMutexTypeRecursive);
    furi_check(candidate);
    portENTER_CRITICAL(&create_lock);
    if(!nrf24_operation_mutex) nrf24_operation_mutex = candidate;
    mutex = nrf24_operation_mutex;
    portEXIT_CRITICAL(&create_lock);
    if(mutex != candidate) furi_mutex_free(candidate);
    return mutex;
}

static void nrf24_hw_operation_acquire(void) {
    furi_check(furi_mutex_acquire(nrf24_hw_get_operation_mutex(), FuriWaitForever) == FuriStatusOk);
    furi_hal_power_insomnia_enter();
}

static void nrf24_hw_operation_release(void) {
    furi_hal_power_insomnia_exit();
    furi_check(furi_mutex_release(nrf24_hw_get_operation_mutex()) == FuriStatusOk);
}

/* Keep the nRF24 operation mutex while yielding only the shared host. CE remains
 * high, so the radio continues its queued TX while LCD/SD/CC1101 use other CSs. */
static uint32_t nrf24_hw_yield_shared_bus(uint32_t delay_us) {
    const int64_t start_us = esp_timer_get_time();
    furi_hal_spi_release(&furi_hal_spi_bus_handle_nrf24);
    if(delay_us >= 1000U) {
        furi_delay_ms((delay_us + 999U) / 1000U);
    } else if(delay_us) {
        esp_rom_delay_us(delay_us);
    }
    taskYIELD();
    furi_hal_spi_acquire(&furi_hal_spi_bus_handle_nrf24);
    return (uint32_t)(esp_timer_get_time() - start_us);
}

void nrf24_hw_init(void) {
    /* Routes the SPI handle + CE to the chosen module (board or external) and leaves CE an
     * output driven low (PullUp + VeryHigh speed, as in the nRF24_jammer FAP). */
    const FuriHalNrf24ExtResult result = furi_hal_nrf24_session_begin();
    if(result == FuriHalNrf24ExtOk) {
        nrf24_route_error = NULL;
        nrf24_ce = *furi_hal_nrf24_get_ce_pin();
        ESP_LOGI(
            TAG,
            "NRF24 route: %s CSN=GPIO%u CE=GPIO%u",
            furi_hal_nrf24_session_is_external() ? "external" : "board",
            (unsigned)furi_hal_spi_bus_handle_nrf24.cs->pin,
            (unsigned)nrf24_ce.pin);
    } else {
        nrf24_route_error = furi_hal_nrf24_ext_result_text(result);
        ESP_LOGW(TAG, "NRF24 module not routed: %s", nrf24_route_error);
    }
}

void nrf24_hw_deinit(void) {
    furi_hal_nrf24_session_end();
    /* A header pin handed back is not ours to write any more (late CE writes would only
     * land on the board's own, idle, CE). */
    if(!furi_hal_nrf24_session_is_active()) nrf24_ce.pin = BOARD_PIN_NRF24_CE;
}

const char* nrf24_hw_route_error(void) {
    return nrf24_route_error;
}

void nrf24_hw_acquire(void) {
    nrf24_hw_operation_acquire();
    furi_hal_spi_acquire(&furi_hal_spi_bus_handle_nrf24);
}

void nrf24_hw_release(void) {
    furi_hal_spi_release(&furi_hal_spi_bus_handle_nrf24);
    nrf24_hw_operation_release();
}

static bool nrf24_hw_write_reg_checked(uint8_t reg, uint8_t value) {
    uint8_t tx[2] = {NRF_CMD_W_REGISTER | (reg & 0x1F), value};
    return furi_hal_spi_bus_tx(&furi_hal_spi_bus_handle_nrf24, tx, sizeof(tx), 100);
}

void nrf24_hw_write_reg(uint8_t reg, uint8_t value) {
    (void)nrf24_hw_write_reg_checked(reg, value);
}

uint8_t nrf24_hw_read_reg(uint8_t reg) {
    uint8_t tx[2] = {NRF_CMD_R_REGISTER | (reg & 0x1F), 0xFF};
    uint8_t rx[2] = {0};
    furi_hal_spi_bus_trx(&furi_hal_spi_bus_handle_nrf24, tx, rx, sizeof(tx), 100);
    return rx[1];
}

void nrf24_hw_write_buf(uint8_t reg, const uint8_t* data, uint8_t len) {
    uint8_t tx[1 + 32];
    if(len > 32) len = 32;
    tx[0] = NRF_CMD_W_REGISTER | (reg & 0x1F);
    memcpy(&tx[1], data, len);
    furi_hal_spi_bus_tx(&furi_hal_spi_bus_handle_nrf24, tx, 1 + len, 100);
}

void nrf24_hw_cmd(uint8_t cmd) {
    furi_hal_spi_bus_tx(&furi_hal_spi_bus_handle_nrf24, &cmd, 1, 100);
}

void nrf24_hw_ce(bool high) {
    furi_hal_gpio_write(&nrf24_ce, high);
}

bool nrf24_hw_probe(void) {
    /* The external module could not be routed (pin in use, bus busy): nothing to talk to,
     * and the SPI handle may still point at the board's module. */
    if(!furi_hal_nrf24_session_is_active()) return false;
    nrf24_hw_write_reg(NRF_REG_CONFIG, NRF_CONFIG_PWR_UP);
    nrf24_hw_yield_shared_bus(5000);
    /* Write-read two patterns to RF_CH. A single transient SPI glitch on the
     * shared bus used to fail the whole probe; retry a few times before giving
     * up and log the readback so a real wiring/bus fault is diagnosable. */
    for(uint32_t attempt = 0; attempt < 3; attempt++) {
        nrf24_hw_write_reg(NRF_REG_RF_CH, 0x55);
        const uint8_t r1 = nrf24_hw_read_reg(NRF_REG_RF_CH);
        nrf24_hw_write_reg(NRF_REG_RF_CH, 0x2A);
        const uint8_t r2 = nrf24_hw_read_reg(NRF_REG_RF_CH);
        if(r1 == 0x55 && r2 == 0x2A) {
            /* Keep the radio powered after a successful probe. RX consumers
             * configure their mode immediately afterwards and rely on the
             * oscillator already being up; powering down here made their first
             * listen window start before Tpd2stby had elapsed. */
            return true;
        }
        ESP_LOGW(
            TAG,
            "NRF24 probe attempt %lu: RF_CH readback 0x%02X/0x%02X (want 0x55/0x2A)",
            (unsigned long)attempt,
            r1,
            r2);
        nrf24_hw_yield_shared_bus(2000);
    }
    /* A failed probe must not leave an unresponsive module powered. */
    nrf24_hw_power_down();
    return false;
}

/* Put the radio in a defined state for energy-only RPD scanning: 2 Mbps (widest
 * RX bandwidth), auto-ack off, CRC on, and all RX pipes disabled. Packet
 * reception is unnecessary for an energy scan; disabling pipes prevents random
 * address matches from being accepted and latching RPD high. Without the
 * RF_SETUP write, a previous app (e.g. a 250 kbps jammer) narrows the RX
 * bandwidth and the RPD misses most activity.
 * Must be wrapped in acquire/release. Leaves the radio in powered RX standby
 * (CE low) so the following channel scan does not pay a cold-start delay. */
bool nrf24_hw_rpd_setup(void) {
    furi_hal_gpio_write(&nrf24_ce, false);
    /* The radio module can remain powered across an ESP32 soft reboot. Force
     * Power Down here so a stale RX/RPD state cannot leak into a new scan. */
    if(!nrf24_hw_write_reg_checked(NRF_REG_CONFIG, 0x00)) {
        FURI_LOG_W(TAG, "RPD setup could not reset the radio to Power Down");
        return false;
    }
    nrf24_hw_write_reg(NRF_REG_EN_AA, 0x00);
    nrf24_hw_write_reg(NRF_REG_EN_RXADDR, 0x00);
    nrf24_hw_write_reg(NRF_REG_SETUP_AW, 0x03);
    const uint8_t rx_addr[5] = {0xE7, 0xE7, 0xE7, 0xE7, 0xE7};
    nrf24_hw_write_buf(NRF_REG_RX_ADDR_P0, rx_addr, sizeof(rx_addr));
    nrf24_hw_write_reg(NRF_REG_RX_PW_P0, 32);
    nrf24_hw_write_reg(NRF_REG_RF_SETUP, NRF_RF_SETUP_2M_MAX);
    nrf24_hw_write_reg(NRF_REG_STATUS, NRF_STATUS_RX_DR | NRF_STATUS_TX_DS | NRF_STATUS_MAX_RT);
    nrf24_hw_cmd(NRF_CMD_FLUSH_RX);
    /* Ensure the receiver is in RX standby (PWR_UP | PRIM_RX | EN_CRC) and pay
     * any remaining Tpd2stby (~1.5 ms) HERE. A successful probe leaves the
     * radio powered, while callers that enter without probing may still need
     * a cold start. Warming here means every per-channel listen then pays only the
     * Standby->RX turn-on (Tstby2a, ~130 us). CE stays low, so this is Standby-I,
     * not active RX -- listen_rpd_begin() raises CE to actually receive. */
    nrf24_hw_write_reg(
        NRF_REG_CONFIG, NRF_CONFIG_PWR_UP | NRF_CONFIG_PRIM_RX | NRF_CONFIG_EN_CRC);
    nrf24_hw_yield_shared_bus(1500);
    uint8_t rf_setup = 0;
    uint8_t en_aa = 0;
    uint8_t en_rxaddr = 0;
    uint8_t setup_aw = 0;
    uint8_t config = 0;
    const bool read_ok = nrf24_hw_read_reg_checked(NRF_REG_RF_SETUP, &rf_setup) &&
                         nrf24_hw_read_reg_checked(NRF_REG_EN_AA, &en_aa) &&
                         nrf24_hw_read_reg_checked(NRF_REG_EN_RXADDR, &en_rxaddr) &&
                         nrf24_hw_read_reg_checked(NRF_REG_SETUP_AW, &setup_aw) &&
                         nrf24_hw_read_reg_checked(NRF_REG_CONFIG, &config);
    const bool setup_ok = read_ok && rf_setup == NRF_RF_SETUP_2M_MAX && en_aa == 0x00 &&
                          en_rxaddr == 0x00 && setup_aw == 0x03 &&
                          config == (NRF_CONFIG_PWR_UP | NRF_CONFIG_PRIM_RX | NRF_CONFIG_EN_CRC);
    if(!setup_ok) {
        FURI_LOG_W(
            TAG,
            "RPD setup verification failed: read_ok=%u RF_SETUP=0x%02X EN_AA=0x%02X EN_RXADDR=0x%02X AW=0x%02X CONFIG=0x%02X",
            (unsigned)read_ok,
            rf_setup,
            en_aa,
            en_rxaddr,
            setup_aw,
            config);
        nrf24_hw_power_down();
        return false;
    }

    ESP_LOGI(
        TAG,
        "RPD setup verified: RF_SETUP=0x%02X EN_AA=0x%02X EN_RXADDR=0x%02X AW=0x%02X CONFIG=0x%02X",
        rf_setup,
        en_aa,
        en_rxaddr,
        setup_aw,
        config);
    return true;
}

void nrf24_hw_power_down(void) {
    furi_hal_gpio_write(&nrf24_ce, false);
    nrf24_hw_write_reg(NRF_REG_CONFIG, 0x00);
}

void nrf24_hw_set_channel(uint8_t channel) {
    /* RF_CH is a 7-bit field (bit7 reserved); mask so a bad caller/state can
     * never set the reserved bit. Valid channels (0..125) are unaffected. */
    nrf24_hw_write_reg(NRF_REG_RF_CH, channel & 0x7F);
}

/* Phase 1 of an RPD listen: tune the channel and raise CE so the receiver
 * starts settling. Touches SPI → caller must hold the bus. CE is left HIGH. */
bool nrf24_hw_listen_rpd_begin(uint8_t channel) {
    /* Keep same-device access serialized and prevent light sleep while CE is
     * left high and the shared SPI bus is released for other devices. */
    nrf24_hw_operation_acquire();
    furi_hal_gpio_write(&nrf24_ce, false);
    if(!nrf24_hw_write_reg_checked(
           NRF_REG_CONFIG, NRF_CONFIG_PWR_UP | NRF_CONFIG_PRIM_RX | NRF_CONFIG_EN_CRC)) {
        nrf24_hw_operation_release();
        return false;
    }
    if(!nrf24_hw_write_reg_checked(NRF_REG_RF_CH, channel & 0x7F)) {
        nrf24_hw_operation_release();
        return false;
    }
    furi_hal_gpio_write(&nrf24_ce, true);
    return true;
}

bool nrf24_hw_listen_rpd_sample(uint8_t* rpd) {
    furi_check(rpd);
    uint8_t value = 0;
    bool ok = nrf24_hw_read_reg_checked(NRF_REG_RPD, &value);
    *rpd = ok ? (value & 0x01) : 0;
    return ok;
}

/* Phase 2: read the live RPD snapshot while the receiver is still in RX, then
 * drop CE to end the window. Nordic specifies that RPD is readable in RX mode;
 * sampling before CE falls avoids carrying an end-of-window latch from an
 * earlier channel into this sample. Touches SPI → caller must hold the bus.
 * The dwell between begin() and end() needs no SPI (CE is a dedicated GPIO),
 * so a shared-bus caller can release the bus for that wait and let the LCD draw. */
bool nrf24_hw_listen_rpd_end(uint8_t* rpd) {
    furi_check(rpd);
    uint8_t value = 0;
    const bool ok = nrf24_hw_read_reg_checked(NRF_REG_RPD, &value);
    *rpd = ok ? (value & 0x01) : 0;
    furi_hal_gpio_write(&nrf24_ce, false);
    /* Paired with the operation/sleep hold established by begin(). */
    nrf24_hw_operation_release();
    return ok;
}

/* Listen on one channel for `dwell_us` then read the RPD. Nordic requires the
 * 130 us standby-to-RX transition plus 40 us AGC settling before RPD is valid.
 * Must be wrapped in acquire/release. */
bool nrf24_hw_listen_rpd_dwell(uint8_t channel, uint32_t dwell_us, uint8_t* rpd) {
    furi_check(rpd);
    *rpd = 0;

    if(dwell_us < NRF24_RPD_SCAN_DWELL_US) dwell_us = NRF24_RPD_SCAN_DWELL_US;
    if(!nrf24_hw_listen_rpd_begin(channel)) return false;

    /* Nordic specifies 130 us RX settling plus 40 us AGC settling before RPD is
     * valid. The remaining dwell is sampled in short intervals for early hits. */
    esp_rom_delay_us(NRF24_RPD_MIN_VALID_DWELL_US);
    uint32_t remaining = dwell_us - NRF24_RPD_MIN_VALID_DWELL_US;
    uint8_t observed = 0;
    bool ok = true;
    while(remaining > 0) {
        const uint32_t step = remaining < 50 ? remaining : 50;
        esp_rom_delay_us(step);
        remaining -= step;
        uint8_t sample = 0;
        if(!nrf24_hw_listen_rpd_sample(&sample)) {
            ok = false;
            break;
        }
        if(sample) {
            observed = 1;
            break;
        }
    }

    uint8_t latched = 0;
    if(!nrf24_hw_listen_rpd_end(&latched)) ok = false;
    if(!ok) return false;

    *rpd = observed | latched;
    return true;
}

bool nrf24_hw_listen_rpd(uint8_t channel, uint8_t* rpd) {
    return nrf24_hw_listen_rpd_dwell(channel, NRF24_RPD_SCAN_DWELL_US, rpd);
}

/* Build an RF_SETUP byte for the given data rate (0=1M, 1=2M, 2=250k), PA level
 * (0..3 = MIN/LOW/HIGH/MAX) and optional continuous-wave + PLL-lock bits. */
static uint8_t nrf24_rf_setup_byte(uint8_t data_rate, uint8_t pa_level, bool cont_wave) {
    uint8_t v = (uint8_t)((pa_level & 0x03) << 1); /* RF_PWR (bits 2:1) */
    if(data_rate == 1) {
        v |= 0x08; /* RF_DR_HIGH → 2 Mbps */
    } else if(data_rate == 2) {
        v |= 0x20; /* RF_DR_LOW → 250 kbps */
    } /* data_rate == 0 → 1 Mbps: neither DR bit set */
    if(cont_wave) v |= 0x80 | 0x10; /* CONT_WAVE | PLL_LOCK */
    return v;
}

/* set_tx_mode: standby → clear IRQs → PWR_UP, PRIM_RX=0 → CE high. Byte-for-byte
 * the same as the nRF24_jammer FAP's nrf24_set_tx_mode(). */
static void nrf24_hw_tx_mode(void) {
    furi_hal_gpio_write(&nrf24_ce, false);
    nrf24_hw_write_reg(NRF_REG_STATUS, NRF_STATUS_TX_DS | NRF_STATUS_MAX_RT); /* 0x30 */
    uint8_t config = nrf24_hw_read_reg(NRF_REG_CONFIG);
    config &= ~NRF_CONFIG_PRIM_RX;
    config |= NRF_CONFIG_PWR_UP;
    nrf24_hw_write_reg(NRF_REG_CONFIG, config);
    furi_hal_gpio_write(&nrf24_ce, true);
}

void nrf24_hw_jammer_start_ex(uint8_t channel, uint8_t pa_level) {
    /* Exact replica of the nRF24_jammer FAP's nrf24_startConstCarrier(): a plain
     * CONT_WAVE carrier with CE held high — NO REUSE_TX_PL keep-alive. The FAP
     * radiates correctly on real nRF24L01+ modules relying only on CONT_WAVE, so
     * we do the same. The carrier stays on while CE is high; retuning only writes
     * RF_CH (see nrf24_hw_jammer_set_channel) and never touches CE. */

    /* Bring up TX mode; Tpd2stby (~1.5 ms) + PLL settle on a cold start. */
    nrf24_hw_tx_mode();
    nrf24_hw_yield_shared_bus(5000);

    /* Tune, then arm the continuous wave. RF_SETUP = 2 Mbps | PA | CONT_WAVE |
     * PLL_LOCK (power bits 2:1 from pa_level; 3=Max → 0b11, == FAP's level 6). */
    nrf24_hw_write_reg(NRF_REG_RF_CH, channel);
    nrf24_hw_write_reg(NRF_REG_RF_SETUP, nrf24_rf_setup_byte(1, pa_level, true));

    /* Disable auto-ack and CRC, exactly like the FAP const-carrier path. */
    nrf24_hw_write_reg(NRF_REG_EN_AA, 0x00);
    uint8_t config = nrf24_hw_read_reg(NRF_REG_CONFIG);
    config &= ~NRF_CONFIG_EN_CRC;
    nrf24_hw_write_reg(NRF_REG_CONFIG, config);

    /* Load a single dummy 0xFF payload (datasheet const-carrier procedure). */
    uint8_t tx[1 + 32];
    tx[0] = NRF_CMD_W_TX_PAYLOAD;
    memset(&tx[1], 0xFF, 32);
    furi_hal_spi_bus_tx(&furi_hal_spi_bus_handle_nrf24, tx, sizeof(tx), 100);

    /* Re-assert TX mode → PWR_UP, PRIM_RX=0, CE high: carrier is now radiating. */
    nrf24_hw_tx_mode();
}

void nrf24_hw_jammer_start(uint8_t channel) {
    nrf24_hw_jammer_start_ex(channel, 3 /* PA max */);
}

void nrf24_hw_jammer_set_channel(uint8_t channel) {
    /* Carrier stays on; just retune. PLL re-locks in ~130 µs. RF_CH is 7-bit. */
    nrf24_hw_write_reg(NRF_REG_RF_CH, channel & 0x7F);
}

void nrf24_hw_jammer_stop(void) {
    /* Datasheet: with CONT_WAVE + REUSE_TX_PL the chip ignores CE low — only
     * PWR_UP=0 turns TX off. Power down first, then clear the CW bits. */
    nrf24_hw_write_reg(NRF_REG_CONFIG, 0x00);
    nrf24_hw_write_reg(NRF_REG_RF_SETUP, NRF_RF_SETUP_2M_MAX);
    furi_hal_gpio_write(&nrf24_ce, false);
    nrf24_hw_cmd(NRF_CMD_FLUSH_TX);
}

/* ===== Data-flooding jammer ===== */

/* RF_SETUP for 250 kbps, PA max: RF_DR_LOW(bit5) | RF_PWR=3.
 * Longer on-air time per packet → blocks bigger time slices. */
#define NRF_RF_SETUP_250K_MAX 0x26

/* On-air time of one 32-byte flood frame (preamble + 3-byte addr + 256-bit
 * payload, no CRC ≈ 288 bits) at each data rate, used to size the flood_burst
 * per-phase deadline. The deadline MUST exceed the legitimate completion time
 * of a full burst or a healthy radio gets falsely flagged "stuck" and silently
 * transmits fewer packets than configured — which is exactly what a fixed 6 ms
 * did at 250 kbps (≈1.15 ms/packet → a 20-packet burst needs ~23 ms, far past
 * 6 ms). Set from the configured rate at flood_start; defaults to 2 Mbps. */
static volatile uint32_t s_flood_per_pkt_us = 144; /* 2 Mbps */

static uint32_t nrf24_flood_per_pkt_us(uint8_t data_rate) {
    switch(data_rate) {
    case 0: return 288; /* 1 Mbps  */
    case 2: return 1152; /* 250 kbps */
    case 1: /* 2 Mbps */
    default: return 144;
    }
}

/* Fill a 32-byte payload with pseudo-random bytes. Random garbage spreads the
 * modulation energy across the channel far better than a fixed periodic
 * pattern (which produces narrow spectral lines). */
static void nrf24_flood_fill_random(uint8_t* buf) {
    for(uint8_t i = 0; i < 32; i += 4) {
        uint32_t r = (uint32_t)rand();
        buf[i + 0] = (uint8_t)(r);
        buf[i + 1] = (uint8_t)(r >> 8);
        buf[i + 2] = (uint8_t)(r >> 16);
        buf[i + 3] = (uint8_t)(r >> 24);
    }
}

static void nrf24_flood_write_payload(void) {
    uint8_t tx[1 + 32];
    tx[0] = NRF_CMD_W_TX_PAYLOAD;
    nrf24_flood_fill_random(&tx[1]);
    furi_hal_spi_bus_tx(&furi_hal_spi_bus_handle_nrf24, tx, sizeof(tx), 100);
}

/* Bruce's 32-byte garbage base, XORed by the LFSR seed for the turbo pattern. */
static const uint8_t nrf24_flood_base[32] = {0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA,
                                             0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE,
                                             0xFF, 0x00, 0xFF, 0x00, 0xA5, 0x5A, 0xA5, 0x5A,
                                             0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};

/* Fill a 32-byte payload for a turbo phase:
 *   0 = LFSR-XOR varied base (broadband, defeats fixed CRC filters)
 *   1 = all 0xFF        (max RF energy per bit at 2 Mbps)
 *   2 = 0x55 / 0xAA     (max bit transitions)
 * Phase 0 advances a persistent 8-bit xorshift seed, mirroring Bruce. */
static void nrf24_flood_fill_phase(uint8_t* buf, uint8_t phase) {
    static uint8_t xor_seed = 0xA5;
    switch(phase) {
    case 1:
        memset(buf, 0xFF, 32);
        break;
    case 2:
        for(uint8_t i = 0; i < 32; i++) {
            buf[i] = (i & 1) ? 0xAA : 0x55;
        }
        break;
    default:
        for(uint8_t i = 0; i < 32; i++) {
            xor_seed ^= (uint8_t)(xor_seed << 5);
            xor_seed ^= (uint8_t)(xor_seed >> 3);
            buf[i] = nrf24_flood_base[i] ^ xor_seed;
        }
        break;
    }
}

static void nrf24_flood_write_payload_phase(uint8_t phase) {
    uint8_t tx[1 + 32];
    tx[0] = NRF_CMD_W_TX_PAYLOAD;
    nrf24_flood_fill_phase(&tx[1], phase);
    furi_hal_spi_bus_tx(&furi_hal_spi_bus_handle_nrf24, tx, sizeof(tx), 100);
}

void nrf24_hw_flood_start_ex(uint8_t channel, uint8_t data_rate, uint8_t pa_level) {
    furi_hal_gpio_write(&nrf24_ce, false);

    /* TX mode (PRIM_RX=0), CRC off. Wait Tpd2stby on this cold start. */
    nrf24_hw_write_reg(NRF_REG_CONFIG, NRF_CONFIG_PWR_UP);
    nrf24_hw_yield_shared_bus(5000);

    nrf24_hw_write_reg(NRF_REG_EN_AA, 0x00);
    nrf24_hw_write_reg(NRF_REG_SETUP_RETR, 0x00);
    nrf24_hw_write_reg(NRF_REG_SETUP_AW, 0x01); /* 3-byte address */
    nrf24_hw_write_reg(NRF_REG_EN_RXADDR, 0x01);
    nrf24_hw_write_reg(NRF_REG_RF_SETUP, nrf24_rf_setup_byte(data_rate, pa_level, false));
    nrf24_hw_write_reg(NRF_REG_DYNPD, 0x00);
    nrf24_hw_write_reg(NRF_REG_FEATURE, 0x00);

    /* Record this run's per-frame airtime so flood_burst sizes its deadline to
     * the actual data rate (see nrf24_flood_per_pkt_us). */
    s_flood_per_pkt_us = nrf24_flood_per_pkt_us(data_rate);

    const uint8_t addr[3] = {0xE7, 0xE7, 0xE7};
    nrf24_hw_write_buf(NRF_REG_TX_ADDR, addr, 3);
    nrf24_hw_write_buf(NRF_REG_RX_ADDR_P0, addr, 3);
    nrf24_hw_write_reg(NRF_REG_RX_PW_P0, 32);

    nrf24_hw_write_reg(NRF_REG_STATUS, NRF_STATUS_RX_DR | NRF_STATUS_TX_DS | NRF_STATUS_MAX_RT);
    nrf24_hw_cmd(NRF_CMD_FLUSH_TX);
    nrf24_hw_write_reg(NRF_REG_RF_CH, channel);
}

void nrf24_hw_flood_start(uint8_t channel, bool low_rate) {
    /* 0=1M, 1=2M, 2=250k → legacy bool maps to 2 Mbps / 250 kbps. */
    nrf24_hw_flood_start_ex(channel, low_rate ? 2 : 1, 3 /* PA max */);
}

void nrf24_hw_flood_channel(uint8_t channel) {
    /* CE low so we can safely retune and refill the FIFO. */
    furi_hal_gpio_write(&nrf24_ce, false);
    nrf24_hw_cmd(NRF_CMD_FLUSH_TX);
    nrf24_hw_write_reg(NRF_REG_RF_CH, channel);
    nrf24_hw_write_reg(NRF_REG_STATUS, NRF_STATUS_TX_DS | NRF_STATUS_MAX_RT);

    /* Fill the 3-deep TX FIFO with random garbage packets. */
    for(uint8_t i = 0; i < 3; i++) {
        nrf24_flood_write_payload();
    }

    /* Pulse CE high long enough for all three frames to go out
     * (~144 µs each at 2 Mbps for a 32-byte payload). */
    furi_hal_gpio_write(&nrf24_ce, true);
    nrf24_hw_yield_shared_bus(500);
    furi_hal_gpio_write(&nrf24_ce, false);
}

/* Diagnostic counter: how many times flood_burst() has hit its hard deadline
 * with the TX FIFO still reporting full (radio not actually draining the
 * FIFO). A healthy radio should essentially never hit this; if it climbs
 * continuously during Flood/Turbo jamming, the problem is downstream of this
 * function (CE not reaching the chip, bad RF_SETUP/PA combination for this
 * module, or an SPI readback issue) rather than anything in this loop. */
static volatile uint32_t nrf24_flood_stuck_count = 0;

uint32_t nrf24_hw_flood_stuck_count(void) {
    return nrf24_flood_stuck_count;
}

void nrf24_hw_flood_burst(uint8_t channel, uint8_t burst_count, bool turbo) {
    if(burst_count < 1) burst_count = 1;

#if NRF24_FLOOD_PROFILE
    const int64_t prof_burst_t0 = esp_timer_get_time();
#endif

    /* CE low so we can safely retune and prime the FIFO. */
    furi_hal_gpio_write(&nrf24_ce, false);
    nrf24_hw_cmd(NRF_CMD_FLUSH_TX);
    nrf24_hw_write_reg(NRF_REG_RF_CH, channel);
    nrf24_hw_write_reg(NRF_REG_STATUS, NRF_STATUS_TX_DS | NRF_STATUS_MAX_RT);

    /* Turbo cycles three spectral patterns (burst_count packets each); plain
     * flood streams a single random-garbage burst. CE stays HIGH the whole
     * time, topping up the 3-deep FIFO as it drains so frames go back-to-back.
     *
     * Hard wall-clock deadline (not an iteration guard), reset PER PHASE: if
     * the TX FIFO never drains -- e.g. CE isn't actually reaching the chip, or
     * the RF_SETUP/PA combination is invalid for this module -- the old
     * iteration-count guard (4000 iterations * 60us ~= 240ms, x3 phases for
     * Turbo ~= 720ms) could burn most of a second per call. The operation mutex
     * still protects radio state, but the shared SPI host is yielded during
     * FIFO airtime waits so unrelated devices and tasks can run.
     *
     * The deadline must be per-phase, not shared across all phases: at the
     * documented max burst_count (20) with on-air transmission as the
     * bottleneck (~144us/packet at 2Mbps, slower than the SPI write itself),
     * one legitimate healthy 2 Mbps phase can take up to ~20*144us =~ 2.9ms,
     * and Turbo's three phases can take ~8.6ms total. At 250 kbps, a healthy
     * phase can take about 23ms and Turbo about 69ms; host yielding prevents
     * those airtime intervals from monopolizing SPI2. A single combined deadline
     * tight enough to catch a stuck FIFO quickly would clip that case short and transmit
     * fewer packets than configured. The per-phase budget below is sized to the
     * data rate (see phase_budget_us): a ≥6ms floor for fast rates keeps >2x
     * margin at 2 Mbps while a genuinely stuck FIFO operation is still bounded
     * to at most 30ms per phase instead of hundreds of milliseconds. */
    const uint8_t phase_count = turbo ? 3 : 1;
    bool stuck = false;

    /* Scale the per-phase deadline to the configured data rate: a full burst
     * needs burst_count * per-frame airtime, so allow 2x that (margin for FIFO
     * top-up overhead) but keep the proven ≥6 ms floor for fast rates and cap
     * at 30 ms so a genuinely stuck FIFO operation can't monopolize the radio
     * operation mutex for long. The shared SPI host is yielded between polls.
     * At 2 Mbps this stays at the original 6 ms (20*144us*2 = 5.76ms < floor),
     * so the well-tested 2 Mbps path is unchanged; only slow rates grow it. */
    uint32_t phase_budget_us = (uint32_t)burst_count * s_flood_per_pkt_us * 2u;
    if(phase_budget_us < 6000u) phase_budget_us = 6000u;
    if(phase_budget_us > 30000u) phase_budget_us = 30000u;

    furi_hal_gpio_write(&nrf24_ce, true);
    for(uint8_t p = 0; p < phase_count && !stuck; p++) {
        int64_t phase_deadline =
            esp_timer_get_time() + (int64_t)phase_budget_us; /* this phase only */
        uint8_t sent = 0;
        while(sent < burst_count) {
            if(esp_timer_get_time() > phase_deadline) {
                stuck = true;
                break;
            }
            if(nrf24_hw_read_reg(NRF_REG_FIFO_STATUS) & NRF_FIFO_TX_FULL) {
                /* Let a frame drain with the shared host released. The operation
                 * mutex prevents another NRF24 worker from changing radio state. */
                const uint32_t yielded_us = nrf24_hw_yield_shared_bus(60);
                if(yielded_us > 60U) phase_deadline += (int64_t)(yielded_us - 60U);
                continue;
            }
            if(turbo) {
                nrf24_flood_write_payload_phase(p);
            } else {
                nrf24_flood_write_payload();
            }
            sent++;
        }
    }

    if(stuck) {
        uint32_t count = ++nrf24_flood_stuck_count;
        /* Log sparingly -- this can trigger every hop while stuck, and we do
         * not want logging itself to become another source of delay. */
        if((count & 0xFF) == 1) {
            ESP_LOGW(TAG, "flood_burst: TX FIFO not draining (stuck_count=%lu)",
                     (unsigned long)count);
        }
    }

    /* Let the FIFO tail radiate while the shared bus remains available. */
    nrf24_hw_yield_shared_bus(180);
    furi_hal_gpio_write(&nrf24_ce, false);

#if NRF24_FLOOD_PROFILE
    {
        static uint32_t prof_n = 0, prof_max_us = 0, prof_hist[5] = {0};
        const uint32_t prof_dur_us = (uint32_t)(esp_timer_get_time() - prof_burst_t0);
        const uint32_t prof_ms = prof_dur_us / 1000;
        if(prof_dur_us > prof_max_us) prof_max_us = prof_dur_us;
        prof_n++;
        if(prof_ms < 2)
            prof_hist[0]++;
        else if(prof_ms < 6)
            prof_hist[1]++;
        else if(prof_ms < 10)
            prof_hist[2]++;
        else if(prof_ms < 20)
            prof_hist[3]++;
        else
            prof_hist[4]++;
        if((prof_n & 0xFF) == 0) {
            ESP_LOGI(
                TAG,
                "flood_burst n=%lu max=%luus hist[<2/<6/<10/<20/>=20ms]=%lu/%lu/%lu/%lu/%lu stuck=%lu",
                (unsigned long)prof_n,
                (unsigned long)prof_max_us,
                (unsigned long)prof_hist[0],
                (unsigned long)prof_hist[1],
                (unsigned long)prof_hist[2],
                (unsigned long)prof_hist[3],
                (unsigned long)prof_hist[4],
                (unsigned long)nrf24_flood_stuck_count);
        }
    }
#endif
}

void nrf24_hw_flood_pump(void) {
    /* Refill any free FIFO slots, then keep CE high so the radio transmits the
     * queued packets back-to-back. With AutoAck off each sent packet is dropped
     * from the FIFO, so calling this in a tight loop keeps the FIFO ~full and
     * the carrier busy ~continuously on one channel. */
    for(uint8_t i = 0; i < 3; i++) {
        if(nrf24_hw_read_reg(NRF_REG_FIFO_STATUS) & NRF_FIFO_TX_FULL) break;
        nrf24_flood_write_payload();
    }
    nrf24_hw_write_reg(NRF_REG_STATUS, NRF_STATUS_TX_DS | NRF_STATUS_MAX_RT);
    furi_hal_gpio_write(&nrf24_ce, true);
}

void nrf24_hw_flood_stop(void) {
    furi_hal_gpio_write(&nrf24_ce, false);
    nrf24_hw_write_reg(NRF_REG_CONFIG, 0x00);
    nrf24_hw_cmd(NRF_CMD_FLUSH_TX);
}

/* ===== Promiscuous RX ===== */

/* Noise addresses used to maximize promiscuous packet pickup.
 * The 2-byte SETUP_AW=0 trick means anything starting with these
 * common preamble-friendly bytes will pass the address filter. */
static const uint8_t mj_noise_addr[6][2] = {
    {0x55, 0x55},
    {0xAA, 0xAA},
    {0xA0, 0xAA},
    {0xAB, 0xAA},
    {0xAC, 0xAA},
    {0xAD, 0xAA},
};

void nrf24_hw_rx_start_promiscuous(uint8_t channel) {
    furi_hal_gpio_write(&nrf24_ce, false);

    /* CRC off, PWR_UP, RX */
    nrf24_hw_write_reg(NRF_REG_CONFIG, NRF_CONFIG_PWR_UP | NRF_CONFIG_PRIM_RX);
    nrf24_hw_write_reg(NRF_REG_EN_AA, 0x00);
    nrf24_hw_write_reg(NRF_REG_EN_RXADDR, 0x3F); /* enable all 6 pipes */
    /* SETUP_AW=0 is illegal per datasheet -> 2-byte addresses (promisc trick). */
    nrf24_hw_write_reg(NRF_REG_SETUP_AW, 0x00);
    nrf24_hw_write_reg(NRF_REG_SETUP_RETR, 0x00);
    nrf24_hw_write_reg(NRF_REG_RF_CH, channel);
    nrf24_hw_write_reg(NRF_REG_RF_SETUP, NRF_RF_SETUP_2M_MAX);
    nrf24_hw_write_reg(NRF_REG_DYNPD, 0x00);
    nrf24_hw_write_reg(NRF_REG_FEATURE, 0x00);

    /* All pipes use 32-byte fixed payloads. */
    for(uint8_t i = 0; i < 6; i++) {
        nrf24_hw_write_reg(NRF_REG_RX_PW_P0 + i, 32);
    }

    /* Pipes 0/1: full 2-byte address. Pipes 2..5: only LSB (top 4 bits inherit P1). */
    nrf24_hw_write_buf(NRF_REG_RX_ADDR_P0, mj_noise_addr[0], 2);
    nrf24_hw_write_buf(NRF_REG_RX_ADDR_P1, mj_noise_addr[1], 2);
    nrf24_hw_write_reg(NRF_REG_RX_ADDR_P2, mj_noise_addr[2][0]);
    nrf24_hw_write_reg(NRF_REG_RX_ADDR_P3, mj_noise_addr[3][0]);
    nrf24_hw_write_reg(NRF_REG_RX_ADDR_P4, mj_noise_addr[4][0]);
    nrf24_hw_write_reg(NRF_REG_RX_ADDR_P5, mj_noise_addr[5][0]);

    /* Clear interrupt flags */
    nrf24_hw_write_reg(NRF_REG_STATUS, NRF_STATUS_RX_DR | NRF_STATUS_TX_DS | NRF_STATUS_MAX_RT);
    /* Drain any leftover FIFO data */
    nrf24_hw_cmd(NRF_CMD_FLUSH_RX);

    /* Settle PLL, then start listening while leaving SPI2 available. */
    nrf24_hw_yield_shared_bus(150);
    furi_hal_gpio_write(&nrf24_ce, true);
}

/* Read a register but report SPI success separately. On a failed transaction the
 * HAL zero-fills the RX buffer; for an active-low flag like RX_EMPTY that zero is
 * the WRONG default ("data ready"). Callers whose default matters on failure use
 * this instead of nrf24_hw_read_reg() so they can fail safe. */
static bool nrf24_hw_read_reg_checked(uint8_t reg, uint8_t* out) {
    uint8_t tx[2] = {NRF_CMD_R_REGISTER | (reg & 0x1F), 0xFF};
    uint8_t rx[2] = {0};
    bool ok = furi_hal_spi_bus_trx(&furi_hal_spi_bus_handle_nrf24, tx, rx, sizeof(tx), 100);
    *out = rx[1];
    return ok;
}

bool nrf24_hw_rx_data_ready(void) {
    uint8_t fifo = 0;
    /* Fail safe: a failed FIFO_STATUS read means "no data", never "data ready".
     * Otherwise the HAL's zero-fill would clear RX_EMPTY and report a phantom
     * packet that rx_read_payload would then hand up as all-zero data. */
    if(!nrf24_hw_read_reg_checked(NRF_REG_FIFO_STATUS, &fifo)) return false;
    return (fifo & NRF_FIFO_RX_EMPTY) == 0;
}

uint8_t nrf24_hw_rx_read_payload(uint8_t* buf, uint8_t maxlen) {
    if(maxlen > 32) maxlen = 32;
    uint8_t tx[1 + 32];
    uint8_t rx[1 + 32];
    tx[0] = NRF_CMD_R_RX_PAYLOAD;
    memset(&tx[1], NRF_CMD_NOP, maxlen);
    if(!furi_hal_spi_bus_trx(&furi_hal_spi_bus_handle_nrf24, tx, rx, 1 + maxlen, 100)) {
        /* SPI failed: return 0 so the caller does not mistake the HAL's
         * zero-filled buffer for a real (all-zero) packet. */
        memset(buf, 0, maxlen);
        return 0;
    }
    memcpy(buf, &rx[1], maxlen);
    /* Clear RX_DR */
    nrf24_hw_write_reg(NRF_REG_STATUS, NRF_STATUS_RX_DR);
    return maxlen;
}

void nrf24_hw_rx_stop(void) {
    furi_hal_gpio_write(&nrf24_ce, false);
    /* Stay PWR_UP; caller decides full power-down. */
}

/* ===== Targeted TX ===== */

void nrf24_hw_tx_setup(
    const uint8_t* addr,
    uint8_t addr_len,
    uint8_t channel,
    uint8_t payload_len) {
    if(addr_len < 3) addr_len = 3;
    if(addr_len > 5) addr_len = 5;
    if(payload_len > 32) payload_len = 32;

    furi_hal_gpio_write(&nrf24_ce, false);

    /* CRC off, PWR_UP, TX (PRIM_RX=0) */
    nrf24_hw_write_reg(NRF_REG_CONFIG, NRF_CONFIG_PWR_UP);
    nrf24_hw_write_reg(NRF_REG_EN_AA, 0x00);
    nrf24_hw_write_reg(NRF_REG_EN_RXADDR, 0x01);
    /* SETUP_AW: 1=3byte, 2=4byte, 3=5byte */
    nrf24_hw_write_reg(NRF_REG_SETUP_AW, addr_len - 2);
    nrf24_hw_write_reg(NRF_REG_SETUP_RETR, 0x00);
    nrf24_hw_write_reg(NRF_REG_RF_CH, channel);
    nrf24_hw_write_reg(NRF_REG_RF_SETUP, NRF_RF_SETUP_2M_MAX);
    nrf24_hw_write_reg(NRF_REG_DYNPD, 0x00);
    nrf24_hw_write_reg(NRF_REG_FEATURE, 0x00);

    /* nRF24 transmits LSB-first per byte but expects address MSB-first in the
     * register. The Bruce reference passes the address as captured from the
     * promiscuous sniff so we mirror that here. */
    nrf24_hw_write_buf(NRF_REG_TX_ADDR, addr, addr_len);
    nrf24_hw_write_buf(NRF_REG_RX_ADDR_P0, addr, addr_len);
    nrf24_hw_write_reg(NRF_REG_RX_PW_P0, payload_len);

    nrf24_hw_write_reg(NRF_REG_STATUS, NRF_STATUS_RX_DR | NRF_STATUS_TX_DS | NRF_STATUS_MAX_RT);
    nrf24_hw_cmd(NRF_CMD_FLUSH_TX);
    nrf24_hw_cmd(NRF_CMD_FLUSH_RX);

    /* Wait out Tpd2stby (~1.5 ms) + PLL settle. The radio may have been powered
     * down by the preceding feature, so this setup may be a cold start: without the wait
     * the first nrf24_hw_tx_send() pulses CE before the oscillator/PLL have
     * settled and that frame never radiates (TX_DS still asserts with AutoAck
     * off, so the loss is silent). The jammer/flood start paths already do this;
     * MouseJacker's targeted-TX path was missing it, dropping its first frames. */
    nrf24_hw_yield_shared_bus(1500);
}

bool nrf24_hw_tx_send(const uint8_t* data, uint8_t len) {
    if(len > 32) len = 32;

    /* Clear stale flags */
    nrf24_hw_write_reg(NRF_REG_STATUS, NRF_STATUS_TX_DS | NRF_STATUS_MAX_RT);

    /* Write payload to TX FIFO */
    uint8_t tx[1 + 32];
    tx[0] = NRF_CMD_W_TX_PAYLOAD;
    memcpy(&tx[1], data, len);
    furi_hal_spi_bus_tx(&furi_hal_spi_bus_handle_nrf24, tx, 1 + len, 100);

    /* Pulse CE >= 10 µs to start TX. */
    furi_hal_gpio_write(&nrf24_ce, true);
    esp_rom_delay_us(15);
    furi_hal_gpio_write(&nrf24_ce, false);

    /* Wait for TX_DS or MAX_RT. MouseJacker configures 2 Mbps with AutoAck
     * disabled, so MAX_RT cannot occur. Keep a 600 us active-poll budget for
     * these short (10/19-byte) frames, but yield the shared host between polls
     * and extend the deadline only by time spent waiting for other devices. */
    int64_t poll_deadline = esp_timer_get_time() + 600;
    for(uint16_t i = 0; i < 60 && esp_timer_get_time() <= poll_deadline; i++) {
        uint8_t st = nrf24_hw_read_reg(NRF_REG_STATUS);
        if(st & (NRF_STATUS_TX_DS | NRF_STATUS_MAX_RT)) {
            nrf24_hw_write_reg(NRF_REG_STATUS, NRF_STATUS_TX_DS | NRF_STATUS_MAX_RT);
            return (st & NRF_STATUS_TX_DS) != 0;
        }
        const uint32_t yielded_us = nrf24_hw_yield_shared_bus(10);
        if(yielded_us > 10U) poll_deadline += (int64_t)(yielded_us - 10U);
    }
    /* Timeout — flush to keep the FIFO clean. */
    nrf24_hw_cmd(NRF_CMD_FLUSH_TX);
    return false;
}

void nrf24_hw_tx_stop(void) {
    furi_hal_gpio_write(&nrf24_ce, false);
    nrf24_hw_cmd(NRF_CMD_FLUSH_TX);
}

void nrf24_hw_flush_rx(void) {
    nrf24_hw_cmd(NRF_CMD_FLUSH_RX);
}

void nrf24_hw_flush_tx(void) {
    nrf24_hw_cmd(NRF_CMD_FLUSH_TX);
}
