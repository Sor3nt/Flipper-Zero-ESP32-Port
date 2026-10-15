#include "nrf24.h"
#include <furi_hal_resources.h>
#include <string.h>
#include "../helper.h"

/* ESP32-Port (LilyGO T-Embed): CE is wired to GPIO43 and is the ONLY line we
drive manually. CS (gpio_nrf24_cs / GPIO44) is owned by the HW SPI driver and
must not be touched from here. Mirrors applications_user/nRF24_jammer. */
static const GpioPin nrf24_ce_pin = {.port = NULL, .pin = 43};

/* Ref-count for the shared SPI2 bus: furi_hal_spi_bus_handle_init/deinit must
balance across (re)init of the radio, and the bus must never be held. */
static uint8_t nrf24_spi_users_count = 0;

uint8_t EMPTY_MAC[] = {0, 0, 0, 0, 0};

NRF24L01_Config find_channel_config = {
    .channel = 0,
    .data_rate = DATA_RATE_2MBPS,
    .tx_power = TX_POWER_0DBM,
    .crc_length = CRC_2_BYTES,
    .mac_len = ADDR_WIDTH_5_BYTES,
    .arc = 15,
    .ard = 500,
    .auto_ack = {true, false, false, false, false, false},
    .dynamic_payload = {true, false, false, false, false, false},
    .ack_payload = false,
    .tx_no_ack = false,
    .tx_addr = NULL,
    .rx_addr = {NULL, NULL, NULL, NULL, NULL, NULL},
    .payload_size = {MAX_PAYLOAD_SIZE, 0, 0, 0, 0, 0}};

void nrf24_init_bus() {
    /* ESP32-Port (T-Embed): the nRF24 lives on the shared SPI2 bus (LCD / SD /
     * SubGhz). Register the bus handle once, ref-counted, and DO NOT hold it:
     * acquire/release happen per transaction in nrf24_spi_trx(). Only CE stays a
     * manually driven GPIO. */
    if(nrf24_spi_users_count == 0) {
        furi_hal_spi_bus_handle_init(&nrf24_HANDLE);
    }
    nrf24_spi_users_count++;

    furi_hal_gpio_init(nrf24_CE_PIN, GpioModeOutputPushPull, GpioPullUp, GpioSpeedVeryHigh);
    furi_hal_gpio_write(nrf24_CE_PIN, false);
}

void nrf24_deinit_bus() {
    furi_hal_gpio_write(nrf24_CE_PIN, false);

    if(nrf24_spi_users_count > 0) nrf24_spi_users_count--;
    if(nrf24_spi_users_count == 0) {
        furi_hal_spi_bus_handle_deinit(&nrf24_HANDLE);
    }

    furi_hal_gpio_init(nrf24_CE_PIN, GpioModeAnalog, GpioPullNo, GpioSpeedLow);
}

void nrf24_spi_trx(uint8_t* tx, uint8_t* rx, uint8_t size) {
    /* CS (gpio_nrf24_cs) is asserted/deasserted by the HW SPI driver via the
     * bus handle's `cs` field -- we must NOT toggle it as a GPIO. Just grab the
     * shared bus for the duration of this transfer. */
    furi_hal_spi_acquire(&nrf24_HANDLE);
    furi_hal_spi_bus_trx(&nrf24_HANDLE, tx, rx, size, nrf24_TIMEOUT);
    furi_hal_spi_release(&nrf24_HANDLE);
}

uint8_t nrf24_write_reg(uint8_t reg, uint8_t data) {
    uint8_t tx[2] = {W_REGISTER | reg, data};
    uint8_t rx[2] = {0};
    nrf24_spi_trx(tx, rx, 2);
    return rx[0];
}

uint8_t nrf24_write_buf_reg(uint8_t reg, uint8_t* data, uint8_t size) {
    uint8_t tx[size + 1];
    uint8_t rx[size + 1];
    memset(rx, 0, size + 1);
    tx[0] = W_REGISTER | reg;
    memcpy(&tx[1], data, size);
    nrf24_spi_trx(tx, rx, size + 1);
    return rx[0];
}

