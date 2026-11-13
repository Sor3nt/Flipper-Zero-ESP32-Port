/**
 * @file st25r3916_reg.c
 * @brief ST25R3916 register/FIFO access over ESP-IDF SPI.
 *
 * Ported from the Flipper Zero firmware (lib/drivers/st25r3916_reg.c).
 *
 * The original targets an STM32 SPI peripheral where the driver toggles the
 * CS line by hand and emits several `furi_hal_spi_bus_tx` calls inside one
 * CS-low window. On ESP-IDF the chip select is owned by the SPI driver and is
 * asserted for the duration of a single `spi_device_polling_transmit()` call,
 * so every logical ST25R3916 access (command byte(s) + payload) has to be
 * assembled into one contiguous transfer. That is what the helpers below do.
 *
 * All transfers go through `st25r3916_tx_buf`/`st25r3916_rx_buf`, which are
 * safe because the NFC HAL serialises chip access with its own mutex.
 */

#include "st25r3916_reg.h"

#include <furi.h>
#include <string.h>

#define ST25R3916_WRITE_MODE \
    (0U << 6) /*!< ST25R3916 Operation Mode: Write                                */
#define ST25R3916_READ_MODE \
    (1U << 6) /*!< ST25R3916 Operation Mode: Read                                 */
#define ST25R3916_CMD_MODE \
    (3U << 6) /*!< ST25R3916 Operation Mode: Direct Command                       */
#define ST25R3916_FIFO_LOAD \
    (0x80U) /*!< ST25R3916 Operation Mode: FIFO Load                            */
#define ST25R3916_FIFO_READ \
    (0x9FU) /*!< ST25R3916 Operation Mode: FIFO Read                            */
#define ST25R3916_PT_A_CONFIG_LOAD \
    (0xA0U) /*!< ST25R3916 Operation Mode: Passive Target Memory A-Config Load  */
#define ST25R3916_PT_F_CONFIG_LOAD \
    (0xA8U) /*!< ST25R3916 Operation Mode: Passive Target Memory F-Config Load  */
#define ST25R3916_PT_TSN_DATA_LOAD \
    (0xACU) /*!< ST25R3916 Operation Mode: Passive Target Memory TSN Load       */
#define ST25R3916_PT_MEM_READ \
    (0xBFU) /*!< ST25R3916 Operation Mode: Passive Target Memory Read           */

#define ST25R3916_CMD_LEN \
    (1U) /*!< ST25R3916 CMD length                                           */
#define ST25R3916_FIFO_DEPTH (512U)
#define ST25R3916_BUF_LEN \
    (2U +             \
     ST25R3916_FIFO_DEPTH) /*!< command prefix + FIFO payload                  */

#define ST25R3916_SPI_TIMEOUT 200

static const char* TAG = "St25r3916Reg";

static uint8_t st25r3916_tx_buf[ST25R3916_BUF_LEN];
static uint8_t st25r3916_rx_buf[ST25R3916_BUF_LEN];

/* furi_hal_spi_bus_trx() zero-fills the RX buffer and still reports success
 * when the ESP-IDF device handle is missing, and every caller here ignores its
 * return value. That combination looks exactly like a chip that answers 0x00 to
 * every register read, so make the distinction visible. */
static void st25r3916_check_handle(const FuriHalSpiBusHandle* handle) {
    if(!handle->initialized || !handle->device) {
        FURI_LOG_W(
            TAG,
            "SPI device not usable (initialized=%d device=%p): transfer is a no-op",
            (int)handle->initialized,
            handle->device);
    }
}

/** Write `prefix` (one or two command bytes) followed by `values` in a single
 *  CS-low SPI transaction. */
static void st25r3916_spi_write(
    const FuriHalSpiBusHandle* handle,
    const uint8_t* prefix,
    size_t prefix_len,
    const uint8_t* values,
    size_t length) {
    furi_check(prefix_len <= 2);
    furi_check((prefix_len + length) <= sizeof(st25r3916_tx_buf));

    st25r3916_check_handle(handle);

    memcpy(st25r3916_tx_buf, prefix, prefix_len);
    if(length) memcpy(&st25r3916_tx_buf[prefix_len], values, length);

    furi_hal_spi_bus_tx(handle, st25r3916_tx_buf, prefix_len + length, ST25R3916_SPI_TIMEOUT);
}

/** Write `prefix`, then clock `length` bytes in from the chip, all inside one
 *  CS-low SPI transaction. */
