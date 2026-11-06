/**
 * @file furi_hal_i2c.c
 * I2C HAL for apps (FAPs) on the board's EXTERNAL (Qwiic) bus.
 *
 * Stock Flipper API (8-bit addresses: 7-bit address << 1) on top of the ESP-IDF
 * legacy driver. The bus is the one the firmware itself runs on, shared by:
 *
 *   address  device              owner / who may touch it
 *   0x24     PN532 (NFC)         furi_hal_nfc          -> exclusive
 *   0x55     BQ27220 fuel gauge  power service         -> read-only for apps
 *   0x6B     BQ25896 charger     power service         -> read-only for apps
 *   0x42     BW16 I2C bridge     furi_hal_bw16_i2c     -> exclusive (T-Embed profile only)
 *   0x3C     SSD1306 display     display driver        -> exclusive (Thormini only)
 *
 * "Exclusive": the firmware talks to a stateful device on that address (a read can
 * eat a frame the driver is waiting for, a write can break a protocol). "Read-only":
 * a stray write can change charger / gauge state (a BQ25896 write can enter ship
 * mode). The HAL refuses those accesses itself, so an app cannot get them wrong.
 * Address-only probes (a bus scan) are always allowed.
 *
 * The bus pins and port come from the board header, never from the app: one FAP
 * binary then works on every board. The HAL never reconfigures a bus that is already
 * installed (the power service / NFC / display own it).
 */

#include "furi_hal_i2c.h"

#include <furi.h>
#include <driver/i2c.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdlib.h>
#include <string.h>

#include "boards/board.h"
#include "furi_hal_bq25896.h"
#include "furi_hal_bq27220.h"
#include "furi_hal_bw16_i2c.h"

#define TAG "FuriHalI2c"

/* ---- Which bus is "external" on this board ---- */
#if defined(BOARD_PIN_QWIIC_SDA) && defined(BOARD_PIN_QWIIC_SCL)
#define FHI2C_SDA        BOARD_PIN_QWIIC_SDA
#define FHI2C_SCL        BOARD_PIN_QWIIC_SCL
#define FHI2C_PORT       I2C_NUM_0 /* installed by furi_hal_power_init() */
#define FHI2C_BUS_OWNED  1
#elif defined(BOARD_PIN_NFC_SDA) && defined(BOARD_PIN_NFC_SCL) && defined(BOARD_NFC_I2C_PORT) && \
    (BOARD_PIN_NFC_SDA < 64) && (BOARD_PIN_NFC_SCL < 64)
#define FHI2C_SDA        BOARD_PIN_NFC_SDA
#define FHI2C_SCL        BOARD_PIN_NFC_SCL
#define FHI2C_PORT       BOARD_NFC_I2C_PORT
#define FHI2C_BUS_OWNED  0
#endif

#ifdef FHI2C_SDA
#define FHI2C_AVAILABLE 1
#else
#define FHI2C_AVAILABLE 0
#endif

/* ---- Addresses used by firmware drivers on that bus (7-bit) ---- */
#define FHI2C_ADDR_BQ25896 0x6B
#define FHI2C_ADDR_PN532   0x24 /* PN532_I2C_ADDR in furi_hal_nfc.c */
#ifdef BQ27220_ADDR
#define FHI2C_ADDR_BQ27220 BQ27220_ADDR
#else
#define FHI2C_ADDR_BQ27220 0x55
#endif

