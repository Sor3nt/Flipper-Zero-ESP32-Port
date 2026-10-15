#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <furi_hal_spi.h>

#define R_REGISTER         0x00
#define W_REGISTER         0x20
#define ACTIVATE           0x50
#define R_RX_PL_WID        0x60
#define R_RX_PAYLOAD       0x61
#define W_TX_PAYLOAD       0xA0
#define W_TX_PAYLOAD_NOACK 0xB0
#define W_ACK_PAYLOAD      0xA8
#define FLUSH_TX           0xE1
#define FLUSH_RX           0xE2
#define REUSE_TX_PL        0xE3
#define RF24_NOP           0xFF

#define REG_CONFIG      0x00
#define REG_EN_AA       0x01
#define REG_EN_RXADDR   0x02
#define REG_SETUP_AW    0x03
#define REG_SETUP_RETR  0x04
#define REG_DYNPD       0x1C
#define REG_FEATURE     0x1D
#define REG_RF_SETUP    0x06
#define REG_STATUS      0x07
#define REG_RX_ADDR_P0  0x0A
#define REG_RX_ADDR_P1  0x0B
#define REG_RX_ADDR_P2  0x0C
#define REG_RX_ADDR_P3  0x0D
#define REG_RX_ADDR_P4  0x0E
#define REG_RX_ADDR_P5  0x0F
#define REG_RF_CH       0x05
#define REG_TX_ADDR     0x10
#define REG_FIFO_STATUS 0x17
#define REG_RPD         0x09
#define REG_RX_P_NO     0x0E

#define RX_PW_P0 0x11
#define RX_PW_P1 0x12
#define RX_PW_P2 0x13
#define RX_PW_P3 0x14
#define RX_PW_P4 0x15
#define RX_PW_P5 0x16
#define RX_DR    0x40
#define TX_DS    0x20
#define MAX_RT   0x10
#define EN_DPL   0x04
#define PWR_UP   0x02
#define PRIM_RX  0x01

#define MAX_CHANNEL      125
#define MIN_CHANNEL      0
#define MIN_PIPE         0
#define MAX_PIPE         5
#define PIPE_QTY         6
#define MIN_PAYLOAD_SIZE ((uint8_t)1)
#define MAX_PAYLOAD_SIZE ((uint8_t)32)
#define MAX_CRC_LENGTH   2
#define MIN_MAC_SIZE     2
#define MAX_MAC_SIZE     5
#define MIN_ARD_SIZE     250 // Auto Retransmit Delay in µs
#define MAX_ARD_SIZE     4000 // Auto Retransmit Delay in µs
#define ARD_STEP         250
#define MAX_ARC_SIZE     15 //Auto Retransmit Count

#define nrf24_TIMEOUT 500

/* ------------------------------------------------------------------------- *
 * ESP32-Port (LilyGO T-Embed) hardware binding
 *   - CE  : GPIO43, driven manually (GpioPin defined in nrf24.c as
 *           `nrf24_ce_pin`), exactly like applications_user/nRF24_jammer.
 *   - CS  : owned by the ESP32 HW SPI driver (gpio_nrf24_cs) -- it is
 *           asserted automatically by the bus handle, so it must NEVER be
 *           initialised or toggled here (doing so breaks the module).
 *   - SPI : shared SPI2 bus exported to FAPs as
 *           `furi_hal_spi_bus_handle_nrf24`. The bus is acquired/released
 *           per transaction and its init/deinit is ref-counted.
 * ------------------------------------------------------------------------- */
#define nrf24_CE_PIN  (&nrf24_ce_pin)
#define nrf24_HANDLE  furi_hal_spi_bus_handle_nrf24

// Data rate options
#define DATA_RATE_1MBPS   ((uint8_t)0) // 1 Mbps (default)
#define DATA_RATE_2MBPS   ((uint8_t)1) // 2 Mbps
#define DATA_RATE_250KBPS ((uint8_t)2) // 250 kbps
typedef uint8_t nrf24_data_rate;

// Transmission power levels
#define TX_POWER_M18DBM ((uint8_t)0) // -18 dBm (lowest power)
#define TX_POWER_M12DBM ((uint8_t)1) // -12 dBm
#define TX_POWER_M6DBM  ((uint8_t)2) // -6 dBm
#define TX_POWER_0DBM   ((uint8_t)3) // 0 dBm (highest power)
typedef uint8_t nrf24_tx_power;