static void st25r3916_spi_read(
    const FuriHalSpiBusHandle* handle,
    const uint8_t* prefix,
    size_t prefix_len,
    uint8_t* values,
    size_t length) {
    furi_check(prefix_len <= 2);
    furi_check((prefix_len + length) <= sizeof(st25r3916_tx_buf));

    st25r3916_check_handle(handle);

    memcpy(st25r3916_tx_buf, prefix, prefix_len);
    memset(&st25r3916_tx_buf[prefix_len], 0xFF, length);

    furi_hal_spi_bus_trx(
        handle,
        st25r3916_tx_buf,
        st25r3916_rx_buf,
        prefix_len + length,
        ST25R3916_SPI_TIMEOUT);

    memcpy(values, &st25r3916_rx_buf[prefix_len], length);
}

void st25r3916_read_reg(const FuriHalSpiBusHandle* handle, uint8_t reg, uint8_t* val) {
    furi_check(handle);
    st25r3916_read_burst_regs(handle, reg, val, 1);
}

void st25r3916_read_burst_regs(
    const FuriHalSpiBusHandle* handle,
    uint8_t reg_start,
    uint8_t* values,
    uint8_t length) {
    furi_check(handle);
    furi_check(values);
    furi_check(length);

    uint8_t prefix[2];
    size_t prefix_len = 0;

    if(reg_start & ST25R3916_SPACE_B) {
        prefix[prefix_len++] = ST25R3916_CMD_SPACE_B_ACCESS;
    }
    prefix[prefix_len++] = (reg_start & ~ST25R3916_SPACE_B) | ST25R3916_READ_MODE;

    st25r3916_spi_read(handle, prefix, prefix_len, values, length);
}

void st25r3916_write_reg(const FuriHalSpiBusHandle* handle, uint8_t reg, uint8_t val) {
    furi_check(handle);
    uint8_t reg_val = val;
    st25r3916_write_burst_regs(handle, reg, &reg_val, 1);
}

void st25r3916_write_burst_regs(
    const FuriHalSpiBusHandle* handle,
    uint8_t reg_start,
    const uint8_t* values,
    uint8_t length) {
    furi_check(handle);
    furi_check(values);
    furi_check(length);

    uint8_t prefix[2];
    size_t prefix_len = 0;

    if(reg_start & ST25R3916_SPACE_B) {
        prefix[prefix_len++] = ST25R3916_CMD_SPACE_B_ACCESS;
    }
    prefix[prefix_len++] = (reg_start & ~ST25R3916_SPACE_B) | ST25R3916_WRITE_MODE;

    st25r3916_spi_write(handle, prefix, prefix_len, values, length);
}

void st25r3916_reg_write_fifo(
    const FuriHalSpiBusHandle* handle,
    const uint8_t* buff,
    size_t length) {
    furi_check(handle);
    furi_check(buff);
    furi_check(length);
    furi_check(length <= ST25R3916_FIFO_DEPTH);

    const uint8_t prefix[1] = {ST25R3916_FIFO_LOAD};
    st25r3916_spi_write(handle, prefix, 1, buff, length);
}

void st25r3916_reg_read_fifo(const FuriHalSpiBusHandle* handle, uint8_t* buff, size_t length) {
    furi_check(handle);
    furi_check(buff);
    furi_check(length);
    furi_check(length <= ST25R3916_FIFO_DEPTH);

    const uint8_t prefix[1] = {ST25R3916_FIFO_READ};
    st25r3916_spi_read(handle, prefix, 1, buff, length);
}

void st25r3916_write_pta_mem(
    const FuriHalSpiBusHandle* handle,
    const uint8_t* values,
    size_t length) {
    furi_check(handle);
    furi_check(values);
    furi_check(length);
    furi_check(length <= ST25R3916_PTM_LEN);

    const uint8_t prefix[1] = {ST25R3916_PT_A_CONFIG_LOAD};
    st25r3916_spi_write(handle, prefix, 1, values, length);
}

void st25r3916_read_pta_mem(const FuriHalSpiBusHandle* handle, uint8_t* buff, size_t length) {
    furi_check(handle);
    furi_check(buff);
    furi_check(length);
    furi_check(length <= ST25R3916_PTM_LEN);

    const uint8_t prefix[1] = {ST25R3916_PT_MEM_READ};
    /* The chip clocks out one dummy byte before the memory contents. */
    uint8_t tmp_buff[ST25R3916_PTM_LEN + 1];
    st25r3916_spi_read(handle, prefix, 1, tmp_buff, length + 1);
    memcpy(buff, tmp_buff + 1, length);
}