/* Two firmware drivers must never claim one address. */
_Static_assert(FHI2C_ADDR_BQ25896 != FHI2C_ADDR_BQ27220, "BQ25896 / BQ27220 address clash");
_Static_assert(FHI2C_ADDR_BQ25896 != FHI2C_ADDR_PN532, "BQ25896 / PN532 address clash");
_Static_assert(FHI2C_ADDR_BQ27220 != FHI2C_ADDR_PN532, "BQ27220 / PN532 address clash");
_Static_assert(FURI_HAL_BW16_I2C_ADDRESS != FHI2C_ADDR_PN532, "BW16 / PN532 address clash");
_Static_assert(FURI_HAL_BW16_I2C_ADDRESS != FHI2C_ADDR_BQ25896, "BW16 / BQ25896 address clash");
_Static_assert(FURI_HAL_BW16_I2C_ADDRESS != FHI2C_ADDR_BQ27220, "BW16 / BQ27220 address clash");
#ifdef BOARD_DISPLAY_I2C_ADDR
_Static_assert(BOARD_DISPLAY_I2C_ADDR != FHI2C_ADDR_PN532, "display / PN532 address clash");
_Static_assert(BOARD_DISPLAY_I2C_ADDR != FHI2C_ADDR_BQ25896, "display / BQ25896 address clash");
_Static_assert(BOARD_DISPLAY_I2C_ADDR != FHI2C_ADDR_BQ27220, "display / BQ27220 address clash");
_Static_assert(BOARD_DISPLAY_I2C_ADDR != FURI_HAL_BW16_I2C_ADDRESS, "display / BW16 address clash");
#endif

/* Floor for every bus timeout. The call also waits for the driver's bus lock, which
 * the power service holds while it polls the BQ chips, so a tight value reports present
 * devices as absent. */
#define FHI2C_MIN_TIMEOUT_MS 20
#define FHI2C_MAX_MEM_WRITE  255

struct FuriHalI2cBusHandle {
    uint8_t reserved;
};

const FuriHalI2cBusHandle furi_hal_i2c_handle_external = {0};

/* ---- Bus lock (serializes app-level sequences; the driver serializes single transfers) ---- */
static StaticSemaphore_t fhi2c_mutex_storage;
static SemaphoreHandle_t fhi2c_mutex = NULL;
static portMUX_TYPE fhi2c_init_lock = portMUX_INITIALIZER_UNLOCKED;
static bool fhi2c_bus_checked = false;

static SemaphoreHandle_t fhi2c_get_mutex(void) {
    portENTER_CRITICAL(&fhi2c_init_lock);
    if(!fhi2c_mutex) {
        fhi2c_mutex = xSemaphoreCreateRecursiveMutexStatic(&fhi2c_mutex_storage);
    }
    portEXIT_CRITICAL(&fhi2c_init_lock);
    return fhi2c_mutex;
}

static TickType_t fhi2c_ticks(uint32_t timeout_ms) {
    if(timeout_ms < FHI2C_MIN_TIMEOUT_MS) timeout_ms = FHI2C_MIN_TIMEOUT_MS;
    return pdMS_TO_TICKS(timeout_ms);
}

/* Bring the bus up only if nothing else has, and only on the board's own pins. On
 * Qwiic boards the power service installs it at boot. Called with the bus lock held. */
static void fhi2c_ensure_bus(void) {
#if FHI2C_AVAILABLE && !FHI2C_BUS_OWNED
    /* Under the bus lock (recursive: callers usually hold it already), so two threads cannot
     * both install, and every transfer can call this without having called acquire(). */
    SemaphoreHandle_t mutex = fhi2c_get_mutex();
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    if(!fhi2c_bus_checked) {
        /* A bus that is already installed is reused untouched: reconfiguring a live bus
         * could glitch a transfer in flight. ESP_ERR_INVALID_STATE means "already there". */
        bool done = true;
        if(i2c_driver_install(FHI2C_PORT, I2C_MODE_MASTER, 0, 0, 0) == ESP_OK) {
            const i2c_config_t conf = {
                .mode = I2C_MODE_MASTER,
                .sda_io_num = FHI2C_SDA,
                .scl_io_num = FHI2C_SCL,
                .sda_pullup_en = GPIO_PULLUP_ENABLE,
                .scl_pullup_en = GPIO_PULLUP_ENABLE,
                .master.clk_speed = 100000,
            };
            if(i2c_param_config(FHI2C_PORT, &conf) != ESP_OK) {
                /* Never leave a half-configured bus behind: take it down, try again later. */
                ESP_LOGE(TAG, "I2C pin setup failed; bus left uninstalled");
                i2c_driver_delete(FHI2C_PORT);
                done = false;
            }
        }
        fhi2c_bus_checked = done;
    }
    xSemaphoreGiveRecursive(mutex);
#else
    fhi2c_bus_checked = true;
#endif
}