// Operating modes
#define NRF24_MODE_POWER_DOWN ((uint8_t)0) // Power Down Mode (low power consumption)
#define NRF24_MODE_STANDBY    ((uint8_t)1) // Standby Mode (ready to switch to TX or RX)
#define NRF24_MODE_RX         ((uint8_t)2) // Receive Mode (listening for incoming data)
#define NRF24_MODE_TX         ((uint8_t)3) // Transmit Mode (sending data)
typedef uint8_t nrf24_mode;

// Mac address size
#define ADDR_WIDTH_1_BYTE  ((uint8_t)1)
#define ADDR_WIDTH_2_BYTES ((uint8_t)2)
#define ADDR_WIDTH_3_BYTES ((uint8_t)3)
#define ADDR_WIDTH_4_BYTES ((uint8_t)4)
#define ADDR_WIDTH_5_BYTES ((uint8_t)5)
typedef uint8_t nrf24_addr_width;

// CRC lenght
#define CRC_DISABLED ((uint8_t)0)
#define CRC_1_BYTE   ((uint8_t)1)
#define CRC_2_BYTES  ((uint8_t)2)
typedef uint8_t nrf24_crc_lenght;

// Payload size 0=pipe disabled 33=dyn payload
typedef uint8_t nrf24_payload;

typedef struct NRF24L01_Config {
    uint8_t channel; // RF channel (0 - 125)
    nrf24_data_rate data_rate; // Transmission speed: 0 -> 1 Mbps, 1 -> 2 Mbps, 2 -> 250 kbps
    nrf24_tx_power
        tx_power; // Transmission power: 0 -> -18 dBm, 1 -> -12 dBm, 2 -> -6 dBm, 3 -> 0 dBm
    nrf24_crc_lenght crc_length; // CRC length: 1 or 2 bytes 0 -> disable CRC
    nrf24_addr_width mac_len; // Address width: 2, 3, 4, or 5 bytes
    uint8_t arc; // Auto retransmit count (0-15)
    uint16_t ard; // Retransmit delay (250 µs to 4000 µs)
    bool auto_ack[PIPE_QTY]; // Auto ACK aka "Enhanced ShockBurst"
    bool dynamic_payload[PIPE_QTY]; // Dynamic payload (requires auto_ack)
    bool ack_payload; // Return ACK + payload (requires dynamic payload)
    bool tx_no_ack; // Send "NO_ACK" flag with transmission

    // Transmission and reception addresses
    uint8_t* tx_addr; // Transmission address (TX address)

    // Reception addresses for pipes 0 to 5
    uint8_t* rx_addr
        [PIPE_QTY]; // Array to store full addresses for pipes 0 and 1, and only last byte for pipes 2 to 5

    uint8_t payload_size
        [PIPE_QTY]; // Fixed payload size (1 to 32 bytes) for each pipe (0 to 5) (unset if dynamic_payload is enabled)
} NRF24L01_Config;

/* Low level API */

/** Writes a single byte to a specified register
 *
 * @param      reg     - register
 * @param      data    - data to write
 *
 * @return     device status
 */
uint8_t nrf24_write_reg(uint8_t reg, uint8_t data);

/** Writes multiple bytes to a specified register
 *
 * @param      reg     - register
 * @param      data    - data to write
 * @param      size    - size of data to write
 *
 * @return     device status
 */
uint8_t nrf24_write_buf_reg(uint8_t reg, uint8_t* data, uint8_t size);

/** Reads multiple bytes from a specified register
 *
 * @param      reg     - register
 * @param[out] data    - pointer to data
 *
 * @return     device status
 */
uint8_t nrf24_read_buf_reg(uint8_t reg, uint8_t* data, uint8_t size);

/** Reads a single byte from a specified register
 *
 * @param      reg     - register
 *
 * @return     register value
 */
uint8_t nrf24_read_reg(uint8_t reg);

