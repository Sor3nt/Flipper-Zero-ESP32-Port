/**
 * @file furi_hal_reboot_combo.c
 * Forced reboot combo, see furi_hal_reboot_combo.h.
 */

#include "furi_hal_reboot_combo.h"
#include "boards/board.h"

#include <sdkconfig.h>

#if CONFIG_FURI_HAL_REBOOT_COMBO && defined(BOARD_REBOOT_COMBO_PIN_A) && \
    defined(BOARD_REBOOT_COMBO_PIN_B)

#if CONFIG_FREERTOS_UNICORE || !CONFIG_ESP_TIMER_SUPPORTS_ISR_DISPATCH_METHOD
#error "Reboot combo needs a dual-core build with esp_timer ISR dispatch"
#endif

#include <esp_attr.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <soc/gpio_reg.h>
#include <soc/rtc_cntl_reg.h>
#include <soc/soc.h>

#define TAG "RebootCombo"

#define REBOOT_COMBO_PERIOD_US    100000
#define REBOOT_COMBO_HOLD_SAMPLES 50U /* 5 s at 100 ms */

typedef enum {
    RebootComboIdle,
    RebootComboWaitBootRelease, /* combo was held at boot: ignore until released */
} RebootComboState;

static esp_timer_handle_t reboot_combo_timer = NULL;
static DRAM_ATTR volatile RebootComboState reboot_combo_state = RebootComboIdle;
static DRAM_ATTR volatile uint32_t reboot_combo_count = 0;
static DRAM_ATTR volatile bool reboot_combo_held = false;

static inline bool IRAM_ATTR reboot_combo_pin_low(uint32_t pin) {
    const uint32_t in = pin < 32 ? REG_READ(GPIO_IN_REG) : REG_READ(GPIO_IN1_REG);
    return (in & (1UL << (pin & 31U))) == 0;
}

static void IRAM_ATTR reboot_combo_timer_cb(void* arg) {
    (void)arg;
    const bool a = reboot_combo_pin_low(BOARD_REBOOT_COMBO_PIN_A);
    const bool b = reboot_combo_pin_low(BOARD_REBOOT_COMBO_PIN_B);
    const bool both = a && b;
    const bool none = !a && !b;
    reboot_combo_held = both;

    switch(reboot_combo_state) {
    case RebootComboWaitBootRelease:
        if(none) {
            reboot_combo_state = RebootComboIdle;
            reboot_combo_count = 0;
        }
        break;
    case RebootComboIdle:
        if(both) {
            if(++reboot_combo_count >= REBOOT_COMBO_HOLD_SAMPLES) {
                /* BOOT/strap pins are not re-latched by this software reset. */
                REG_CLR_BIT(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
                esp_rom_software_reset_system();
            }
        } else {
            reboot_combo_count = 0;
        }
        break;
    }
}

void furi_hal_reboot_combo_init(void) {
    if(reboot_combo_timer) return;

    reboot_combo_count = 0;
    /* Ignore a combo that is already fully held when the service starts,
     * which can happen across a reset and would otherwise cause a reboot loop.
     * A single button held at startup must not disable the combo: once the
     * second button is pressed, the normal 5 s hold timer should run. */
    reboot_combo_state =
        (reboot_combo_pin_low(BOARD_REBOOT_COMBO_PIN_A) &&
         reboot_combo_pin_low(BOARD_REBOOT_COMBO_PIN_B)) ?
            RebootComboWaitBootRelease :
            RebootComboIdle;

    const esp_timer_create_args_t args = {
        .callback = reboot_combo_timer_cb,
        .dispatch_method = ESP_TIMER_ISR,
        /* Do not wake light sleep ten times a second: while asleep nothing
         * is held (a press wakes the SoC first). */
        .skip_unhandled_events = true,
        .name = "reboot_combo",
    };
    esp_err_t err = esp_timer_create(&args, &reboot_combo_timer);
    if(err == ESP_OK) err = esp_timer_start_periodic(reboot_combo_timer, REBOOT_COMBO_PERIOD_US);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "Reboot combo timer failed: %s", esp_err_to_name(err));
        if(reboot_combo_timer) {
            esp_timer_delete(reboot_combo_timer);
            reboot_combo_timer = NULL;
        }
        return;
    }
    ESP_LOGI(
        TAG,
        "Hold GPIO%d + GPIO%d for 5 s to reboot",
        BOARD_REBOOT_COMBO_PIN_A,
        BOARD_REBOOT_COMBO_PIN_B);
}

bool furi_hal_reboot_combo_is_held(void) {
    return reboot_combo_held;
}

#else

void furi_hal_reboot_combo_init(void) {
}

bool furi_hal_reboot_combo_is_held(void) {
    return false;
}

#endif
