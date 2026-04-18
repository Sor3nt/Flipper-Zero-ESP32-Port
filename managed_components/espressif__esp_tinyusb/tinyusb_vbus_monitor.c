/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "tinyusb_vbus_monitor.h"

#include "driver/gpio.h"     // This implementation of VBUS monitoring is based on GPIO interrupt
#include "device/dcd.h"      // For sending events to TinyUSB DCD layer

const static char *TAG = "VBUS mon";

/**
 * @brief VBUS monitoring context
 *
 * Only one instance of VBUS monitoring is supported at the moment.
 */
typedef struct {
    gpio_num_t gpio_num;            /*!< GPIO number used for VBUS monitoring */
    int port;                        /*!< USB port number */
} vbus_monitor_context_t;

static vbus_monitor_context_t s_vbus_ctx = {
    .gpio_num = GPIO_NUM_NC,
    .port = 0,
};

// -------------- VBUS Internal Logic ------------------

/**
 * @brief GPIO interrupt handler for VBUS monitoring io
 *
 * @param arg VBUS monitoring context
 */
static void vbus_io_cb(void *arg)
{
    vbus_monitor_context_t *ctx = (vbus_monitor_context_t *)arg;
    if (ctx == NULL) {
        return;
    }
    gpio_intr_disable(ctx->gpio_num);

    const bool vbus_curr_state = gpio_get_level(ctx->gpio_num);

    if (!vbus_curr_state) {
        dcd_event_bus_signal(ctx->port, DCD_EVENT_UNPLUGGED, true);
    }
    gpio_intr_enable(ctx->gpio_num);
}

// -------------- Public API ------------------

esp_err_t tinyusb_vbus_monitor_init(const tinyusb_vbus_monitor_config_t *config)
{
    esp_err_t ret = ESP_OK;
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid argument: config is NULL");
    ESP_RETURN_ON_FALSE(s_vbus_ctx.gpio_num == GPIO_NUM_NC, ESP_ERR_INVALID_STATE, TAG, "Already initialized");

    // Init gpio IRQ for VBUS monitoring
    const gpio_config_t vbus_io_cfg = {
        .pin_bit_mask = BIT64(config->gpio_num),
        .mode = GPIO_MODE_INPUT,
        .intr_type = GPIO_INTR_NEGEDGE,
        .hys_ctrl_mode = GPIO_HYS_SOFT_ENABLE, // Enable hysteresis. Low->High = 1.7V, High->Low = 1.4V
    };

    ESP_RETURN_ON_ERROR(gpio_config(&vbus_io_cfg), TAG, "Failed to configure VBUS GPIO%d", config->gpio_num);
    ESP_GOTO_ON_ERROR(gpio_isr_handler_add(config->gpio_num, vbus_io_cb, (void *)&s_vbus_ctx),
                      isr_err,
                      TAG,
                      "Failed to add ISR handler. Please install ISR service with gpio_install_isr_service().");

    s_vbus_ctx.gpio_num   = config->gpio_num;
    s_vbus_ctx.port       = config->port;

    return ESP_OK;

isr_err:
    gpio_reset_pin(config->gpio_num);
    return ret;
}

void tinyusb_vbus_monitor_deinit(void)
{
    if (s_vbus_ctx.gpio_num == GPIO_NUM_NC) {
        return;
    }
    // Reset GPIO to the default state
    gpio_reset_pin(s_vbus_ctx.gpio_num);
    // Remove gpio IRQ for VBUS monitoring - this can fail only if another thread uninstalled GPIO ISR service-> nothing to do if it fails
    gpio_isr_handler_remove(s_vbus_ctx.gpio_num);
    s_vbus_ctx.gpio_num = GPIO_NUM_NC;
}