/** Sets the operational mode of the nRF24 device
 *
 * This function configures the mode of the nRF24 device based on the specified mode parameter.
 *
 * @param      mode - operational mode to set (NRF24_MODE_POWER_DOWN, NRF24_MODE_STANDBY, NRF24_MODE_RX, NRF24_MODE_TX)
 *
 * MODE_IDLE = 0,  Power Down Mode (low power consumption)
 * MODE_STANDBY,   Standby Mode (ready to switch to TX or RX)
 * MODE_RX,        Receive Mode (listening for incoming data)
 * MODE_TX         ransmit Mode (sending data)
 *
 * @return     device status
 */
uint8_t nrf24_set_mode(nrf24_mode mode);

/*=============================================================================================================*/

/* High level API */

/** Must call this before using any other nRF24 API
 *
 * This function initializes the SPI bus and any other necessary configurations before using
 * other nRF24 functions.
 */
void nrf24_init_bus();

/** Must call this when ending use of the nRF24 device
 *
 * This function deinitializes the SPI bus, freeing resources and resetting configurations as needed.
 */
void nrf24_deinit_bus();

/** Send flush rx command
 *
 * This command clears any pending received data in the RX buffer.
 *
 * @return     device status
 */
uint8_t nrf24_flush_rx();

/** Send flush tx command
 *
 * This command clears any pending data in the TX buffer.
 *
 * @return     device status
 */
uint8_t nrf24_flush_tx();

/** Gets the RX packet length for a specified data pipe
 *
 * This function retrieves the length of a received packet for a specified pipe.
 *
 * @param      pipe - index of the data pipe (0..5) for which to get the packet length
 *
 * @return     packet length in bytes for the specified data pipe
 */
uint8_t nrf24_get_packetlen(uint8_t pipe);

/** Sets the RX payload size for a specified data pipe
 *
 * Configures the size of the payload expected on a specified data pipe.
 *
 * @param      len  - payload size in bytes
 * @param      pipe - the data pipe number (0 to 5) for which to set the payload size
 *
 * @return     device status
 */
uint8_t nrf24_set_payload_size(uint8_t len, uint8_t pipe);

/** Gets the configured length of the MAC address
 *
 * Returns the length of the MAC address currently configured for the nRF24 device.
 *
 * @return     MAC address length in bytes
 */
nrf24_addr_width nrf24_get_maclen();

/** Sets the MAC address length
 *
 * Sets the length of the MAC address for the nRF24 device. The length must be between 2 and 5 bytes.
 *
 * @param      maclen - desired MAC address length in bytes
 *
 * @return     device status
 */
uint8_t nrf24_set_maclen(nrf24_addr_width maclen);

/** Reads the STATUS register to retrieve the current status flags
 *
 * This function retrieves the contents of the STATUS register, which includes flags for
 * data ready, data sent, and maximum retries.
 *
 * @return     value of the STATUS register
 */
uint8_t nrf24_status();

/** Gets the current data transfer rate setting
 *
 * Reads the RF_SETUP register to retrieve the current data transfer rate.
 *
 * @return     current transfer rate setting
 */
nrf24_data_rate nrf24_get_rate();

/** Sets the data transfer rate
 *
 * Configures the data transfer rate of the nRF24 device, with options for 1 Mbps, 2 Mbps, or 250 kbps.
 *
 * @param      rate - desired data rate setting
 *
 * @return     device status
 */
uint8_t nrf24_set_rate(nrf24_data_rate rate);

/** Sets the transmit power (RF_PWR)
 *
 * Configures the transmit power of the nRF24 device.
 *
 * @param      txpower - desired TX power setting (0 -> -18 dBm, 1 -> -12 dBm, 2 -> -6 dBm, 3 -> 0 dBm)
 *
 * @return     device status
 */
uint8_t nrf24_set_txpower(nrf24_tx_power txpower);

/** Gets the current RF channel
 *
 * Returns the channel number currently set for RF communication.
 *
 * @return     current channel number
 */
uint8_t nrf24_get_chan();

/** Sets the channel for RF communication
 *
 * Configures the RF channel used for communication. Channels range from 0 to 125, which translates
 * to frequencies between 2400 MHz and 2525 MHz.
 *
 * @param      chan - RF channel number
 *
 * @return     device status
 */
uint8_t nrf24_set_chan(uint8_t chan);

/** Gets the source MAC address for a specified data pipe
 *
 * Reads the MAC address associated with a specific RX data pipe.
 *
 * @param[out] mac  - buffer to store the retrieved MAC address
 * @param      pipe - the data pipe number (0 to 5) for which to retrieve the MAC address
 *
 * @return     device status
 */
