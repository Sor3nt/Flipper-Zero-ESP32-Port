/* "Identify" feature (id 0): blink the onboard LED so you can physically tell
 * which buddy the master selected. Pure demonstrator for the feature pipeline. */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_log.h"

#include "buddy_config.h"
#include "buddy_features.h"
#include "buddy_node.h"

#define FEAT_ID 0

static const char* TAG = "feat-identify";

#if BUDDY_IDENTIFY_LED_GPIO >= 0
static volatile bool s_run;
static TaskHandle_t s_task;

static void blink_task(void* arg) {
    (void)arg;
    gpio_reset_pin(BUDDY_IDENTIFY_LED_GPIO);
    gpio_set_direction(BUDDY_IDENTIFY_LED_GPIO, GPIO_MODE_OUTPUT);
    int level = 0;
    while(s_run) {
        gpio_set_level(BUDDY_IDENTIFY_LED_GPIO, level);
        level ^= 1;
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    gpio_set_level(BUDDY_IDENTIFY_LED_GPIO, 0);
    s_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t feat_start(const uint8_t* args, uint8_t arg_len) {
    (void)args;
    (void)arg_len;
    if(!s_run) {
        s_run = true;
        xTaskCreate(blink_task, "identify", 2048, NULL, 4, &s_task);
    }
    buddy_node_feature_status(FEAT_ID, BuddyFeatStateRunning, NULL, 0);
    return ESP_OK;
}

static esp_err_t feat_stop(void) {
    s_run = false; /* blink_task exits and clears the LED */
    buddy_node_feature_status(FEAT_ID, BuddyFeatStateStopped, NULL, 0);
    return ESP_OK;
}

static bool feat_is_running(void) {
    return s_run;
}
#else
static esp_err_t feat_start(const uint8_t* args, uint8_t arg_len) {
    (void)args;
    (void)arg_len;
    ESP_LOGW(TAG, "no identify LED configured");
    buddy_node_feature_status(FEAT_ID, BuddyFeatStateError, NULL, 0);
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t feat_stop(void) {
    buddy_node_feature_status(FEAT_ID, BuddyFeatStateStopped, NULL, 0);
    return ESP_OK;
}

static bool feat_is_running(void) {
    return false;
}
#endif

const BuddyFeature buddy_feature_identify = {
    .id = FEAT_ID,
    .name = "Identify",
    .start = feat_start,
    .stop = feat_stop,
    .is_running = feat_is_running,
};