void st25r3916_write_ptf_mem(
    const FuriHalSpiBusHandle* handle,
    const uint8_t* values,
    size_t length) {
    furi_check(handle);
    furi_check(values);

    const uint8_t prefix[1] = {ST25R3916_PT_F_CONFIG_LOAD};
    st25r3916_spi_write(handle, prefix, 1, values, length);
}

void st25r3916_write_pttsn_mem(const FuriHalSpiBusHandle* handle, uint8_t* buff, size_t length) {
    furi_check(handle);
    furi_check(buff);

    const uint8_t prefix[1] = {ST25R3916_PT_TSN_DATA_LOAD};
    st25r3916_spi_write(handle, prefix, 1, buff, length);
}

void st25r3916_direct_cmd(const FuriHalSpiBusHandle* handle, uint8_t cmd) {
    furi_check(handle);

    const uint8_t prefix[1] = {cmd | ST25R3916_CMD_MODE};
    furi_hal_spi_bus_tx(handle, prefix, 1, ST25R3916_SPI_TIMEOUT);
}

void st25r3916_read_test_reg(const FuriHalSpiBusHandle* handle, uint8_t reg, uint8_t* val) {
    furi_check(handle);

    const uint8_t prefix[2] = {ST25R3916_CMD_TEST_ACCESS, reg | ST25R3916_READ_MODE};
    st25r3916_spi_read(handle, prefix, 2, val, 1);
}

void st25r3916_write_test_reg(const FuriHalSpiBusHandle* handle, uint8_t reg, uint8_t val) {
    furi_check(handle);

    const uint8_t prefix[2] = {ST25R3916_CMD_TEST_ACCESS, reg | ST25R3916_WRITE_MODE};
    st25r3916_spi_write(handle, prefix, 2, &val, 1);
}

void st25r3916_clear_reg_bits(const FuriHalSpiBusHandle* handle, uint8_t reg, uint8_t clr_mask) {
    furi_check(handle);

    uint8_t reg_val = 0;
    st25r3916_read_reg(handle, reg, &reg_val);
    if((reg_val & ~clr_mask) != reg_val) {
        reg_val &= ~clr_mask;
        st25r3916_write_reg(handle, reg, reg_val);
    }
}

void st25r3916_set_reg_bits(const FuriHalSpiBusHandle* handle, uint8_t reg, uint8_t set_mask) {
    furi_check(handle);

    uint8_t reg_val = 0;
    st25r3916_read_reg(handle, reg, &reg_val);
    if((reg_val | set_mask) != reg_val) {
        reg_val |= set_mask;
        st25r3916_write_reg(handle, reg, reg_val);
    }
}

void st25r3916_change_reg_bits(
    const FuriHalSpiBusHandle* handle,
    uint8_t reg,
    uint8_t mask,
    uint8_t value) {
    furi_check(handle);

    st25r3916_modify_reg(handle, reg, mask, (mask & value));
}

void st25r3916_modify_reg(
    const FuriHalSpiBusHandle* handle,
    uint8_t reg,
    uint8_t clr_mask,
    uint8_t set_mask) {
    furi_check(handle);

    uint8_t reg_val = 0;
    uint8_t new_val = 0;
    st25r3916_read_reg(handle, reg, &reg_val);
    new_val = (reg_val & ~clr_mask) | set_mask;
    if(new_val != reg_val) {
        st25r3916_write_reg(handle, reg, new_val);
    }
}

void st25r3916_change_test_reg_bits(
    const FuriHalSpiBusHandle* handle,
    uint8_t reg,
    uint8_t mask,
    uint8_t value) {
    furi_check(handle);

    uint8_t reg_val = 0;
    uint8_t new_val = 0;
    st25r3916_read_test_reg(handle, reg, &reg_val);
    new_val = (reg_val & ~mask) | (mask & value);
    if(new_val != reg_val) {
        st25r3916_write_test_reg(handle, reg, new_val);
    }
}

bool st25r3916_check_reg(const FuriHalSpiBusHandle* handle, uint8_t reg, uint8_t mask, uint8_t val) {
    furi_check(handle);

    uint8_t reg_val = 0;
    st25r3916_read_reg(handle, reg, &reg_val);
    return (reg_val & mask) == val;
}