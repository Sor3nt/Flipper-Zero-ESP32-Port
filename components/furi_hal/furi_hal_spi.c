#include "furi_hal_spi.h"
#include "furi_hal_spi_bus.h"

#include <stdlib.h>
#include <string.h>

#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>

#include <furi.h>
#include <furi_hal_gpio.h>
#include <furi_hal_power.h>
#include <furi_hal_resources.h>
#include "boards/board.h"

#ifndef BOARD_CC1101_SPI_SHARED
#define BOARD_CC1101_SPI_SHARED 0
#endif

/* On shared-bus boards (LCD + SD + CC1101 + NRF24 on one SPI host), the radio
 * used to also take the software-level global SPI lock (furi_hal_spi_bus_lock)
 * on every acquire/release. That serialized every single register transaction
 * against display/SD, and since the display held that same lock for an entire
 * frame (pre-stripe-lock fix) or SD held it for a whole sector op, the radio
 * could stall for tens of milliseconds per transaction -- observed as delay on
 * NRF24 spectrum/jammer and Sub-GHz.
 *
 * The underlying ESP-IDF SPI driver already serializes individual
 * spi_device_polling_transmit() calls against OTHER devices on the same host
 * (LCD, SD) at the hardware/driver level -- each call is a complete, atomic,
 * short transaction. The global software lock was therefore redundant for the
 * radio's per-register-write pattern, and only added artificial, long-held
 * contention. Once the display's own lock-hold time was fixed (stripe-level
 * locking instead of per-frame), the radio was switched to taking only its own
 * bus-local mutex (shared between CC1101 and NRF24), relying on the driver.
 *
 * That turned out to be wrong in practice: esp_lcd's tx_color() queues the
 * colour DMA and releases its IDF bus lock while the transfer is still running
 * from the SPI ISR, and a radio spi_device_polling_transmit() started from the
 * other core during that window hits
 *   assert failed: spi_hal_setup_trans ... (spi_ll_get_running_cmd(hw) == 0)
 * i.e. the hardware is still busy with the LCD transfer. It reproduced on
 * every Sub-GHz scene (Read, Read RAW TX, Frequency Analyzer, Spectrum) and
 * on NRF24 (spectrum, jammer, mousejacker) as soon as the UI redrew while the
 * radio was active, and got far more frequent once the CC1101 SPI clock went
 * from 200 kHz to 2 MHz (back-to-back register transactions).
 *
 * So radios take the global lock again (1). The display holds that lock per
 * stripe until the DMA completion callback fired (display_flush_stripe /
 * furi_hal_display_blit), so a radio transaction can no longer overlap an LCD
 * transfer; the cost is waiting at most one stripe (~1 ms). Override with 0
 * only on a board where the radio has its own SPI host. */
#ifndef FURI_HAL_SPI_RADIO_TAKES_GLOBAL_LOCK
#define FURI_HAL_SPI_RADIO_TAKES_GLOBAL_LOCK 1
#endif

static const char* TAG = "FuriHalSpi";
static const uint16_t FURI_HAL_SPI_PIN_UNMAPPED = UINT16_MAX;
static const uint32_t FURI_HAL_SPI_BITBANG_DELAY_US = 1;
#define FURI_HAL_SPI_MAX_TRANSFER_SZ 16384
#define FURI_HAL_SPI_TRX_STACK_BUF_SIZE 64

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
    /* Shared-bus boards keep the radio on SPI2 with LCD/SD. */
    .host_id = SPI2_HOST,
    .bitbang = false,
#elif defined(BOARD_CC1101_SPI_HOST)
    /* Boards with a dedicated radio bus use the hardware SPI host selected
     * by the board schematic, normally SPI3_HOST on Smoochie. */
    .host_id = BOARD_CC1101_SPI_HOST,
    .bitbang = false,
#else
    /* Legacy boards without a hardware radio host use bitbang SPI. */
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
    .frequency_hz = 2 * 1000 * 1000,
    .mode = 0,
};

/* NRF24 sits on the same SPI bus as the SubGhz / CC1101 on shared-SPI boards but uses its
 * own CS pin -- both devices can be parallel-attached and CS-muxed on the bus. */
