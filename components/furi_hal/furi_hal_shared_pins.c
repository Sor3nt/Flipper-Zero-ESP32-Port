#include "furi_hal_shared_pins.h"
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <driver/gpio.h>
#include "boards/board.h"
#if CONFIG_IDF_TARGET_ESP32S3
#include <soc/gpio_struct.h>
#include <soc/gpio_periph.h>
#include <soc/io_mux_reg.h>
#include <soc/gpio_sig_map.h>
#include <soc/soc.h>
#include <esp_private/esp_gpio_reserve.h>
#endif

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static const void* pin_owner;
static bool saved;
#if CONFIG_IDF_TARGET_ESP32S3
static uint32_t mux[2], pin[2], output_route[2], output, enable, rx_route;
static uint64_t previous_reservations;
#endif

bool furi_hal_shared_pins_is_saved(const void* owner) {
    portENTER_CRITICAL(&lock);
    bool ok = owner && pin_owner == owner && saved;
    portEXIT_CRITICAL(&lock);
    return ok;
}

bool furi_hal_shared_pins_acquire(const void* owner) {
    if(!owner) return false;
    portENTER_CRITICAL(&lock);
    bool ok = !pin_owner;
    if(ok) pin_owner = owner;
    portEXIT_CRITICAL(&lock);
    return ok;
}

void furi_hal_shared_pins_release(const void* owner) {
    portENTER_CRITICAL(&lock);
    if(pin_owner == owner && !saved) pin_owner = NULL;
    portEXIT_CRITICAL(&lock);
}

bool furi_hal_shared_pins_save(const void* owner) {
    bool ok = false;
    portENTER_CRITICAL(&lock);
#if CONFIG_IDF_TARGET_ESP32S3 && BOARD_PIN_RFID_TX == 43 && BOARD_PIN_RFID_RX == 44
    if(owner && pin_owner == owner && !saved) {
        previous_reservations = 0;
        for(int i = 0; i < 2; ++i) {
            if(esp_gpio_is_reserved(1ULL << (43 + i))) previous_reservations |= 1ULL << (43 + i);
            mux[i] = REG_READ(GPIO_PIN_MUX_REG[43 + i]);
            pin[i] = GPIO.pin[43 + i].val;
            output_route[i] = GPIO.func_out_sel_cfg[43 + i].val;
        }
        output = GPIO.out1.val;
        enable = GPIO.enable1.val;
        rx_route = GPIO.func_in_sel_cfg[U1RXD_IN_IDX].val;
        saved = ok = true;
    }
#endif
    portEXIT_CRITICAL(&lock);
    return ok;
}

void furi_hal_shared_pins_restore(const void* owner) {
    portENTER_CRITICAL(&lock);
#if CONFIG_IDF_TARGET_ESP32S3
    if(owner && pin_owner == owner && saved) {
        const uint32_t mask = (1U << (43 - 32)) | (1U << (44 - 32));
        GPIO.enable1_w1tc.val = mask;
        GPIO.out1_w1ts.val = output & mask;
        GPIO.out1_w1tc.val = ~output & mask;
        GPIO.func_in_sel_cfg[U1RXD_IN_IDX].val = rx_route;
        for(int i = 0; i < 2; ++i) {
            GPIO.func_out_sel_cfg[43 + i].val = output_route[i];
            GPIO.pin[43 + i].val = pin[i];
            REG_WRITE(GPIO_PIN_MUX_REG[43 + i], mux[i]);
        }
        GPIO.enable1_w1ts.val = enable & mask;
        /* uart_driver_delete does not undo uart_set_pin's GPIO reservation. */
        esp_gpio_revoke(((1ULL << 43) | (1ULL << 44)) & ~previous_reservations);
        saved = false;
    }
#endif
    portEXIT_CRITICAL(&lock);
}
