#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct { unsigned id; } SubGhzDevice;
typedef enum {
  FuriHalSubGhzPresetOok270Async,
  FuriHalSubGhzPresetOok650Async,
  FuriHalSubGhzPreset2FSKDev238Async,
  FuriHalSubGhzPreset2FSKDev12KAsync,
  FuriHalSubGhzPreset2FSKDev476Async,
  FuriHalSubGhzPresetCustom
} FuriHalSubGhzPreset;
void subghz_devices_init(void);
void subghz_devices_deinit(void);
const SubGhzDevice *subghz_devices_get_by_name(const char *);
bool subghz_devices_is_frequency_valid(const SubGhzDevice *, uint32_t);
void subghz_devices_idle(const SubGhzDevice *);
void subghz_devices_reset(const SubGhzDevice *);
void subghz_devices_load_preset(const SubGhzDevice *, FuriHalSubGhzPreset, uint8_t *);
uint32_t subghz_devices_set_frequency(const SubGhzDevice *, uint32_t);
void subghz_devices_flush_rx(const SubGhzDevice *);
void subghz_devices_start_async_rx(const SubGhzDevice *, void (*)(bool, uint32_t, void *), void *);
void subghz_devices_stop_async_rx(const SubGhzDevice *);
void subghz_devices_sleep(const SubGhzDevice *);
float subghz_devices_get_rssi(const SubGhzDevice *);
bool subghz_devices_start_async_tx(const SubGhzDevice *, void *, void *);
bool subghz_devices_is_async_complete_tx(const SubGhzDevice *);
void subghz_devices_stop_async_tx(const SubGhzDevice *);