FuriHalSpiBusHandle furi_hal_spi_bus_handle_nrf24 = {
    .bus = &furi_hal_spi_bus_subghz,
    .miso = &gpio_ext_pa6,
    .mosi = &gpio_ext_pa7,
    .sck = &gpio_ext_pb3,
    .cs = &gpio_nrf24_cs,
    .device = NULL,
    .initialized = false,
    .frequency_hz = 4 * 1000 * 1000,
    .mode = 0,
};

static bool furi_hal_spi_pin_valid(const GpioPin* pin) {
    return pin && pin->pin != FURI_HAL_SPI_PIN_UNMAPPED;
}

static bool furi_hal_spi_handles_share_signals(
    const FuriHalSpiBusHandle* first,
    const FuriHalSpiBusHandle* second) {
    const uint16_t first_miso =
        furi_hal_spi_pin_valid(first->miso) ? first->miso->pin : FURI_HAL_SPI_PIN_UNMAPPED;
    const uint16_t second_miso =
        furi_hal_spi_pin_valid(second->miso) ? second->miso->pin : FURI_HAL_SPI_PIN_UNMAPPED;
    return first->mosi->pin == second->mosi->pin && first_miso == second_miso &&
           first->sck->pin == second->sck->pin;
}

static bool furi_hal_spi_handle_conflicts_with_radio_bus(const FuriHalSpiBusHandle* handle) {
    const FuriHalSpiBusHandle* radio_handles[] = {
        &furi_hal_spi_bus_handle_subghz,
        &furi_hal_spi_bus_handle_nrf24,
    };
    const GpioPin* handle_pins[] = {handle->mosi, handle->miso, handle->sck, handle->cs};

    if(!handle->bus || !handle->bus->bitbang) return false;

    for(size_t i = 0; i < sizeof(radio_handles) / sizeof(radio_handles[0]); i++) {
        const FuriHalSpiBusHandle* radio = radio_handles[i];
        if(!radio->bus || radio->bus == handle->bus || radio->bus->bitbang) continue;

        const GpioPin* radio_pins[] = {radio->mosi, radio->miso, radio->sck, radio->cs};
        for(size_t handle_pin = 0;
            handle_pin < sizeof(handle_pins) / sizeof(handle_pins[0]);
            handle_pin++) {
            if(!furi_hal_spi_pin_valid(handle_pins[handle_pin])) continue;
            for(size_t radio_pin = 0;
                radio_pin < sizeof(radio_pins) / sizeof(radio_pins[0]);
                radio_pin++) {
                if(furi_hal_spi_pin_valid(radio_pins[radio_pin]) &&
                   handle_pins[handle_pin]->pin == radio_pins[radio_pin]->pin) {
                    return true;
                }
            }
        }
    }

    return false;
}

static void furi_hal_spi_bitbang_delay(void) {
    esp_rom_delay_us(FURI_HAL_SPI_BITBANG_DELAY_US);
}

static void furi_hal_spi_fill_stub_rx(uint8_t* rx_buffer, size_t size) {
    if(rx_buffer) memset(rx_buffer, 0, size);
}

static FuriMutex* furi_hal_spi_get_mutex(FuriHalSpiBus* bus) {
    static portMUX_TYPE mutex_lock = portMUX_INITIALIZER_UNLOCKED;
    FuriMutex* mutex = bus->mutex;
    if(mutex) return mutex;

    FuriMutex* candidate = furi_mutex_alloc(FuriMutexTypeRecursive);
    furi_check(candidate);
    portENTER_CRITICAL(&mutex_lock);
    if(!bus->mutex) bus->mutex = candidate;
    mutex = bus->mutex;
    portEXIT_CRITICAL(&mutex_lock);
    if(mutex != candidate) furi_mutex_free(candidate);
    return mutex;
}

static void furi_hal_spi_shared_bus_lock_if_needed(const FuriHalSpiBus* bus) {
#if BOARD_CC1101_SPI_SHARED && FURI_HAL_SPI_RADIO_TAKES_GLOBAL_LOCK
    /* Match transfer lock order: shared bus first, then the bus-local mutex. */
    if(!bus->bitbang) furi_hal_spi_bus_lock();
#else
    UNUSED(bus);
#endif
}

