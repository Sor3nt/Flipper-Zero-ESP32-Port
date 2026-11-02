#include "i2c_compat.h"

#include <driver/i2c.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

/* Floor for every bus timeout. Upstream asks for 3 ms in the scanner, but the call also waits
 * for the driver's bus lock, which the power service holds while it polls the BQ chips, so a
 * tight value reports present devices as absent. */
#define I2C_COMPAT_MIN_TIMEOUT_MS 20

/* The header only forward-declares the handle type; this port never had a real one. */
struct FuriHalI2cBusHandle {
    uint8_t reserved;
};

const FuriHalI2cBusHandle furi_hal_i2c_handle_external = {0};

const GpioPin i2c_compat_sniff_sda = {.port = NULL, .pin = I2C_SNIFF_SDA_PIN};
const GpioPin i2c_compat_sniff_scl = {.port = NULL, .pin = I2C_SNIFF_SCL_PIN};

static FuriMutex* i2c_compat_lock = NULL;
static volatile bool i2c_compat_blocked = false;

static void i2c_compat_bus_up(void);

void i2c_compat_init(void) {
    if(!i2c_compat_lock) i2c_compat_lock = furi_mutex_alloc(FuriMutexTypeNormal);
    /* Once per run: the legacy driver logs an error line every time install() finds the bus
     * already installed, which is the normal case here. */
    i2c_compat_bus_up();
}

void i2c_compat_deinit(void) {
    if(i2c_compat_lock) {
        furi_mutex_free(i2c_compat_lock);
        i2c_compat_lock = NULL;
    }
}

bool i2c_compat_write_blocked(void) {
    return i2c_compat_blocked;
}

static TickType_t i2c_compat_ticks(uint32_t timeout_ms) {
    if(timeout_ms < I2C_COMPAT_MIN_TIMEOUT_MS) timeout_ms = I2C_COMPAT_MIN_TIMEOUT_MS;
    return pdMS_TO_TICKS(timeout_ms);
}

static bool i2c_compat_is_protected(uint8_t addr7) {
    return addr7 == I2C_ADDR_BQ25896 || addr7 == I2C_ADDR_BQ27220;
}

/* Bring the bus up if nothing else has. The power service normally installs it at boot on
 * the same pins; in that case it is reused untouched (reconfiguring a live bus could glitch a
 * transfer the power service has in flight). */
static void i2c_compat_bus_up(void) {
    esp_err_t err = i2c_driver_install(I2C_BUS_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if(err == ESP_OK) {
        i2c_config_t conf = {
            .mode = I2C_MODE_MASTER,
            .sda_io_num = I2C_BUS_SDA_PIN,
            .scl_io_num = I2C_BUS_SCL_PIN,
            .sda_pullup_en = GPIO_PULLUP_ENABLE,
            .scl_pullup_en = GPIO_PULLUP_ENABLE,
            .master.clk_speed = 100000,
        };
        i2c_param_config(I2C_BUS_PORT, &conf);
    }
}

void furi_hal_i2c_acquire(const FuriHalI2cBusHandle* handle) {
    UNUSED(handle);
    if(i2c_compat_lock) furi_mutex_acquire(i2c_compat_lock, FuriWaitForever);
}

void furi_hal_i2c_release(const FuriHalI2cBusHandle* handle) {
    UNUSED(handle);
    /* The bus is shared with the power ICs and NFC: never uninstall it. */
    if(i2c_compat_lock) furi_mutex_release(i2c_compat_lock);
}

/* Upstream Flipper addresses are 8-bit (7-bit << 1). */
bool furi_hal_i2c_is_device_ready(
    const FuriHalI2cBusHandle* handle,
    uint8_t i2c_addr,
    uint32_t timeout) {
    UNUSED(handle);
    /* The scanner also probes the odd (read) form of every address and discards it, so
     * answering false here saves 127 pointless bus transactions per scan. */
    if(i2c_addr & 1) return false;
    const uint8_t addr7 = i2c_addr >> 1;
    /* 0x00-0x07 and 0x78-0x7F are reserved by the I2C spec (general call, CBUS, 10-bit, HS). */
    if(addr7 < 0x08 || addr7 > 0x77) return false;

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if(!cmd) return false;
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (uint8_t)((addr7 << 1) | I2C_MASTER_WRITE), true);
    i2c_master_stop(cmd);
    const esp_err_t err = i2c_master_cmd_begin(I2C_BUS_PORT, cmd, i2c_compat_ticks(timeout));
    i2c_cmd_link_delete(cmd);
    return err == ESP_OK;
}

bool furi_hal_i2c_tx(
    const FuriHalI2cBusHandle* handle,
    uint8_t address,
    const uint8_t* data,
    size_t size,
    uint32_t timeout) {
    UNUSED(handle);
    i2c_compat_blocked = false;
    if(!data || size == 0) return false;
    const uint8_t addr7 = address >> 1;
    if(i2c_compat_is_protected(addr7)) {
        i2c_compat_blocked = true;
        return false;
    }
    return i2c_master_write_to_device(I2C_BUS_PORT, addr7, data, size, i2c_compat_ticks(timeout)) ==
           ESP_OK;
}

bool furi_hal_i2c_trx(
    const FuriHalI2cBusHandle* handle,
    uint8_t address,
    const uint8_t* tx_data,
    size_t tx_size,
    uint8_t* rx_data,
    size_t rx_size,
    uint32_t timeout) {
    UNUSED(handle);
    i2c_compat_blocked = false;
    if(!tx_data || tx_size == 0 || !rx_data || rx_size == 0) return false;
    const uint8_t addr7 = address >> 1;
    /* A single byte before the restart is only the register pointer of a read. Anything
     * longer would be a write to the device. */
    if(tx_size > 1 && i2c_compat_is_protected(addr7)) {
        i2c_compat_blocked = true;
        return false;
    }
    return i2c_master_write_read_device(
               I2C_BUS_PORT,
               addr7,
               tx_data,
               tx_size,
               rx_data,
               rx_size,
               i2c_compat_ticks(timeout)) == ESP_OK;
}
