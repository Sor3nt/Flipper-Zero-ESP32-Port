/**
 * @file furi_hal_gpio.h
 * GPIO HAL API (ESP32 stub)
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Number of gpio on one port
 */
#define GPIO_NUMBER (16U)

/**
 * Interrupt callback prototype
 */
typedef void (*GpioExtiCallback)(void* ctx);

/**
 * Gpio interrupt type
 */
typedef struct {
    GpioExtiCallback callback;
    void* context;
} GpioInterrupt;

/**
 * Gpio modes
 */
typedef enum {
    GpioModeInput,
    GpioModeOutputPushPull,
    GpioModeOutputOpenDrain,
    GpioModeAltFunctionPushPull,
    GpioModeAltFunctionOpenDrain,
    GpioModeAnalog,
    GpioModeInterruptRise,
    GpioModeInterruptFall,
    GpioModeInterruptRiseFall,
    GpioModeEventRise,
    GpioModeEventFall,
    GpioModeEventRiseFall,
} GpioMode;

/**
 * Gpio pull modes
 */
typedef enum {
    GpioPullNo,
    GpioPullUp,
    GpioPullDown,
} GpioPull;

/**
 * Gpio speed modes
 */
typedef enum {
    GpioSpeedLow,
    GpioSpeedMedium,
    GpioSpeedHigh,
    GpioSpeedVeryHigh,
} GpioSpeed;

/**
 * Gpio alternate functions (stub - values not meaningful on ESP32)
 */
typedef enum {
    GpioAltFnUnused = 0,
} GpioAltFn;

/**
 * Gpio structure
 * On ESP32 we use a simple pin number instead of port+pin.
 */
typedef struct {
    void* port; /**< GPIO port (unused on ESP32, kept for API compatibility) */
    uint16_t pin; /**< Pin number */
} GpioPin;

/**
 * GPIO initialization function, simple version
 * @param gpio  GpioPin
 * @param mode  GpioMode
 */
void furi_hal_gpio_init_simple(const GpioPin* gpio, const GpioMode mode);

/**
 * GPIO initialization function, normal version
 * @param gpio  GpioPin
 * @param mode  GpioMode
 * @param pull  GpioPull
 * @param speed GpioSpeed
 */
void furi_hal_gpio_init(
    const GpioPin* gpio,
    const GpioMode mode,
    const GpioPull pull,
    const GpioSpeed speed);

/**
 * GPIO initialization function, extended version
 * @param gpio  GpioPin
 * @param mode  GpioMode
 * @param pull  GpioPull
 * @param speed GpioSpeed
 * @param alt_fn GpioAltFn
 */
void furi_hal_gpio_init_ex(
    const GpioPin* gpio,
    const GpioMode mode,
    const GpioPull pull,
    const GpioSpeed speed,
    const GpioAltFn alt_fn);

/**
 * Add and enable interrupt
 * @param gpio GpioPin
 * @param cb   GpioExtiCallback
 * @param ctx  context for callback
 */
void furi_hal_gpio_add_int_callback(const GpioPin* gpio, GpioExtiCallback cb, void* ctx);

/**
 * Enable interrupt
 * @param gpio GpioPin
 */
void furi_hal_gpio_enable_int_callback(const GpioPin* gpio);

/**
 * Disable interrupt
 * @param gpio GpioPin
 */
void furi_hal_gpio_disable_int_callback(const GpioPin* gpio);

/**
 * Remove interrupt
 * @param gpio GpioPin
 */
void furi_hal_gpio_remove_int_callback(const GpioPin* gpio);

/**
 * GPIO write pin
 * @param gpio  GpioPin
 * @param state true / false
 */
void furi_hal_gpio_write(const GpioPin* gpio, const bool state);

/**
 * GPIO read pin
 * @param gpio GpioPin
 * @return true / false
 */
bool furi_hal_gpio_read(const GpioPin* gpio);

/**
 * Install the shared GPIO interrupt service. Called once from furi_hal_init()
 * so the service (and every GPIO ISR) is allocated on the boot core instead
 * of whichever core a later caller happens to run on.
 */
void furi_hal_gpio_init_isr_service(void);

/**
 * Check whether a GPIO is configured as an input or output.
 * Bidirectional pins report true for both checks.
 */
bool furi_hal_gpio_is_input(const GpioPin* gpio);
bool furi_hal_gpio_is_output(const GpioPin* gpio);

/**
 * Reserve a pin for a HAL-driven peripheral (e.g. external IR TX).
 *
 * While reserved, furi_hal_gpio_init*() calls from anyone else are ignored, so
 * scripts or apps cannot reroute a pin the peripheral is currently driving.
 * Nothing is reserved unless a driver asks for it, so boards without such a
 * driver behave exactly as before.
 *
 * @param gpio  pin to reserve
 * @param owner static string identifying the owner, also shown to users
 * @return true if the pin was free or is already held by the same owner
 */
bool furi_hal_gpio_reserve(const GpioPin* gpio, const char* owner);

/**
 * Same as furi_hal_gpio_init(), but a pin reserved by `owner` is configured too.
 * `owner` must be the same string pointer passed to furi_hal_gpio_reserve().
 */
void furi_hal_gpio_init_owned(
    const GpioPin* gpio,
    const GpioMode mode,
    const GpioPull pull,
    const GpioSpeed speed,
    const char* owner);

/** Release a reservation taken by the same owner with furi_hal_gpio_reserve(). */
void furi_hal_gpio_unreserve(const GpioPin* gpio, const char* owner);

/** @return owner name of a reserved pin, or NULL if the pin is free */
const char* furi_hal_gpio_get_reservation(const GpioPin* gpio);

#ifdef __cplusplus
}
#endif
