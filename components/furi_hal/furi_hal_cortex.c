#include "furi_hal_cortex.h"
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <rom/ets_sys.h>

void furi_hal_cortex_init_early(void) {
    // No-op on ESP32: no DWT cycle counter to configure
}

void furi_hal_cortex_delay_us(uint32_t microseconds) {
    ets_delay_us(microseconds);
}

uint32_t furi_hal_cortex_instructions_per_microsecond(void) {
    /* Not a constant: this port scales the CPU between 80 and 160 MHz (DFS), and the
     * old fixed 240 overstated it by 50-200%. The ROM keeps the live value (ticks per us). */
    return esp_rom_get_cpu_ticks_per_us();
}

FuriHalCortexTimer furi_hal_cortex_timer_get(uint32_t timeout_us) {
    FuriHalCortexTimer timer = {
        .start = (uint32_t)esp_timer_get_time(),
        .value = timeout_us,
    };
    return timer;
}

bool furi_hal_cortex_timer_is_expired(FuriHalCortexTimer cortex_timer) {
    uint32_t now = (uint32_t)esp_timer_get_time();
    return (now - cortex_timer.start) >= cortex_timer.value;
}

void furi_hal_cortex_timer_wait(FuriHalCortexTimer cortex_timer) {
    while(!furi_hal_cortex_timer_is_expired(cortex_timer)) {
        // Busy wait
    }
}
