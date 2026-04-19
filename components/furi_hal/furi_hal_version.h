#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <version.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FURI_HAL_VERSION_NAME_LENGTH        (8)
#define FURI_HAL_VERSION_ARRAY_NAME_LENGTH  (FURI_HAL_VERSION_NAME_LENGTH + 1)
#ifndef FURI_HAL_BT_ADV_NAME_LENGTH
#define FURI_HAL_BT_ADV_NAME_LENGTH         (32)
#endif
#define FURI_HAL_VERSION_DEVICE_NAME_LENGTH (1 + FURI_HAL_BT_ADV_NAME_LENGTH)

typedef enum {
    FuriHalVersionOtpVersion0 = 0x00,
    FuriHalVersionOtpVersion1 = 0x01,
    FuriHalVersionOtpVersion2 = 0x02,
    FuriHalVersionOtpVersionEmpty = 0xFFFFFFFE,
    FuriHalVersionOtpVersionUnknown = 0xFFFFFFFF,
} FuriHalVersionOtpVersion;

typedef enum {
    FuriHalVersionColorUnknown = 0x00,
    FuriHalVersionColorBlack = 0x01,
    FuriHalVersionColorWhite = 0x02,
    FuriHalVersionColorTransparent = 0x03,
} FuriHalVersionColor;

typedef enum {
    FuriHalVersionRegionUnknown = 0x00,
    FuriHalVersionRegionEuRu = 0x01,
    FuriHalVersionRegionUsCaAu = 0x02,
    FuriHalVersionRegionJp = 0x03,
    FuriHalVersionRegionWorld = 0x04,
} FuriHalVersionRegion;

typedef enum {
    FuriHalVersionDisplayUnknown = 0x00,
    FuriHalVersionDisplayErc = 0x01,
    FuriHalVersionDisplayMgg = 0x02,
} FuriHalVersionDisplay;

void furi_hal_version_init(void);
bool furi_hal_version_do_i_belong_here(void);

const char* furi_hal_version_get_model_name(void);
const char* furi_hal_version_get_model_code(void);
const char* furi_hal_version_get_fcc_id(void);
const char* furi_hal_version_get_ic_id(void);
const char* furi_hal_version_get_mic_id(void);
const char* furi_hal_version_get_srrc_id(void);
const char* furi_hal_version_get_ncc_id(void);

FuriHalVersionOtpVersion furi_hal_version_get_otp_version(void);
uint8_t furi_hal_version_get_hw_version(void);
uint8_t furi_hal_version_get_hw_target(void);
uint8_t furi_hal_version_get_hw_body(void);
FuriHalVersionColor furi_hal_version_get_hw_color(void);
uint8_t furi_hal_version_get_hw_connect(void);
FuriHalVersionRegion furi_hal_version_get_hw_region(void);
const char* furi_hal_version_get_hw_region_name(void);
FuriHalVersionRegion furi_hal_version_get_hw_region_otp(void);
const char* furi_hal_version_get_hw_region_name_otp(void);
FuriHalVersionDisplay furi_hal_version_get_hw_display(void);
uint32_t furi_hal_version_get_hw_timestamp(void);

const char* furi_hal_version_get_name_ptr(void);
const char* furi_hal_version_get_device_name_ptr(void);
const char* furi_hal_version_get_ble_local_device_name_ptr(void);
void furi_hal_version_set_name(const char* name);
const uint8_t* furi_hal_version_get_ble_mac(void);

const struct Version* furi_hal_version_get_firmware_version(void);
size_t furi_hal_version_uid_size(void);
const uint8_t* furi_hal_version_uid(void);
const uint8_t* furi_hal_version_uid_default(void);

#ifdef __cplusplus
}
#endif