uint8_t nrf24_read_buf_reg(uint8_t reg, uint8_t* data, uint8_t size) {
    uint8_t tx[size + 1];
    uint8_t rx[size + 1];
    tx[0] = reg;
    memset(&tx[1], 0, size);
    nrf24_spi_trx(tx, rx, size + 1);
    memcpy(data, &rx[1], size);
    return rx[0];
}

uint8_t nrf24_read_reg(uint8_t reg) {
    uint8_t tx[2] = {reg, RF24_NOP};
    uint8_t rx[2] = {0, 0};
    nrf24_spi_trx(tx, rx, 2);
    return rx[1];
}

uint8_t nrf24_flush_rx() {
    uint8_t tx = FLUSH_RX;
    uint8_t rx = 0;
    nrf24_spi_trx(&tx, &rx, 1);
    return rx;
}

uint8_t nrf24_flush_tx() {
    uint8_t tx = FLUSH_TX;
    uint8_t rx = 0;
    nrf24_spi_trx(&tx, &rx, 1);
    return rx;
}

nrf24_addr_width nrf24_get_maclen() {
    uint8_t maclen = nrf24_read_reg(REG_SETUP_AW);
    maclen &= 3;
    return (nrf24_addr_width)(maclen + 2);
}

uint8_t nrf24_set_maclen(nrf24_addr_width maclen) {
    if(maclen < MIN_MAC_SIZE || maclen > MAX_MAC_SIZE) return 0;
    uint8_t status = 0;
    status = nrf24_write_reg(REG_SETUP_AW, maclen - 2);
    return status;
}

uint8_t nrf24_status() {
    uint8_t tx = RF24_NOP;
    uint8_t status = 0;
    nrf24_spi_trx(&tx, &status, 1);
    return status;
}

nrf24_data_rate nrf24_get_rate() {
    uint8_t rate = nrf24_read_reg(REG_RF_SETUP);
    rate &= 0x28;
    if(rate == 0x20) // 250kbps
        return DATA_RATE_250KBPS;
    else if(rate == 0x08) // 2Mbps
        return DATA_RATE_2MBPS;
    else // 1Mbps
        return DATA_RATE_1MBPS;
}

uint8_t nrf24_set_rate(nrf24_data_rate rate) {
    uint8_t r6 = nrf24_read_reg(REG_RF_SETUP);
    uint8_t status = 0;

    r6 &= 0xd7; // Clear rate fields.

    if(rate == DATA_RATE_250KBPS) r6 |= 0x20;
    if(rate == DATA_RATE_2MBPS) r6 |= 0x08;

    status = nrf24_write_reg(REG_RF_SETUP, r6); // Write new rate.
    return status;
}

uint8_t nrf24_get_chan() {
    return nrf24_read_reg(REG_RF_CH);
}

uint8_t nrf24_set_chan(uint8_t chan) {
    if(chan > MAX_CHANNEL) return 0;
    uint8_t status;
    status = nrf24_write_reg(REG_RF_CH, chan);
    return status;
}

nrf24_tx_power nrf24_get_txpower() {
    uint8_t txpower = nrf24_read_reg(REG_RF_SETUP);
    txpower = (txpower >> 1) & 0x03;
    return (nrf24_tx_power)txpower;
}

uint8_t nrf24_set_txpower(nrf24_tx_power txpower) {
    uint8_t status = 0;
    uint8_t rf_setup = nrf24_read_reg(REG_RF_SETUP);
    rf_setup &= ~(0x06); // Clear bits 2 and 1 (RF_PWR)
    rf_setup |= ((uint8_t)txpower << 1); // set new tx power bits
    status = nrf24_write_reg(REG_RF_SETUP, rf_setup);
    return status;
}

uint8_t nrf24_set_crc(nrf24_crc_lenght crc) {
    if(crc > CRC_2_BYTES) return 0;

    uint8_t status = 0;
    uint8_t config = nrf24_read_reg(REG_CONFIG);

    if(crc == CRC_DISABLED) {
        config &= ~(0x08);
    } else {
        config |= 0x08;

        if(crc == CRC_2_BYTES) {
            config |= 0x04;
        } else {
            config &= ~(0x04);
        }
    }
    status = nrf24_write_reg(REG_CONFIG, config);

    return status;
}