/* ---- Address policy ---- */

static bool fhi2c_lookup(uint8_t addr7, FuriHalI2cAccess* access, const char** owner) {
    FuriHalI2cAccess a = FuriHalI2cAccessFree;
    const char* o = NULL;

    if(addr7 == FHI2C_ADDR_BQ25896 && furi_hal_bq25896_is_present()) {
        a = FuriHalI2cAccessReadOnly;
        o = "BQ25896 charger";
    } else if(addr7 == FHI2C_ADDR_BQ27220 && furi_hal_bq27220_is_present()) {
        a = FuriHalI2cAccessReadOnly;
        o = "BQ27220 gauge";
    }
#if defined(BOARD_HAS_NFC) && BOARD_HAS_NFC && defined(BOARD_PIN_NFC_SDA) && FHI2C_AVAILABLE && \
    (BOARD_PIN_NFC_SDA == FHI2C_SDA) && (BOARD_PIN_NFC_SCL == FHI2C_SCL)
    else if(addr7 == FHI2C_ADDR_PN532) {
        a = FuriHalI2cAccessExclusive;
        o = "PN532 NFC";
    }
#endif
    else if(addr7 == FURI_HAL_BW16_I2C_ADDRESS && furi_hal_bw16_i2c_available()) {
        a = FuriHalI2cAccessExclusive;
        o = "BW16 bridge";
    }
#if defined(BOARD_DISPLAY_I2C_ADDR) && FHI2C_AVAILABLE && defined(BOARD_PIN_DISPLAY_SDA) && \
    (BOARD_PIN_DISPLAY_SDA == FHI2C_SDA) && (BOARD_PIN_DISPLAY_SCL == FHI2C_SCL)
    else if(addr7 == BOARD_DISPLAY_I2C_ADDR) {
        a = FuriHalI2cAccessExclusive;
        o = "OLED display";
    }
#endif

    if(access) *access = a;
    if(owner) *owner = o;
    return o != NULL;
}

FuriHalI2cAccess furi_hal_i2c_get_access(uint8_t addr7) {
    FuriHalI2cAccess access = FuriHalI2cAccessFree;
    fhi2c_lookup(addr7, &access, NULL);
    return access;
}

const char* furi_hal_i2c_get_owner(uint8_t addr7) {
    const char* owner = NULL;
    fhi2c_lookup(addr7, NULL, &owner);
    return owner;
}

bool furi_hal_i2c_external_available(void) {
    return FHI2C_AVAILABLE;
}

bool furi_hal_i2c_external_get_pins(uint8_t* sda, uint8_t* scl) {
#if FHI2C_AVAILABLE
    if(sda) *sda = (uint8_t)FHI2C_SDA;
    if(scl) *scl = (uint8_t)FHI2C_SCL;
    return true;
#else
    if(sda) *sda = UINT8_MAX;
    if(scl) *scl = UINT8_MAX;
    return false;
#endif
}

bool furi_hal_i2c_get_sniffer_pins(uint8_t* sda, uint8_t* scl) {
#if defined(BOARD_PIN_I2C_SNIFF_SDA) && defined(BOARD_PIN_I2C_SNIFF_SCL)
    if(sda) *sda = (uint8_t)BOARD_PIN_I2C_SNIFF_SDA;
    if(scl) *scl = (uint8_t)BOARD_PIN_I2C_SNIFF_SCL;
    return true;
#else
    if(sda) *sda = UINT8_MAX;
    if(scl) *scl = UINT8_MAX;
    return false;
#endif
}

/* ---- Bus access ---- */

