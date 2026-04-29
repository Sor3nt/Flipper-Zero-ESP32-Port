#include "nrf24_hw.h"

#include <furi_hal_gpio.h>
#include <furi_hal_resources.h>
#include <furi_hal_spi.h>
#include <furi_hal_spi_bus.h>
#include <esp_rom_sys.h>
#include <string.h>
#include "boards/board.h"

#define NRF_CMD_R_REGISTER  0x00
#define NRF_CMD_W_REGISTER  0x20
#define NRF_CMD_R_RX_PAYLOAD 0x61
#define NRF_CMD_W_TX_PAYLOAD 0xA0
#define NRF_CMD_FLUSH_TX     0xE1
#define NRF_CMD_FLUSH_RX     0xE2
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

/* RF_SETUP for constant-carrier jammer:
 *   bit7 CONT_WAVE | bit4 PLL_LOCK | bit3 RF_DR_HIGH (2 Mbps) | bits2-1 RF_PWR=3 (max)
 *   = 0x80 | 0x10 | 0x08 | 0x06  =  0x9E */
#define NRF_RF_SETUP_JAMMER 0x9E
/* Normal 2 Mbps, max PA */
#define NRF_RF_SETUP_2M_MAX 0x0E

static const GpioPin nrf24_ce = {.port = NULL, .pin = BOARD_PIN_NRF24_CE};

void nrf24_hw_init(void) {
    furi_hal_gpio_init_simple(&nrf24_ce, GpioModeOutputPushPull);
    furi_hal_gpio_write(&nrf24_ce, false);
}

void nrf24_hw_deinit(void) {
    furi_hal_gpio_write(&nrf24_ce, false);
    furi_hal_gpio_init_simple(&nrf24_ce, GpioModeAnalog);
}

void nrf24_hw_acquire(void) {
    furi_hal_spi_acquire(&furi_hal_spi_bus_handle_nrf24);
}

void nrf24_hw_release(void) {
    furi_hal_spi_release(&furi_hal_spi_bus_handle_nrf24);
}

void nrf24_hw_write_reg(uint8_t reg, uint8_t value) {
    uint8_t tx[2] = {NRF_CMD_W_REGISTER | (reg & 0x1F), value};
    furi_hal_spi_bus_tx(&furi_hal_spi_bus_handle_nrf24, tx, sizeof(tx), 100);
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
    nrf24_hw_write_reg(NRF_REG_CONFIG, NRF_CONFIG_PWR_UP);
    esp_rom_delay_us(5000);
    nrf24_hw_write_reg(NRF_REG_RF_CH, 0x55);
    if(nrf24_hw_read_reg(NRF_REG_RF_CH) != 0x55) return false;
    nrf24_hw_write_reg(NRF_REG_RF_CH, 0x2A);
    return nrf24_hw_read_reg(NRF_REG_RF_CH) == 0x2A;
}

void nrf24_hw_power_down(void) {
    furi_hal_gpio_write(&nrf24_ce, false);
    nrf24_hw_write_reg(NRF_REG_CONFIG, 0x00);
}

void nrf24_hw_set_channel(uint8_t channel) {
    nrf24_hw_write_reg(NRF_REG_RF_CH, channel);
}

uint8_t nrf24_hw_listen_rpd(uint8_t channel) {
    furi_hal_gpio_write(&nrf24_ce, false);
    nrf24_hw_write_reg(NRF_REG_CONFIG, NRF_CONFIG_PWR_UP | NRF_CONFIG_PRIM_RX);
    nrf24_hw_write_reg(NRF_REG_RF_CH, channel);
    furi_hal_gpio_write(&nrf24_ce, true);
    esp_rom_delay_us(140);
    furi_hal_gpio_write(&nrf24_ce, false);
    return nrf24_hw_read_reg(NRF_REG_RPD) & 0x01;
}

void nrf24_hw_jammer_start(uint8_t channel) {
    /* Standby */
    furi_hal_gpio_write(&nrf24_ce, false);
    /* Power up in TX mode */
    nrf24_hw_write_reg(NRF_REG_CONFIG, NRF_CONFIG_PWR_UP);
    /* Continuous carrier @ 2 Mbps, PA max, PLL locked */
    nrf24_hw_write_reg(NRF_REG_RF_SETUP, NRF_RF_SETUP_JAMMER);
    nrf24_hw_write_reg(NRF_REG_RF_CH, channel);
    /* Settle PLL */
    esp_rom_delay_us(150);
    furi_hal_gpio_write(&nrf24_ce, true);
}

void nrf24_hw_jammer_set_channel(uint8_t channel) {
    nrf24_hw_write_reg(NRF_REG_RF_CH, channel);
}

void nrf24_hw_jammer_stop(void) {
    furi_hal_gpio_write(&nrf24_ce, false);
    /* Restore default RF_SETUP (2 Mbps, max power, no CW) */
    nrf24_hw_write_reg(NRF_REG_RF_SETUP, NRF_RF_SETUP_2M_MAX);
    /* Power down */
    nrf24_hw_write_reg(NRF_REG_CONFIG, 0x00);
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

    /* Settle PLL, then start listening. */
    esp_rom_delay_us(150);
    furi_hal_gpio_write(&nrf24_ce, true);
}

bool nrf24_hw_rx_data_ready(void) {
    return (nrf24_hw_read_reg(NRF_REG_FIFO_STATUS) & NRF_FIFO_RX_EMPTY) == 0;
}

uint8_t nrf24_hw_rx_read_payload(uint8_t* buf, uint8_t maxlen) {
    if(maxlen > 32) maxlen = 32;
    uint8_t tx[1 + 32];
    uint8_t rx[1 + 32];
    tx[0] = NRF_CMD_R_RX_PAYLOAD;
    memset(&tx[1], NRF_CMD_NOP, maxlen);
    furi_hal_spi_bus_trx(&furi_hal_spi_bus_handle_nrf24, tx, rx, 1 + maxlen, 100);
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

    /* Wait for TX_DS or MAX_RT. With AA off MAX_RT cannot happen, so we poll
     * TX_DS with a short timeout (≈1 ms covers any 32-byte 2 Mbps frame). */
    for(uint16_t i = 0; i < 200; i++) {
        uint8_t st = nrf24_hw_read_reg(NRF_REG_STATUS);
        if(st & (NRF_STATUS_TX_DS | NRF_STATUS_MAX_RT)) {
            nrf24_hw_write_reg(NRF_REG_STATUS, NRF_STATUS_TX_DS | NRF_STATUS_MAX_RT);
            return (st & NRF_STATUS_TX_DS) != 0;
        }
        esp_rom_delay_us(10);
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