nrf24_addr_width nrf24_get_rx_mac(uint8_t* mac, uint8_t pipe) {
    if(pipe > MAX_PIPE) return 0;
    nrf24_addr_width size = nrf24_get_maclen();
    if(pipe <= 1) {
        nrf24_read_buf_reg(REG_RX_ADDR_P0 + pipe, mac, size);
    } else {
        nrf24_read_buf_reg(REG_RX_ADDR_P1, mac, size);
        nrf24_read_buf_reg(REG_RX_ADDR_P0 + pipe, &mac[size - 1], 1);
    }

    return size;
}

uint8_t nrf24_set_rx_mac(uint8_t* mac, nrf24_addr_width size, uint8_t pipe) {
    if(pipe > MAX_PIPE || (size < MIN_MAC_SIZE && pipe <= 1) || size > MAX_MAC_SIZE) return 0;
    if(mac == NULL) return 0;

    uint8_t status = 0;
    uint8_t reg_addr = REG_RX_ADDR_P0 + pipe;
    uint8_t buffer_size = (pipe <= 1) ? size : ADDR_WIDTH_1_BYTE;
    status = nrf24_write_buf_reg(reg_addr, mac, buffer_size);

    return status;
}

nrf24_addr_width nrf24_get_tx_mac(uint8_t* mac) {
    uint8_t size = nrf24_get_maclen();
    nrf24_read_buf_reg(REG_TX_ADDR, mac, size);
    return size;
}

uint8_t nrf24_set_tx_mac(uint8_t* mac, nrf24_addr_width size) {
    if(size < MIN_MAC_SIZE || size > MAX_MAC_SIZE) return 0;
    uint8_t status = 0;
    nrf24_set_maclen(size);
    nrf24_write_buf_reg(REG_TX_ADDR, EMPTY_MAC, MAX_MAC_SIZE);
    status = nrf24_write_buf_reg(REG_TX_ADDR, mac, size);
    return status;
}

uint8_t nrf24_get_packetlen(uint8_t pipe) {
    if(pipe > MAX_PIPE) return 0;
    return nrf24_read_reg(RX_PW_P0 + pipe);
}

uint8_t nrf24_set_payload_size(uint8_t len, uint8_t pipe) {
    if(pipe > MAX_PIPE) return 0;
    if(len > MAX_PAYLOAD_SIZE) return 0;
    uint8_t status = 0;
    status = nrf24_write_reg(RX_PW_P0 + pipe, len);
    return status;
}

uint8_t nrf24_set_arc_ard(uint8_t arc, uint16_t ard) {
    if(arc > MAX_ARC_SIZE) return 0;
    if(ard < MIN_ARD_SIZE || ard > MAX_ARD_SIZE || ard % MIN_ARD_SIZE != 0) return 0;
    uint8_t status = 0;
    uint8_t ard_reg = (ard / MIN_ARD_SIZE) - 1;
    uint8_t re_tr = (ard_reg << 4) | (arc & 0x0f);
    status = nrf24_write_reg(REG_SETUP_RETR, re_tr);
    return status;
}

bool nrf24_rxpacket(uint8_t* packet, uint8_t* packetsize, uint8_t* pipe, bool rpd_active) {
    uint8_t status = nrf24_status();

    // No packet avaiable, exit
    if(!(status & RX_DR) && ((status & REG_RX_P_NO) == REG_RX_P_NO)) {
        return false;
    }
    // clear IRQ bit
    if(status & RX_DR) {
        nrf24_write_reg(REG_STATUS, RX_DR);
    }
    // get RPD status
    uint8_t rpd_value = nrf24_read_reg(REG_RPD);
    if(rpd_active && rpd_value == 0) {
        return false;
    }
    // get receive pipe number
    uint8_t pipe_num = (status & REG_RX_P_NO) >> 1;
    if(pipe_num > MAX_PIPE) {
        return false;
    }
    *pipe = pipe_num;
    // get payload size
    uint8_t size = 0;
    if(nrf24_read_reg(REG_DYNPD) & (1 << pipe_num)) {
        size = nrf24_read_reg(R_RX_PL_WID);
        if(size > MAX_PAYLOAD_SIZE) {
            nrf24_flush_rx();
            return false;
        }
    } else {
        size = nrf24_read_reg(RX_PW_P0 + pipe_num);
    }
    if(size > MAX_PAYLOAD_SIZE) {
        nrf24_flush_rx();
        *packetsize = 0;
        return false;
    }
    // get payload
    uint8_t tx_pl[size + 1];
    uint8_t rx_pl[size + 1];
    tx_pl[0] = R_RX_PAYLOAD;
    if(size > 0) {
        memset(&tx_pl[1], 0, size);
    }
    nrf24_spi_trx(tx_pl, rx_pl, size + 1);
    if(size > 0) {
        memcpy(packet, &rx_pl[1], size);
    }
    // return payload size
    *packetsize = size;

    return true;
}

