#include "furi_hal_spi.h"
#include "furi_hal_spi_bus.h"

#include <string.h>

#include <driver/spi_master.h>
#include <esp_log.h>
#include <esp_rom_sys.h>

#include <furi.h>
#include <furi_hal_gpio.h>
#include <furi_hal_power.h>
#include <furi_hal_resources.h>
#include "boards/board.h"

#ifndef BOARD_CC1101_SPI_SHARED
#define BOARD_CC1101_SPI_SHARED 0
#endif

static const char* TAG = "FuriHalSpi";
static const uint16_t FURI_HAL_SPI_PIN_UNMAPPED = UINT16_MAX;
static const uint32_t FURI_HAL_SPI_BITBANG_DELAY_US = 1;

FuriHalSpiBus furi_hal_spi_bus_external = {
    .host_id = -1,
    .bitbang = true,
    .initialized = false,
    .mutex = NULL,
    .current_handle = NULL,
    .mosi_pin = FURI_HAL_SPI_PIN_UNMAPPED,
    .miso_pin = FURI_HAL_SPI_PIN_UNMAPPED,
    .sck_pin = FURI_HAL_SPI_PIN_UNMAPPED,
};

FuriHalSpiBus furi_hal_spi_bus_subghz = {
#if BOARD_CC1101_SPI_SHARED
    /* T-Embed: CC1101 shares SPI2_HOST with LCD+SD, CS-muxed */
    .host_id = SPI2_HOST,
    .bitbang = false,
#else
    /* Waveshare: CC1101 on separate bitbang SPI */
    .host_id = -1,
    .bitbang = true,
#endif
    .initialized = false,
    .mutex = NULL,
    .current_handle = NULL,
    .mosi_pin = FURI_HAL_SPI_PIN_UNMAPPED,
    .miso_pin = FURI_HAL_SPI_PIN_UNMAPPED,
    .sck_pin = FURI_HAL_SPI_PIN_UNMAPPED,
};

FuriHalSpiBusHandle furi_hal_spi_bus_handle_external = {
    .bus = &furi_hal_spi_bus_external,
    .miso = &gpio_ext_pa6,
    .mosi = &gpio_ext_pa7,
    .sck = &gpio_ext_pb3,
    .cs = &gpio_ext_pa4,
    .device = NULL,
    .initialized = false,
    .frequency_hz = 2 * 1000 * 1000,
    .mode = 0,
};

FuriHalSpiBusHandle furi_hal_spi_bus_handle_subghz = {
    .bus = &furi_hal_spi_bus_subghz,
    .miso = &gpio_ext_pa6,
    .mosi = &gpio_ext_pa7,
    .sck = &gpio_ext_pb3,
    .cs = &gpio_ext_pa4,
    .device = NULL,
    .initialized = false,
    .frequency_hz = 100 * 1000,
    .mode = 0,
};

static bool furi_hal_spi_pin_valid(const GpioPin* pin) {
    return pin && pin->pin != FURI_HAL_SPI_PIN_UNMAPPED;
}

static void furi_hal_spi_bitbang_delay(void) {
    esp_rom_delay_us(FURI_HAL_SPI_BITBANG_DELAY_US);
}

static void furi_hal_spi_fill_stub_rx(uint8_t* rx_buffer, size_t size) {
    if(rx_buffer) memset(rx_buffer, 0, size);
}

static FuriMutex* furi_hal_spi_get_mutex(FuriHalSpiBus* bus) {
    if(!bus->mutex) {
        bus->mutex = furi_mutex_alloc(FuriMutexTypeRecursive);
        furi_check(bus->mutex);
    }
    return bus->mutex;
}

