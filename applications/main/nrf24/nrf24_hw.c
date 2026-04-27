#include "nrf24_hw.h"

#include <furi_hal_gpio.h>
#include <furi_hal_resources.h>
#include <furi_hal_spi.h>
#include <furi_hal_spi_bus.h>
#include <esp_rom_sys.h>
#include "boards/board.h"

#define NRF_CMD_R_REGISTER 0x00
#define NRF_CMD_W_REGISTER 0x20

#define NRF_REG_CONFIG     0x00
#define NRF_REG_RF_CH      0x05
#define NRF_REG_RF_SETUP   0x06
#define NRF_REG_RPD        0x09

#define NRF_CONFIG_PWR_UP  0x02
#define NRF_CONFIG_PRIM_RX 0x01

/* RF_SETUP for constant-carrier jammer:
 *   bit7 CONT_WAVE | bit4 PLL_LOCK | bit3 RF_DR_HIGH (2 Mbps) | bits2-1 RF_PWR=3 (max)
 *   = 0x80 | 0x10 | 0x08 | 0x06  =  0x9E */
#define NRF_RF_SETUP_JAMMER 0x9E

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
    nrf24_hw_write_reg(NRF_REG_RF_SETUP, 0x0E);
    /* Power down */
    nrf24_hw_write_reg(NRF_REG_CONFIG, 0x00);
}