uint8_t nrf24_write_ack_payload(uint8_t pipe, uint8_t* payload, uint8_t size) {
    if(size > MAX_PAYLOAD_SIZE) {
        size = MAX_PAYLOAD_SIZE;
    }
    if(pipe > MAX_PIPE) {
        return 0;
    }

    uint8_t tx_buffer[size + 1];
    uint8_t rx_buffer[size + 1];
    memset(rx_buffer, 0, size + 1);

    tx_buffer[0] = W_ACK_PAYLOAD | (pipe & 0x07);
    memcpy(&tx_buffer[1], payload, size);
    nrf24_spi_trx(tx_buffer, rx_buffer, size + 1);

    return rx_buffer[0];
}

// Return 0 when error
bool nrf24_txpacket(
    uint8_t* payload,
    uint8_t size,
    bool no_ack,
    uint8_t* ack_payload,
    uint8_t* ack_payload_size) {
    uint8_t status = 0;
    uint8_t spi_size = size + 1;
    bool ack_payload_enabled = ((ack_payload != NULL) && (ack_payload_size != NULL) && !no_ack);

    if(size > MAX_PAYLOAD_SIZE) return false;

    uint8_t tx[spi_size];
    uint8_t rx[spi_size];
    memset(tx, 0, spi_size);
    memset(rx, 0, spi_size);

    tx[0] = (no_ack == true) ? W_TX_PAYLOAD_NOACK : W_TX_PAYLOAD;

    memcpy(&tx[1], payload, size);
    nrf24_spi_trx(tx, rx, spi_size);

    nrf24_set_mode(NRF24_MODE_TX);

    const uint32_t started = furi_get_tick();
    while(!(status & (TX_DS | MAX_RT))) {
        status = nrf24_status();
        if(status == 0xFF || (furi_get_tick() - started) >= furi_ms_to_ticks(100)) {
            nrf24_set_mode(NRF24_MODE_STANDBY);
            nrf24_flush_tx();
            if(ack_payload_size) *ack_payload_size = 0;
            return false;
        }
        furi_delay_ms(1);
    }

    nrf24_set_mode(NRF24_MODE_STANDBY);

    if(status & MAX_RT) nrf24_flush_tx();

    // payload with ack received
    if(ack_payload_enabled && (status & RX_DR)) {
        *ack_payload_size = nrf24_read_reg(R_RX_PL_WID);
        if(*ack_payload_size > MAX_PAYLOAD_SIZE) {
            *ack_payload_size = 0;
            nrf24_flush_rx();
            return false;
        }
        nrf24_read_buf_reg(R_RX_PAYLOAD, ack_payload, *ack_payload_size);
    } else if(ack_payload_enabled) {
        *ack_payload_size = 0;
    }

    return (status & TX_DS) && (!ack_payload_enabled || (status & RX_DR));
}

