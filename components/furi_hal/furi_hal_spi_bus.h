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
/* NOTE [QF-2]: while chasing the qFlipper large-upload failure this pin was
 * temporarily reverted to ESP_INTR_CPU_AFFINITY_AUTO; that did NOT fix it (the
 * real cause was the TinyUSB CDC RX race, see furi_hal_usb_cdc.c). Keep the
 * core-0 pin: it prevents the permanent GUI freeze (lost wake-up in the IDF
 * SPI bus lock). Do not unpin it again as a transfer "fix" without new
 * evidence. */
#if CONFIG_FREERTOS_UNICORE
#define FURI_HAL_SPI_BUS_ISR_CPU_AFFINITY ESP_INTR_CPU_AFFINITY_AUTO
#define FURI_HAL_SPI_BUS_ISR_CORE         (-1) /* single core: nothing to pin */
#else
#define FURI_HAL_SPI_BUS_ISR_CPU_AFFINITY ESP_INTR_CPU_AFFINITY_0
#define FURI_HAL_SPI_BUS_ISR_CORE         0
#endif

void furi_hal_spi_bus_init(void);
void furi_hal_spi_bus_lock(void);
/* Try to take the bus, waiting at most timeout_ms. Returns true on success.
 * Used by the display path so a stuck SPI user (slow SD read, NFC) can never
 * freeze the UI forever: the frame is simply dropped and retried later. */
bool furi_hal_spi_bus_lock_timeout(uint32_t timeout_ms);
void furi_hal_spi_bus_unlock(void);
/* QFDIAG: copy the name of the thread holding the bus lock ("none" if free)
 * into out. Diagnostic only. */
void furi_hal_spi_bus_owner_name(char* out, size_t len);

#ifdef __cplusplus
}
#endif