static void furi_hal_spi_shared_bus_unlock_if_needed(const FuriHalSpiBus* bus) {
#if BOARD_CC1101_SPI_SHARED && FURI_HAL_SPI_RADIO_TAKES_GLOBAL_LOCK
    if(!bus->bitbang) furi_hal_spi_bus_unlock();
#else
    UNUSED(bus);
#endif
}

void furi_hal_spi_init(void) {
    /* Create the global bus lock now, while boot is still single threaded. */
    furi_hal_spi_bus_init();
    FuriMutex* subghz_mutex = furi_hal_spi_get_mutex(&furi_hal_spi_bus_subghz);
    if(furi_hal_spi_bus_external.bitbang && furi_hal_spi_bus_subghz.bitbang &&
       furi_hal_spi_handles_share_signals(
           &furi_hal_spi_bus_handle_external, &furi_hal_spi_bus_handle_subghz)) {
        furi_hal_spi_bus_external.mutex = subghz_mutex;
    } else {
        (void)furi_hal_spi_get_mutex(&furi_hal_spi_bus_external);
    }
    ESP_LOGI(TAG, "SPI bus mutexes initialized");
}

static bool furi_hal_spi_ptr_dma_safe(const void* ptr, size_t size) {
    if(!ptr || size == 0) return true;
    return esp_ptr_word_aligned(ptr) && esp_ptr_dma_capable(ptr);
}

static bool furi_hal_spi_bus_init_if_needed(FuriHalSpiBus* bus, const FuriHalSpiBusHandle* handle) {
    if(bus->initialized) {
        const uint16_t miso_pin =
            furi_hal_spi_pin_valid(handle->miso) ? handle->miso->pin : FURI_HAL_SPI_PIN_UNMAPPED;
        const bool same_pins = bus->mosi_pin == handle->mosi->pin &&
                               bus->miso_pin == miso_pin &&
                               bus->sck_pin == handle->sck->pin;
        if(!same_pins) {
            ESP_LOGE(
                TAG,
                "SPI host=%d already initialized on MOSI=%u MISO=%u SCK=%u; requested %u/%u/%u",
                bus->host_id,
                bus->mosi_pin,
                bus->miso_pin,
                bus->sck_pin,
                handle->mosi->pin,
                miso_pin,
                handle->sck->pin);
        }
        return same_pins;
    }

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
        .max_transfer_sz = FURI_HAL_SPI_MAX_TRANSFER_SZ,
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
    furi_check(bus);
    if(!handle->mosi || !handle->sck || !handle->cs) {
        ESP_LOGE(TAG, "SPI handle has a NULL pin descriptor");
        return;
    }
    if(furi_hal_spi_handle_conflicts_with_radio_bus(handle)) {
        ESP_LOGE(TAG, "Refusing bitbang SPI handle that overlaps hardware radio SPI pins");
        mutable_handle->initialized = false;
        mutable_handle->device = NULL;
        return;
    }

    furi_hal_spi_shared_bus_lock_if_needed(bus);
    FuriMutex* mutex = furi_hal_spi_get_mutex(bus);
    furi_check(furi_mutex_acquire(mutex, FuriWaitForever) == FuriStatusOk);
    if(mutable_handle->initialized) {
        furi_check(furi_mutex_release(mutex) == FuriStatusOk);
        furi_hal_spi_shared_bus_unlock_if_needed(bus);
        return;
    }

    if(!furi_hal_spi_bus_init_if_needed(bus, handle) || !furi_hal_spi_pin_valid(handle->cs)) {
        mutable_handle->initialized = false;
        mutable_handle->device = NULL;
        furi_check(furi_mutex_release(mutex) == FuriStatusOk);
        furi_hal_spi_shared_bus_unlock_if_needed(bus);
        return;
    }

    if(bus->bitbang) {
        furi_hal_gpio_init_simple(handle->cs, GpioModeOutputPushPull);
        furi_hal_gpio_write(handle->cs, true);
        mutable_handle->device = NULL;
        mutable_handle->initialized = true;
        furi_check(furi_mutex_release(mutex) == FuriStatusOk);
        furi_hal_spi_shared_bus_unlock_if_needed(bus);
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
        furi_check(furi_mutex_release(mutex) == FuriStatusOk);
        furi_hal_spi_shared_bus_unlock_if_needed(bus);
        return;
    }

    mutable_handle->device = device;
    mutable_handle->initialized = true;
    ESP_LOGI(
        TAG,
        "SPI device ready: host=%d CS=%u mode=%u clock=%luHz",
        bus->host_id,
        (unsigned)handle->cs->pin,
        (unsigned)mutable_handle->mode,
        (unsigned long)mutable_handle->frequency_hz);
    furi_check(furi_mutex_release(mutex) == FuriStatusOk);
    furi_hal_spi_shared_bus_unlock_if_needed(bus);
}

