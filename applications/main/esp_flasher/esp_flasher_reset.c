#include "esp_flasher_reset.h"
#include "esp_flasher_hw.h"

#include <furi.h>

#if ESP_FLASHER_HAS_RESET_PINS
#include <driver/gpio.h>
#include <furi_hal_gpio.h>
#endif

#define TAG "EspFlasherReset"

#define ESP_FLASHER_RESET_HOLD_MS (100)
#define ESP_FLASHER_BOOT_HOLD_MS  (50)

#if ESP_FLASHER_HAS_RESET_PINS

/* EN/IO0 alias the USB-UART Bridge's RTS/DTR header GPIOs (see esp_flasher_hw.h); claimed
 * through the GPIO arbiter so an external Sub-GHz/NRF24 module already on one of them is
 * never also driven as a reset line, and vice versa. The two features never actually run
 * at once (they share the same UART peripheral via furi_hal_uart_shared), so this only
 * ever contends with an external radio module, never with the bridge itself. */
static const char ESP_FLASHER_GPIO_OWNER[] = "ESP Flasher";
static bool s_pins_reserved = false;

static bool esp_flasher_reset_reserve_pins(void) {
    if(s_pins_reserved) return true;
    const GpioPin en = {.port = NULL, .pin = (uint16_t)ESP_FLASHER_EN_GPIO};
    const GpioPin io0 = {.port = NULL, .pin = (uint16_t)ESP_FLASHER_IO0_GPIO};
    if(!furi_hal_gpio_reserve(&en, ESP_FLASHER_GPIO_OWNER)) {
        FURI_LOG_E(
            TAG, "EN GPIO%d is in use by %s", (int)ESP_FLASHER_EN_GPIO,
            furi_hal_gpio_get_reservation(&en));
        return false;
    }
    if(!furi_hal_gpio_reserve(&io0, ESP_FLASHER_GPIO_OWNER)) {
        FURI_LOG_E(
            TAG, "IO0 GPIO%d is in use by %s", (int)ESP_FLASHER_IO0_GPIO,
            furi_hal_gpio_get_reservation(&io0));
        furi_hal_gpio_unreserve(&en, ESP_FLASHER_GPIO_OWNER);
        return false;
    }
    s_pins_reserved = true;
    return true;
}

static void esp_flasher_reset_unreserve_pins(void) {
    if(!s_pins_reserved) return;
    const GpioPin en = {.port = NULL, .pin = (uint16_t)ESP_FLASHER_EN_GPIO};
    const GpioPin io0 = {.port = NULL, .pin = (uint16_t)ESP_FLASHER_IO0_GPIO};
    furi_hal_gpio_unreserve(&en, ESP_FLASHER_GPIO_OWNER);
    furi_hal_gpio_unreserve(&io0, ESP_FLASHER_GPIO_OWNER);
    s_pins_reserved = false;
}

static EspFlasherResetMode s_driven_mode = EspFlasherResetModeOff;

static int esp_flasher_reset_level(EspFlasherResetMode mode, bool asserted) {
    const int active = (mode == EspFlasherResetModeFlipper) ? 1 : 0;
    return asserted ? active : !active;
}

static void esp_flasher_reset_release_pin(gpio_num_t pin) {
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(pin, GPIO_FLOATING);
}

static void esp_flasher_reset_drive_pin(gpio_num_t pin, int level) {
    /* Preload the level so switching to output does not glitch the target. */
    gpio_set_level(pin, level);
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    const esp_err_t err = gpio_config(&config);
    if(err != ESP_OK) FURI_LOG_E(TAG, "GPIO%d output failed: %s", (int)pin, esp_err_to_name(err));
}

static void esp_flasher_reset_set(gpio_num_t pin, EspFlasherResetMode mode, bool asserted) {
    gpio_set_level(pin, esp_flasher_reset_level(mode, asserted));
}

bool esp_flasher_reset_available(void) {
    return true;
}