uint8_t nrf24_set_mode(nrf24_mode mode) {
    uint8_t status = 0;
    uint8_t cfg = nrf24_read_reg(REG_CONFIG);

    switch(mode) {
    case NRF24_MODE_POWER_DOWN:
        cfg &= ~(PWR_UP); // PWR DOWN
        status = nrf24_write_reg(REG_CONFIG, cfg);
        furi_hal_gpio_write(nrf24_CE_PIN, false);
        break;

    case NRF24_MODE_STANDBY:
        furi_hal_gpio_write(nrf24_CE_PIN, false);
        if(!(cfg & PWR_UP)) {
            cfg |= PWR_UP; // PWR_UP
            status = nrf24_write_reg(REG_CONFIG, cfg);
            furi_delay_ms(2);
        }
        break;

    case NRF24_MODE_RX:
        cfg |= PRIM_RX; // PRIM_RX
        status = nrf24_write_reg(REG_CONFIG, cfg);
        nrf24_write_reg(REG_STATUS, TX_DS | MAX_RT | RX_DR);
        nrf24_flush_tx();
        furi_hal_gpio_write(nrf24_CE_PIN, true);
        furi_delay_us(150);
        break;

    case NRF24_MODE_TX:
        cfg &= ~(PRIM_RX); // disable RPIM_RX
        status = nrf24_write_reg(REG_CONFIG, cfg);
        nrf24_write_reg(REG_STATUS, TX_DS | MAX_RT | RX_DR);
        nrf24_flush_rx();
        furi_hal_gpio_write(nrf24_CE_PIN, true);
        furi_delay_us(150);
        break;
    }
    return status;
}

void nrf24_configure(NRF24L01_Config* config) {
    nrf24_set_mode(NRF24_MODE_POWER_DOWN); // power down nrf24
    nrf24_write_reg(REG_STATUS, 0x70); // clear interrupts
    nrf24_write_reg(REG_FEATURE, 0x00); // clear features register

    nrf24_set_chan(config->channel); // set channel
    nrf24_set_rate(config->data_rate); //set data rate
    nrf24_set_txpower(config->tx_power); // set transmit power
    nrf24_set_maclen(config->mac_len); // set adress width 2,3,4 or 5 bytes

    uint8_t autoAck = 0x00;
    uint8_t dynpd = 0x00;
    uint8_t en_rxaddr = 0x00;
    uint8_t reg_feature = 0x00;

    if(config->ack_payload) {
        reg_feature |= 0x02; // enable ack payload
        // activate auto ack end dyn payload on pipe 0 according datasheet
        config->dynamic_payload[0] = true;
        config->auto_ack[0] = true;
        if(config->ard < 500) {
            config->ard = 500; // set ARD to at least 500µs according datasheet
        }
    }

    for(uint8_t i = 0; i <= MAX_PIPE; i++) {
        if(config->dynamic_payload[i]) {
            dynpd |= (1 << i);
            autoAck |= (1 << i);
            en_rxaddr |= (1 << i);
            continue;
        }
        if(config->payload_size[i] > 0) {
            en_rxaddr |= (1 << i);
            if(config->auto_ack[i]) {
                autoAck |= (1 << i);
            }
        }
    }

    if(autoAck != 0x00) {
        if(config->crc_length == CRC_DISABLED) {
            config->crc_length = CRC_1_BYTE; // enable CRC
        }
        if(config->data_rate == DATA_RATE_250KBPS && config->ard < 500) {
            config->ard = 500; // set ARD to at least 500µs according datasheet
        }
    }

    if(dynpd != 0x00) {
        reg_feature |= 0x04; // enable dyn payload
    }

    if(config->tx_no_ack) // enable "NO_ACK" flag on tx frames
    {
        reg_feature |= 0x01;
    }

    nrf24_write_reg(REG_EN_AA, autoAck); // Set Shockburst on pipes
    nrf24_write_reg(REG_DYNPD, dynpd); // Set dynamic payload on pipes
    nrf24_write_reg(REG_EN_RXADDR, en_rxaddr); // Enable RX address on pipes
    nrf24_write_reg(REG_FEATURE, reg_feature); // Enable features

    // set Auto retransmit count and Retransmit delay
    nrf24_set_arc_ard(config->arc, config->ard);

    // flush FIFOs
    nrf24_flush_rx();
    nrf24_flush_tx();

    nrf24_set_crc(config->crc_length); // CRC lenght 1 or 2 bytes 0 -> disable

    if(config->tx_addr) nrf24_set_tx_mac(config->tx_addr, config->mac_len); // set tx adress

    for(uint8_t i = 0; i <= MAX_PIPE; i++) {
        nrf24_set_rx_mac(config->rx_addr[i], config->mac_len, i); // set rx adress for pipe i
        nrf24_set_payload_size(config->payload_size[i], i); // set fix payload size for pipe i
    }
    nrf24_set_mode(NRF24_MODE_STANDBY);
}