void furi_hal_spi_bus_handle_deinit(const FuriHalSpiBusHandle* handle) {
    if(!handle) return;

    FuriHalSpiBusHandle* mutable_handle = (FuriHalSpiBusHandle*)handle;
    FuriHalSpiBus* bus = mutable_handle->bus;
    furi_check(bus);
    furi_hal_spi_shared_bus_lock_if_needed(bus);
    FuriMutex* mutex = furi_hal_spi_get_mutex(bus);
    furi_check(furi_mutex_acquire(mutex, FuriWaitForever) == FuriStatusOk);
    if(bus->current_handle == handle) {
        ESP_LOGW(TAG, "Refusing to deinit SPI device while it is acquired");
        furi_check(furi_mutex_release(mutex) == FuriStatusOk);
        furi_hal_spi_shared_bus_unlock_if_needed(bus);
        return;
    }
    if(mutable_handle->device) {
        spi_bus_remove_device((spi_device_handle_t)mutable_handle->device);
        mutable_handle->device = NULL;
    }
    mutable_handle->initialized = false;
    furi_check(furi_mutex_release(mutex) == FuriStatusOk);
    furi_hal_spi_shared_bus_unlock_if_needed(bus);
}

bool furi_hal_spi_bus_handle_set_cs(const FuriHalSpiBusHandle* handle, const GpioPin* cs) {
    furi_check(handle);
    furi_check(cs);

    FuriHalSpiBusHandle* mutable_handle = (FuriHalSpiBusHandle*)handle;
    FuriHalSpiBus* bus = mutable_handle->bus;
    furi_check(bus);
    furi_hal_spi_shared_bus_lock_if_needed(bus);
    FuriMutex* mutex = furi_hal_spi_get_mutex(bus);
    furi_check(furi_mutex_acquire(mutex, FuriWaitForever) == FuriStatusOk);
    /* Same refusal as deinit: a handle in the middle of a transaction keeps its CS. */
    const bool idle = bus->current_handle != handle;
    if(idle) {
        /* Removing the device hands the old CS pad back to the GPIO driver. The
         * new device is created lazily by the next furi_hal_spi_acquire(). */
        if(mutable_handle->device) {
            spi_bus_remove_device((spi_device_handle_t)mutable_handle->device);
            mutable_handle->device = NULL;
        }
        mutable_handle->initialized = false;
        mutable_handle->cs = cs;
    }
    furi_check(furi_mutex_release(mutex) == FuriStatusOk);
    furi_hal_spi_shared_bus_unlock_if_needed(bus);
    return idle;
}

