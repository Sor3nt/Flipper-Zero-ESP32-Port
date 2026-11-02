#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include <furi_hal_gpio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FuriHalSpiBus FuriHalSpiBus;
typedef struct FuriHalSpiBusHandle FuriHalSpiBusHandle;

struct FuriHalSpiBus {
    int host_id;
    bool bitbang;
    bool initialized;
    void* mutex;
    const FuriHalSpiBusHandle* current_handle;
    uint16_t mosi_pin;
    uint16_t miso_pin;
    uint16_t sck_pin;
    /* Bounce buffers for small transfers whose caller buffer is not DMA-capable.
     * They live in the bus struct (internal DRAM) rather than on the caller's
     * stack, because task stacks are PSRAM-first: a stack copy would not be
     * DMA-capable either and the IDF would then malloc an internal bounce per
     * transaction, which fails (ESP_ERR_NO_MEM) exactly when internal RAM is
     * exhausted. Only used while this bus' current_handle is held, so they are
     * serialized by the bus mutex. */
    uint32_t dma_tx[16];
    uint32_t dma_rx[16];
};

struct FuriHalSpiBusHandle {
    FuriHalSpiBus* bus;
    const GpioPin* miso;
    const GpioPin* mosi;
    const GpioPin* sck;
    const GpioPin* cs;
    void* device;
    bool initialized;
    uint32_t frequency_hz;
    uint8_t mode;
};

extern FuriHalSpiBus furi_hal_spi_bus_external;
extern FuriHalSpiBus furi_hal_spi_bus_subghz;

extern FuriHalSpiBusHandle furi_hal_spi_bus_handle_external;
extern FuriHalSpiBusHandle furi_hal_spi_bus_handle_subghz;
extern FuriHalSpiBusHandle furi_hal_spi_bus_handle_nrf24;

#ifdef __cplusplus
}
#endif