void furi_hal_i2c_acquire(const FuriHalI2cBusHandle* handle) {
    UNUSED(handle);
    SemaphoreHandle_t mutex = fhi2c_get_mutex();
    furi_check(xSemaphoreTakeRecursive(mutex, portMAX_DELAY) == pdTRUE);
    fhi2c_ensure_bus();
}

void furi_hal_i2c_release(const FuriHalI2cBusHandle* handle) {
    UNUSED(handle);
    /* The bus is shared with the power ICs and NFC: it is never uninstalled. */
    furi_check(xSemaphoreGiveRecursive(fhi2c_get_mutex()) == pdTRUE);
}

#if FHI2C_AVAILABLE

/* Upstream Flipper addresses are 8-bit (7-bit << 1). A read/write to a protected
 * address is refused; `write` says which kind of access this is. */
static bool fhi2c_access_ok(uint8_t addr7, bool write) {
    const FuriHalI2cAccess access = furi_hal_i2c_get_access(addr7);
    if(access == FuriHalI2cAccessExclusive) return false;
    if(access == FuriHalI2cAccessReadOnly && write) return false;
    return true;
}

/* Reserved by the I2C spec (general call, CBUS, 10-bit, HS mode) or out of range. */
static bool fhi2c_addr7_valid(uint8_t addr7) {
    return addr7 >= 0x08 && addr7 <= 0x77;
}

bool furi_hal_i2c_is_device_ready(
    const FuriHalI2cBusHandle* handle,
    uint8_t i2c_addr,
    uint32_t timeout) {
    UNUSED(handle);
    /* The scanner also probes the odd (read) form of every address; answering false
     * here saves 127 pointless bus transactions per scan. */
    if(i2c_addr & 1) return false;
    const uint8_t addr7 = i2c_addr >> 1;
    if(!fhi2c_addr7_valid(addr7)) return false;
    fhi2c_ensure_bus();

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if(!cmd) return false;
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (uint8_t)((addr7 << 1) | I2C_MASTER_WRITE), true);
    i2c_master_stop(cmd);
    const esp_err_t err = i2c_master_cmd_begin(FHI2C_PORT, cmd, fhi2c_ticks(timeout));
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
    if(!data || size == 0) return false;
    const uint8_t addr7 = address >> 1;
    if(!fhi2c_addr7_valid(addr7) || !fhi2c_access_ok(addr7, true)) return false;
    fhi2c_ensure_bus();
    return i2c_master_write_to_device(FHI2C_PORT, addr7, data, size, fhi2c_ticks(timeout)) ==
           ESP_OK;
}

bool furi_hal_i2c_rx(
    const FuriHalI2cBusHandle* handle,
    uint8_t address,
    uint8_t* data,
    size_t size,
    uint32_t timeout) {
    UNUSED(handle);
    if(!data || size == 0) return false;
    const uint8_t addr7 = address >> 1;
    if(!fhi2c_addr7_valid(addr7) || !fhi2c_access_ok(addr7, false)) return false;
    fhi2c_ensure_bus();
    return i2c_master_read_from_device(FHI2C_PORT, addr7, data, size, fhi2c_ticks(timeout)) ==
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
    if(!tx_data || tx_size == 0 || !rx_data || rx_size == 0) return false;
    const uint8_t addr7 = address >> 1;
    if(!fhi2c_addr7_valid(addr7)) return false;
    /* A single byte before the restart is only the register pointer of a read. Anything
     * longer would be a write to the device. */
    if(!fhi2c_access_ok(addr7, tx_size > 1)) return false;
    fhi2c_ensure_bus();
    return i2c_master_write_read_device(
               FHI2C_PORT, addr7, tx_data, tx_size, rx_data, rx_size, fhi2c_ticks(timeout)) ==
           ESP_OK;
}

#else /* no external bus on this board */

bool furi_hal_i2c_is_device_ready(const FuriHalI2cBusHandle* h, uint8_t a, uint32_t t) {
    UNUSED(h);
    UNUSED(a);
    UNUSED(t);
    return false;
}

