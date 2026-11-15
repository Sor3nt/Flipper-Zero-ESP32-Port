#pragma once

#include <furi.h>
#include <sdkconfig.h>
#include <esp_intr_types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Core that runs the SPI bus interrupt, and so the GUI thread too.
 *
 * The LCD sends pixel stripes as queued (interrupt-driven) transfers, and the
 * GUI thread is woken from inside that interrupt (flush done) and right away
 * asks for the bus again (esp_lcd tx_param -> spi_device_acquire_bus). On two
 * cores the GUI can set its bus-lock request while the interrupt, on the other
 * core, is between its last scheduling check and its exit; the interrupt then
 * leaves without handing the bus over and the GUI waits forever (ESP-IDF
 * spi_bus_lock bg_exit_core ignores LOCK bits when no device is acquiring;
 * same in IDF 5.5/6.0). On one core the woken GUI only runs after the
 * interrupt has returned, so the window cannot occur.
 *
 * Core 0: the bus is initialized from app_main (core 0), whose boot-time
 * display fill uses the same wait-then-acquire pattern. */
#if CONFIG_FREERTOS_UNICORE
#define FURI_HAL_SPI_BUS_ISR_CPU_AFFINITY ESP_INTR_CPU_AFFINITY_AUTO
#define FURI_HAL_SPI_BUS_ISR_CORE         (-1) /* single core: nothing to pin */
#else
#define FURI_HAL_SPI_BUS_ISR_CPU_AFFINITY ESP_INTR_CPU_AFFINITY_0
#define FURI_HAL_SPI_BUS_ISR_CORE         0
#endif

void furi_hal_spi_bus_init(void);
void furi_hal_spi_bus_lock(void);
void furi_hal_spi_bus_unlock(void);

#ifdef __cplusplus
}
#endif
