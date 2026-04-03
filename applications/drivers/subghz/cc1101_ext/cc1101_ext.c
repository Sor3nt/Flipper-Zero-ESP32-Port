#include "cc1101_ext.h"

#include <furi_hal.h>

typedef struct {
    bool initialized;
    bool extended_range;
    bool amp_and_leds;
} SubGhzDeviceCC1101ExtState;

static SubGhzDeviceCC1101ExtState subghz_device_cc1101_ext = {
    .initialized = false,
    .extended_range = false,
    .amp_and_leds = false,
};

bool subghz_device_cc1101_ext_alloc(SubGhzDeviceConf* conf) {
    subghz_device_cc1101_ext.initialized = true;
    subghz_device_cc1101_ext.extended_range = conf ? conf->extended_range : false;
    subghz_device_cc1101_ext.amp_and_leds = conf ? conf->amp_and_leds : false;

    furi_hal_subghz_set_ext_leds_and_amp(subghz_device_cc1101_ext.amp_and_leds);
    furi_hal_subghz_init();
    return true;
}

void subghz_device_cc1101_ext_free(void) {
    if(!subghz_device_cc1101_ext.initialized) {
        return;
    }

    furi_hal_subghz_shutdown();
    subghz_device_cc1101_ext.initialized = false;
}

void subghz_device_cc1101_ext_set_async_mirror_pin(const GpioPin* pin) {
    furi_hal_subghz_set_async_mirror_pin(pin);
}

const GpioPin* subghz_device_cc1101_ext_get_data_gpio(void) {
    return furi_hal_subghz_get_data_gpio();
}

bool subghz_device_cc1101_ext_is_connect(void) {
    furi_hal_subghz_reset();
    return true;
}

void subghz_device_cc1101_ext_sleep(void) {
    furi_hal_subghz_sleep();
}

void subghz_device_cc1101_ext_dump_state(void) {
    furi_hal_subghz_dump_state();
}

void subghz_device_cc1101_ext_load_custom_preset(const uint8_t* preset_data) {
    furi_hal_subghz_load_custom_preset(preset_data);
}

void subghz_device_cc1101_ext_load_registers(const uint8_t* data) {
    furi_hal_subghz_load_registers(data);
}

void subghz_device_cc1101_ext_load_patable(const uint8_t data[8]) {
    furi_hal_subghz_load_patable(data);
}

void subghz_device_cc1101_ext_write_packet(const uint8_t* data, uint8_t size) {
    furi_hal_subghz_write_packet(data, size);
}

bool subghz_device_cc1101_ext_rx_pipe_not_empty(void) {
    return furi_hal_subghz_rx_pipe_not_empty();
}

bool subghz_device_cc1101_ext_is_rx_data_crc_valid(void) {
    return furi_hal_subghz_is_rx_data_crc_valid();
}

void subghz_device_cc1101_ext_read_packet(uint8_t* data, uint8_t* size) {
    furi_hal_subghz_read_packet(data, size);
}

void subghz_device_cc1101_ext_flush_rx(void) {
    furi_hal_subghz_flush_rx();
}

void subghz_device_cc1101_ext_flush_tx(void) {
    furi_hal_subghz_flush_tx();
}

void subghz_device_cc1101_ext_shutdown(void) {
    furi_hal_subghz_shutdown();
}

void subghz_device_cc1101_ext_reset(void) {
    furi_hal_subghz_reset();
}

void subghz_device_cc1101_ext_idle(void) {
    furi_hal_subghz_idle();
}

void subghz_device_cc1101_ext_rx(void) {
    furi_hal_subghz_rx();
}

bool subghz_device_cc1101_ext_tx(void) {
    return furi_hal_subghz_tx();
}

float subghz_device_cc1101_ext_get_rssi(void) {
    return furi_hal_subghz_get_rssi();
}

uint8_t subghz_device_cc1101_ext_get_lqi(void) {
    return furi_hal_subghz_get_lqi();
}

bool subghz_device_cc1101_ext_is_frequency_valid(uint32_t value) {
    return subghz_device_cc1101_ext.extended_range ?
               furi_hal_subghz_is_frequency_valid(value) :
               (furi_hal_subghz_is_tx_allowed(value) || furi_hal_subghz_is_frequency_valid(value));
}

uint32_t subghz_device_cc1101_ext_set_frequency(uint32_t value) {
    return furi_hal_subghz_set_frequency(value);
}

void subghz_device_cc1101_ext_start_async_rx(
    SubGhzDeviceCC1101ExtCaptureCallback callback,
    void* context) {
    furi_hal_subghz_start_async_rx((FuriHalSubGhzCaptureCallback)callback, context);
}

void subghz_device_cc1101_ext_stop_async_rx(void) {
    furi_hal_subghz_stop_async_rx();
}

bool subghz_device_cc1101_ext_start_async_tx(SubGhzDeviceCC1101ExtCallback callback, void* context) {
    return furi_hal_subghz_start_async_tx((FuriHalSubGhzAsyncTxCallback)callback, context);
}

bool subghz_device_cc1101_ext_is_async_tx_complete(void) {
    return furi_hal_subghz_is_async_tx_complete();
}

void subghz_device_cc1101_ext_stop_async_tx(void) {
    furi_hal_subghz_stop_async_tx();
}