bool furi_hal_i2c_tx(
    const FuriHalI2cBusHandle* h,
    uint8_t a,
    const uint8_t* d,
    size_t n,
    uint32_t t) {
    UNUSED(h);
    UNUSED(a);
    UNUSED(d);
    UNUSED(n);
    UNUSED(t);
    return false;
}

bool furi_hal_i2c_rx(
    const FuriHalI2cBusHandle* h,
    uint8_t a,
    uint8_t* d,
    size_t n,
    uint32_t t) {
    UNUSED(h);
    UNUSED(a);
    UNUSED(d);
    UNUSED(n);
    UNUSED(t);
    return false;
}

bool furi_hal_i2c_trx(
    const FuriHalI2cBusHandle* h,
    uint8_t a,
    const uint8_t* td,
    size_t tn,
    uint8_t* rd,
    size_t rn,
    uint32_t t) {
    UNUSED(h);
    UNUSED(a);
    UNUSED(td);
    UNUSED(tn);
    UNUSED(rd);
    UNUSED(rn);
    UNUSED(t);
    return false;
}

#endif

/* ---- Register helpers (stock Flipper API, big-endian 16-bit like upstream) ---- */

bool furi_hal_i2c_read_reg_8(
    const FuriHalI2cBusHandle* handle,
    uint8_t i2c_addr,
    uint8_t reg_addr,
    uint8_t* data,
    uint32_t timeout) {
    return furi_hal_i2c_trx(handle, i2c_addr, &reg_addr, 1, data, 1, timeout);
}

bool furi_hal_i2c_read_reg_16(
    const FuriHalI2cBusHandle* handle,
    uint8_t i2c_addr,
    uint8_t reg_addr,
    uint16_t* data,
    uint32_t timeout) {
    uint8_t buffer[2] = {0};
    if(!furi_hal_i2c_trx(handle, i2c_addr, &reg_addr, 1, buffer, 2, timeout)) return false;
    *data = (uint16_t)((buffer[0] << 8) | buffer[1]);
    return true;
}

bool furi_hal_i2c_read_mem(
    const FuriHalI2cBusHandle* handle,
    uint8_t i2c_addr,
    uint8_t mem_addr,
    uint8_t* data,
    size_t len,
    uint32_t timeout) {
    return furi_hal_i2c_trx(handle, i2c_addr, &mem_addr, 1, data, len, timeout);
}

bool furi_hal_i2c_write_reg_8(
    const FuriHalI2cBusHandle* handle,
    uint8_t i2c_addr,
    uint8_t reg_addr,
    uint8_t data,
    uint32_t timeout) {
    const uint8_t buffer[2] = {reg_addr, data};
    return furi_hal_i2c_tx(handle, i2c_addr, buffer, sizeof(buffer), timeout);
}

bool furi_hal_i2c_write_reg_16(
    const FuriHalI2cBusHandle* handle,
    uint8_t i2c_addr,
    uint8_t reg_addr,
    uint16_t data,
    uint32_t timeout) {
    const uint8_t buffer[3] = {reg_addr, (uint8_t)(data >> 8), (uint8_t)(data & 0xFF)};
    return furi_hal_i2c_tx(handle, i2c_addr, buffer, sizeof(buffer), timeout);
}

bool furi_hal_i2c_write_mem(
    const FuriHalI2cBusHandle* handle,
    uint8_t i2c_addr,
    uint8_t mem_addr,
    const uint8_t* data,
    size_t len,
    uint32_t timeout) {
    if(!data || len == 0 || len > FHI2C_MAX_MEM_WRITE) return false;
    uint8_t* buffer = malloc(len + 1);
    if(!buffer) return false;
    buffer[0] = mem_addr;
    memcpy(&buffer[1], data, len);
    const bool ok = furi_hal_i2c_tx(handle, i2c_addr, buffer, len + 1, timeout);
    free(buffer);
    return ok;
}