static bool furi_hal_spi_bus_init_if_needed(FuriHalSpiBus* bus, const FuriHalSpiBusHandle* handle) {
    if(bus->initialized) return true;

    if(!furi_hal_spi_pin_valid(handle->mosi) || !furi_hal_spi_pin_valid(handle->sck)) {
        ESP_LOGW(TAG, "SPI pins are not mapped yet; using stub responses");
        return false;
    }

    if(bus->bitbang) {
        furi_hal_gpio_init_simple(handle->mosi, GpioModeOutputPushPull);
        furi_hal_gpio_write(handle->mosi, false);
        if(furi_hal_spi_pin_valid(handle->miso)) {
            furi_hal_gpio_init_simple(handle->miso, GpioModeInput);
        }
        if(furi_hal_spi_pin_valid(handle->sck)) {
            furi_hal_gpio_init_simple(handle->sck, GpioModeOutputPushPull);
            furi_hal_gpio_write(handle->sck, false);
        }
        if(furi_hal_spi_pin_valid(handle->cs)) {
            furi_hal_gpio_init_simple(handle->cs, GpioModeOutputPushPull);
            furi_hal_gpio_write(handle->cs, true);
        }

        bus->initialized = true;
        bus->mosi_pin = handle->mosi->pin;
        bus->miso_pin = furi_hal_spi_pin_valid(handle->miso) ? handle->miso->pin : FURI_HAL_SPI_PIN_UNMAPPED;
        bus->sck_pin = handle->sck->pin;
        ESP_LOGI(
            TAG,
            "Initialized bitbang SPI on MOSI=%u MISO=%u SCK=%u CS=%u",
            bus->mosi_pin,
            bus->miso_pin,
            bus->sck_pin,
            furi_hal_spi_pin_valid(handle->cs) ? handle->cs->pin : FURI_HAL_SPI_PIN_UNMAPPED);
        return true;
    }

    spi_bus_config_t bus_config = {
        .mosi_io_num = handle->mosi->pin,
        .miso_io_num = furi_hal_spi_pin_valid(handle->miso) ? handle->miso->pin : -1,
        .sclk_io_num = handle->sck->pin,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };

    esp_err_t err = spi_bus_initialize(bus->host_id, &bus_config, SPI_DMA_CH_AUTO);
    if(err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        bus->initialized = true;
        bus->mosi_pin = handle->mosi->pin;
        bus->miso_pin = furi_hal_spi_pin_valid(handle->miso) ? handle->miso->pin : FURI_HAL_SPI_PIN_UNMAPPED;
        bus->sck_pin = handle->sck->pin;
        ESP_LOGI(
            TAG,
            "Initialized HW SPI host=%d on MOSI=%u MISO=%u SCK=%u",
            bus->host_id,
            bus->mosi_pin,
            bus->miso_pin,
            bus->sck_pin);
        return true;
    }

    ESP_LOGW(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
    return false;
}

void furi_hal_spi_bus_handle_init(const FuriHalSpiBusHandle* handle) {
    furi_check(handle);

    FuriHalSpiBusHandle* mutable_handle = (FuriHalSpiBusHandle*)handle;
    FuriHalSpiBus* bus = mutable_handle->bus;

    furi_hal_spi_get_mutex(bus);
    if(mutable_handle->initialized) return;

    if(!furi_hal_spi_bus_init_if_needed(bus, handle) || !furi_hal_spi_pin_valid(handle->cs)) {
        mutable_handle->initialized = false;
        mutable_handle->device = NULL;
        return;
    }

    if(bus->bitbang) {
        mutable_handle->device = NULL;
        mutable_handle->initialized = true;
        return;
    }

    spi_device_interface_config_t dev_config = {
        .clock_speed_hz = mutable_handle->frequency_hz ? (int)mutable_handle->frequency_hz : 2 * 1000 * 1000,
        .mode = mutable_handle->mode,
        .spics_io_num = handle->cs->pin,
        .queue_size = 1,
        .flags = 0,
    };

    spi_device_handle_t device = NULL;
    esp_err_t err = spi_bus_add_device(bus->host_id, &dev_config, &device);
    if(err != ESP_OK) {
        ESP_LOGW(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        mutable_handle->initialized = false;
        mutable_handle->device = NULL;
        return;
    }

    mutable_handle->device = device;
    mutable_handle->initialized = true;
}

void furi_hal_spi_bus_handle_deinit(const FuriHalSpiBusHandle* handle) {
    if(!handle) return;

    FuriHalSpiBusHandle* mutable_handle = (FuriHalSpiBusHandle*)handle;
    if(mutable_handle->device) {
        spi_bus_remove_device((spi_device_handle_t)mutable_handle->device);
        mutable_handle->device = NULL;
    }
    mutable_handle->initialized = false;
}

void furi_hal_spi_acquire(const FuriHalSpiBusHandle* handle) {
    furi_check(handle);

    FuriHalSpiBusHandle* mutable_handle = (FuriHalSpiBusHandle*)handle;
    FuriHalSpiBus* bus = mutable_handle->bus;
    furi_check(bus);

    furi_hal_spi_bus_handle_init(handle);
#if BOARD_CC1101_SPI_SHARED
    /* On shared-bus boards, also lock the global SPI bus mutex
     * to serialize with display DMA and SD card transactions */
    if(!bus->bitbang) {
        furi_hal_spi_bus_lock();
    }
#endif
    furi_check(furi_mutex_acquire(furi_hal_spi_get_mutex(bus), FuriWaitForever) == FuriStatusOk);
    furi_hal_power_insomnia_enter();
    bus->current_handle = handle;
}

void furi_hal_spi_release(const FuriHalSpiBusHandle* handle) {
    furi_check(handle);
    FuriHalSpiBus* bus = ((FuriHalSpiBusHandle*)handle)->bus;
    furi_check(bus);

    bus->current_handle = NULL;
    furi_hal_power_insomnia_exit();
    furi_check(furi_mutex_release(furi_hal_spi_get_mutex(bus)) == FuriStatusOk);
#if BOARD_CC1101_SPI_SHARED
    if(!bus->bitbang) {
        furi_hal_spi_bus_unlock();
    }
#endif
}

bool furi_hal_spi_bus_trx(
    const FuriHalSpiBusHandle* handle,
    const uint8_t* tx_buffer,
    uint8_t* rx_buffer,
    size_t size,
    uint32_t timeout) {
    UNUSED(timeout);
    furi_check(handle);
    furi_check(handle->bus);
    furi_check(handle->bus->current_handle == handle);

    if(size == 0) return true;

    if(handle->bus->bitbang) {
        if(!furi_hal_spi_pin_valid(handle->cs) || !furi_hal_spi_pin_valid(handle->sck) ||
           !furi_hal_spi_pin_valid(handle->mosi)) {
            furi_hal_spi_fill_stub_rx(rx_buffer, size);
            return false;
        }

        furi_hal_gpio_write(handle->cs, false);
        furi_hal_spi_bitbang_delay();

        for(size_t byte_index = 0; byte_index < size; byte_index++) {
            uint8_t tx = tx_buffer ? tx_buffer[byte_index] : 0xFF;
            uint8_t rx = 0;

            for(uint8_t bit = 0; bit < 8; bit++) {
                const bool mosi_state = (tx & 0x80U) != 0;
                furi_hal_gpio_write(handle->mosi, mosi_state);
                furi_hal_spi_bitbang_delay();

                furi_hal_gpio_write(handle->sck, true);
                furi_hal_spi_bitbang_delay();

                rx <<= 1;
                if(furi_hal_spi_pin_valid(handle->miso) && furi_hal_gpio_read(handle->miso)) {
                    rx |= 1U;
                }

                furi_hal_gpio_write(handle->sck, false);
                furi_hal_spi_bitbang_delay();
                tx <<= 1;
            }

            if(rx_buffer) rx_buffer[byte_index] = rx;
        }

        furi_hal_gpio_write(handle->cs, true);
        furi_hal_spi_bitbang_delay();
        return true;
    }

    if(!handle->initialized || !handle->device) {
        furi_hal_spi_fill_stub_rx(rx_buffer, size);
        return true;
    }

    spi_transaction_t transaction = {
        .length = size * 8,
        .tx_buffer = tx_buffer,
        .rx_buffer = rx_buffer,
    };

    esp_err_t err =
        spi_device_polling_transmit((spi_device_handle_t)handle->device, &transaction);
    if(err != ESP_OK) {
        ESP_LOGW(TAG, "spi_device_polling_transmit failed: %s", esp_err_to_name(err));
        furi_hal_spi_fill_stub_rx(rx_buffer, size);
        return false;
    }

    return true;
}

bool furi_hal_spi_bus_tx(
    const FuriHalSpiBusHandle* handle,
    const uint8_t* buffer,
    size_t size,
    uint32_t timeout) {
    return furi_hal_spi_bus_trx(handle, buffer, NULL, size, timeout);
}

bool furi_hal_spi_bus_rx(
    const FuriHalSpiBusHandle* handle,
    uint8_t* buffer,
    size_t size,
    uint32_t timeout) {
    if(size == 0) return true;

    uint8_t* dummy = malloc(size);
    if(!dummy) {
        furi_hal_spi_fill_stub_rx(buffer, size);
        return false;
    }

    memset(dummy, 0xFF, size);
    bool result = furi_hal_spi_bus_trx(handle, dummy, buffer, size, timeout);
    free(dummy);
    return result;
}

bool furi_hal_spi_bus_trx_dma(
    const FuriHalSpiBusHandle* handle,
    uint8_t* tx_buffer,
    uint8_t* rx_buffer,
    size_t size,
    uint32_t timeout_ms) {
    return furi_hal_spi_bus_trx(handle, tx_buffer, rx_buffer, size, timeout_ms);
}