nrf24_addr_width nrf24_get_rx_mac(uint8_t* mac, uint8_t pipe);

/** Sets the source MAC address for a specified data pipe
 *
 * Configures the MAC address for a specified RX data pipe.
 *
 * @param      mac - the mac address to set
 * @param      size - the size of the mac address (2 to 5)
 * @param      pipe - the pipe number (0 to 5)
 * 
 * @return     device status
 */
uint8_t nrf24_set_rx_mac(uint8_t* mac, nrf24_addr_width size, uint8_t pipe);

/** Gets the dest mac address
 *
 * Reads the TX MAC address currently configured for outgoing communication.
 *
 * @param[out] mac - the source mac address
 * 
 * @return     device status
 */
uint8_t nrf24_get_tx_mac(uint8_t* mac);

/** Sets the dest mac address
 *
 * Configures the TX MAC address for outgoing data transmissions.
 *
 * @param      mac - the mac address to set
 * @param      size - the size of the mac address (2 to 5)
 * 
 * @return     device status
 */
uint8_t nrf24_set_tx_mac(uint8_t* mac, uint8_t size);

/** Reads an RX packet from the nRF24 device
 *
 * Reads a received packet from the nRF24 and provides details about the packet.
 *
 * @param[out] packet      - pointer to buffer for the received packet content
 * @param[out] packetsize  - pointer to store the size of the received packet
 * @param[out] pipe        - pointer to store the data pipe number from which the packet was received
 * @param[in]  rpd_active  - flag to enable or disable the Received Power Detector (RPD)
 *
 * @return     true if a packet is received, false otherwise
 */
bool nrf24_rxpacket(uint8_t* packet, uint8_t* packetsize, uint8_t* pipe, bool rpd_active);

/** Writes an ACK payload for a specific pipe
 *
 * @param      pipe     - pipe number (0 to 5) to which the ACK payload is associated
 * @param      payload  - pointer to the payload data to send with the ACK
 * @param      size     - size of the payload data (maximum of 32 bytes)
 *
 * @return     device status
 */
uint8_t nrf24_write_ack_payload(uint8_t pipe, uint8_t* payload, uint8_t size);

/** Sends TX packet
 *
 * @param      packet - the packet contents
 * @param      size - packet size
 * @param      ack - boolean to determine whether an ACK is required for the packet or not
 * @param      ack_payload - boolean to determine if payload is inclued with ACK packet
 * 
 * @return     device status
 */
bool nrf24_txpacket(
    uint8_t* payload,
    uint8_t size,
    bool no_ack,
    uint8_t* ack_payload,
    uint8_t* ack_payload_size);

/** Configures the nRF24 device settings
 *
 * Applies configuration settings specified in an NRF24L01_Config structure. This function
 * allows common settings to be adjusted in a single call.
 *
 * @param      config - pointer to the configuration structure with settings to apply
 */
void nrf24_configure(NRF24L01_Config* config);

/** Logs all the device registers
 *
 * This function reads and logs all the registers of the NRF24L01+ for diagnostic purposes.
 * 
 */
void nrf24_log_all_registers();

/** Checks if the nRF24 device is connected
 *
 * This function verifies communication with the nRF24 device, ensuring it is connected and responsive.
 *
 * @return     true if the device is successfully connected, false otherwise
 */
bool nrf24_check_connected();

/** Finds an available RF channel between source and destination devices
 *
 * This function scans a specified range of RF channels to find an optimal channel with minimal interference.
 *
 * @param      srcmac       - source MAC address
 * @param      dstmac       - destination MAC address
 * @param      maclen       - length of the MAC address (between 2 and 5 bytes)
 * @param      rate         - data rate for communication (250 kbps, 1 Mbps, or 2 Mbps)
 * @param      min_channel  - minimum channel number in the scan range
 * @param      max_channel  - maximum channel number in the scan range
 *
 * @return     optimal channel number found within the specified range, or an error if no optimal channel is found
 */
uint8_t nrf24_find_channel(
    uint8_t* srcmac,
    uint8_t* dstmac,
    nrf24_addr_width maclen,
    nrf24_data_rate rate,
    uint8_t min_channel,
    uint8_t max_channel);