bool esp_flasher_reset_lines_init(EspFlasherResetMode mode) {
    if(mode == EspFlasherResetModeOff || mode >= EspFlasherResetModeCount) {
        esp_flasher_reset_lines_deinit();
        return true;
    }
    if(!esp_flasher_reset_reserve_pins()) {
        /* Pin claimed elsewhere (e.g. an attached external Sub-GHz/NRF24 module): leave the
         * lines undriven rather than fighting the other owner for them. */
        s_driven_mode = EspFlasherResetModeOff;
        return false;
    }
    esp_flasher_reset_drive_pin(
        (gpio_num_t)ESP_FLASHER_EN_GPIO, esp_flasher_reset_level(mode, false));
    esp_flasher_reset_drive_pin(
        (gpio_num_t)ESP_FLASHER_IO0_GPIO, esp_flasher_reset_level(mode, false));
    s_driven_mode = mode;
    return true;
}

void esp_flasher_reset_lines_deinit(void) {
    esp_flasher_reset_unreserve_pins();
    if(s_driven_mode == EspFlasherResetModeOff) return;
    esp_flasher_reset_release_pin((gpio_num_t)ESP_FLASHER_EN_GPIO);
    esp_flasher_reset_release_pin((gpio_num_t)ESP_FLASHER_IO0_GPIO);
    s_driven_mode = EspFlasherResetModeOff;
}

/* False means the lines are not actually driven for `mode` (Off/invalid, or the pins
 * could not be claimed) -- callers must not pretend a reset/bootloader pulse happened. */
static bool esp_flasher_reset_prepare(EspFlasherResetMode mode) {
    if(mode == EspFlasherResetModeOff || mode >= EspFlasherResetModeCount) return false;
    if(s_driven_mode != mode) esp_flasher_reset_lines_init(mode);
    return s_driven_mode == mode;
}

bool esp_flasher_reset_target(EspFlasherResetMode mode) {
    if(!esp_flasher_reset_prepare(mode)) return false;
    const gpio_num_t en = (gpio_num_t)ESP_FLASHER_EN_GPIO;
    const gpio_num_t io0 = (gpio_num_t)ESP_FLASHER_IO0_GPIO;

    esp_flasher_reset_set(io0, mode, false);
    esp_flasher_reset_set(en, mode, true);
    furi_delay_ms(ESP_FLASHER_RESET_HOLD_MS);
    esp_flasher_reset_set(en, mode, false);
    return true;
}

bool esp_flasher_reset_enter_bootloader(EspFlasherResetMode mode) {
    if(!esp_flasher_reset_prepare(mode)) return false;
    const gpio_num_t en = (gpio_num_t)ESP_FLASHER_EN_GPIO;
    const gpio_num_t io0 = (gpio_num_t)ESP_FLASHER_IO0_GPIO;

    /* esptool ClassicReset order: hold in reset, pull IO0 low, release reset while IO0
     * is still low so the ROM samples download mode, then release IO0. */
    esp_flasher_reset_set(io0, mode, false);
    esp_flasher_reset_set(en, mode, true);
    furi_delay_ms(ESP_FLASHER_RESET_HOLD_MS);
    esp_flasher_reset_set(io0, mode, true);
    esp_flasher_reset_set(en, mode, false);
    furi_delay_ms(ESP_FLASHER_BOOT_HOLD_MS);
    esp_flasher_reset_set(io0, mode, false);
    return true;
}

#else /* !ESP_FLASHER_HAS_RESET_PINS */

bool esp_flasher_reset_available(void) {
    return false;
}

bool esp_flasher_reset_lines_init(EspFlasherResetMode mode) {
    UNUSED(mode);
    return false;
}

void esp_flasher_reset_lines_deinit(void) {
}

bool esp_flasher_reset_target(EspFlasherResetMode mode) {
    UNUSED(mode);
    return false;
}

bool esp_flasher_reset_enter_bootloader(EspFlasherResetMode mode) {
    UNUSED(mode);
    return false;
}

#endif /* ESP_FLASHER_HAS_RESET_PINS */