void furi_hal_spi_acquire(const FuriHalSpiBusHandle* handle) {
    furi_check(handle);

    FuriHalSpiBusHandle* mutable_handle = (FuriHalSpiBusHandle*)handle;
    FuriHalSpiBus* bus = mutable_handle->bus;
    furi_check(bus);

    furi_hal_spi_bus_handle_init(handle);
#if BOARD_CC1101_SPI_SHARED && FURI_HAL_SPI_RADIO_TAKES_GLOBAL_LOCK
    /* On shared-bus boards, also lock the global SPI bus mutex
     * to serialize with display DMA and SD card transactions. Yielding:
     * radio loops acquire thousands of times per second and must not starve
     * the display/SD waiting on the same lock. */
    if(!bus->bitbang) {
        furi_hal_spi_bus_lock_yielding();
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
#if BOARD_CC1101_SPI_SHARED && FURI_HAL_SPI_RADIO_TAKES_GLOBAL_LOCK
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
        if(!handle->initialized || !furi_hal_spi_pin_valid(handle->cs) ||
           !furi_hal_spi_pin_valid(handle->sck) || !furi_hal_spi_pin_valid(handle->mosi)) {
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
        ESP_LOGW(TAG, "SPI transaction skipped: device is not initialized");
        return false;
    }

    if(size > FURI_HAL_SPI_MAX_TRANSFER_SZ) {
        ESP_LOGE(TAG, "SPI transfer too large: %u bytes", (unsigned)size);
        furi_hal_spi_fill_stub_rx(rx_buffer, size);
        return false;
    }

    /* Per-bus internal-DRAM bounce (see FuriHalSpiBus.dma_tx): current_handle ==
     * handle was checked above, so nobody else uses this bus' buffers now. */
    _Static_assert(
        sizeof(((FuriHalSpiBus*)0)->dma_tx) >= FURI_HAL_SPI_TRX_STACK_BUF_SIZE,
        "bus bounce buffer smaller than FURI_HAL_SPI_TRX_STACK_BUF_SIZE");
    uint32_t* const tx_stack = handle->bus->dma_tx;
    uint32_t* const rx_stack = handle->bus->dma_rx;
    const uint8_t* safe_tx = tx_buffer;
    uint8_t* safe_rx = rx_buffer;
    uint8_t* heap_tx = NULL;
    uint8_t* heap_rx = NULL;

    if(tx_buffer && !furi_hal_spi_ptr_dma_safe(tx_buffer, size)) {
        if(size <= FURI_HAL_SPI_TRX_STACK_BUF_SIZE) {
            safe_tx = (const uint8_t*)tx_stack;
            memcpy(tx_stack, tx_buffer, size);
        } else {
            heap_tx = heap_caps_aligned_alloc(32, size, MALLOC_CAP_DMA);
            if(!heap_tx) goto transfer_failed;
            memcpy(heap_tx, tx_buffer, size);
            safe_tx = heap_tx;
        }
    }
    if(rx_buffer && !furi_hal_spi_ptr_dma_safe(rx_buffer, size)) {
        if(size <= FURI_HAL_SPI_TRX_STACK_BUF_SIZE) {
            safe_rx = (uint8_t*)rx_stack;
        } else {
            heap_rx = heap_caps_aligned_alloc(32, size, MALLOC_CAP_DMA);
            if(!heap_rx) goto transfer_failed;
            safe_rx = heap_rx;
        }
    }

    spi_transaction_t transaction = {
        .length = size * 8,
        .tx_buffer = safe_tx,
        .rx_buffer = safe_rx,
    };

    esp_err_t err =
        spi_device_polling_transmit((spi_device_handle_t)handle->device, &transaction);
    if(err != ESP_OK) {
        ESP_LOGW(TAG, "spi_device_polling_transmit failed: %s", esp_err_to_name(err));
        furi_hal_spi_fill_stub_rx(rx_buffer, size);
    } else if(rx_buffer && safe_rx != rx_buffer) {
        memcpy(rx_buffer, safe_rx, size);
    }

    if(heap_tx) heap_caps_free(heap_tx);
    if(heap_rx) heap_caps_free(heap_rx);
    return err == ESP_OK;

transfer_failed:
    if(heap_tx) heap_caps_free(heap_tx);
    if(heap_rx) heap_caps_free(heap_rx);
    ESP_LOGE(TAG, "SPI DMA bounce allocation failed for %u bytes", (unsigned)size);
    furi_hal_spi_fill_stub_rx(rx_buffer, size);
    return false;
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

    uint32_t dummy_stack[FURI_HAL_SPI_TRX_STACK_BUF_SIZE / sizeof(uint32_t)];
    uint8_t* dummy = (uint8_t*)dummy_stack;
    uint8_t* dummy_heap = NULL;
    if(size > sizeof(dummy_stack)) {
        dummy_heap = heap_caps_aligned_alloc(32, size, MALLOC_CAP_DMA);
        if(!dummy_heap) {
            furi_hal_spi_fill_stub_rx(buffer, size);
            return false;
        }
        dummy = dummy_heap;
    }

    memset(dummy, 0xFF, size);
    bool result = furi_hal_spi_bus_trx(handle, dummy, buffer, size, timeout);
    if(dummy_heap) heap_caps_free(dummy_heap);
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
