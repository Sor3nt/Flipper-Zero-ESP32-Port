#include "furi_hal_random.h"
#include <esp_random.h>

void furi_hal_random_init(void) {
    // ESP32 HW-RNG is always active, no init needed
}

uint32_t furi_hal_random_get(void) {
    return esp_random();
}

void furi_hal_random_fill_buf(uint8_t* buf, uint32_t len) {
    esp_fill_random(buf, len);
}