void nrf24_log_all_registers() {
    // Define the register names and addresses based on the nRF24L01+ register map
    const char* reg_names[] = {"CONFIG",     "EN_AA",      "EN_RXADDR",  "SETUP_AW",
                               "SETUP_RETR", "RF_CH",      "RF_SETUP",   "STATUS",
                               "OBSERVE_TX", "RPD",        "RX_ADDR_P0", "RX_ADDR_P1",
                               "RX_ADDR_P2", "RX_ADDR_P3", "RX_ADDR_P4", "RX_ADDR_P5",
                               "TX_ADDR",    "RX_PW_P0",   "RX_PW_P1",   "RX_PW_P2",
                               "RX_PW_P3",   "RX_PW_P4",   "RX_PW_P5",   "FIFO_STATUS",
                               "DYNPD",      "FEATURE"};

    const uint8_t reg_addresses[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                     0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11,
                                     0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x1C, 0x1D};

    uint8_t reg_value[5]; // Registers can be up to 5 bytes long

    for(uint8_t i = 0; i < sizeof(reg_addresses); i++) {
        uint8_t reg = reg_addresses[i];
        uint8_t size = (reg == REG_RX_ADDR_P0 || reg == REG_RX_ADDR_P1 || reg == REG_TX_ADDR) ?
                           5 :
                           1; // Multi-byte registers are RX_ADDR_P0 - TX_ADDR

        // Read the register value
        nrf24_read_buf_reg(reg, reg_value, size);

        // Print the register name and value in both hex and binary
        FURI_LOG_I("NRF24_INFO", "REG %s (0x%02X):", reg_names[i], reg);
        for(uint8_t j = 0; j < size; j++) {
            FURI_LOG_I(
                "NRF24_INFO",
                "  Value[%d]: 0x%02X (Binary: " BYTE_TO_BINARY_PATTERN ") (Dec: %u)",
                j,
                reg_value[j],
                BYTE_TO_BINARY(reg_value[j]),
                reg_value[j]);
        }
        FURI_LOG_I("NRF24_INFO", " ");
    }
}

bool nrf24_check_connected() {
    /* STATUS depends on IRQ/FIFO state: zero is legal, 0xff is not.
     * Probe the writable retransmit register, preserving the caller's setup.
     * Callers invoke this before starting RX or after a completed TX. */
    if(nrf24_status() & 0x80u) return false;
    uint8_t saved = nrf24_read_reg(REG_SETUP_RETR);
    nrf24_write_reg(REG_SETUP_RETR, 0x5A);
    bool first = nrf24_read_reg(REG_SETUP_RETR) == 0x5A;
    nrf24_write_reg(REG_SETUP_RETR, 0xA5);
    bool second = nrf24_read_reg(REG_SETUP_RETR) == 0xA5;
    nrf24_write_reg(REG_SETUP_RETR, saved);
    bool restored = nrf24_read_reg(REG_SETUP_RETR) == saved;
    return first && second && restored;
}

uint8_t nrf24_find_channel(
    uint8_t* srcmac,
    uint8_t* dstmac,
    nrf24_addr_width maclen,
    nrf24_data_rate rate,
    uint8_t min_channel,
    uint8_t max_channel) {
    uint8_t ping_packet[] = {0x0f, 0x0f, 0x0f, 0x0f};
    uint8_t ch;

    find_channel_config.data_rate = rate;
    find_channel_config.rx_addr[0] = srcmac;
    find_channel_config.tx_addr = dstmac;
    find_channel_config.mac_len = maclen;
    find_channel_config.channel = min_channel;

    nrf24_configure(&find_channel_config);

    for(ch = min_channel; ch <= max_channel; ch++) {
        nrf24_set_chan(ch);
        if(nrf24_txpacket(ping_packet, sizeof(ping_packet), false, NULL, NULL)) {
            return ch;
        }
    }

    return max_channel + 1; // Échec
}
