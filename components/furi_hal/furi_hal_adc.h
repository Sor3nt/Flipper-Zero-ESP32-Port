/**
 * @file furi_hal_adc.h
 * @brief ADC HAL stub for ESP32 port.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FuriHalAdcHandle FuriHalAdcHandle;

typedef enum {
    FuriHalAdcChannel0,
    FuriHalAdcChannel1,
    FuriHalAdcChannel2,
    FuriHalAdcChannel3,
    FuriHalAdcChannel4,
    FuriHalAdcChannel5,
    FuriHalAdcChannel6,
    FuriHalAdcChannel7,
    FuriHalAdcChannel8,
    FuriHalAdcChannel9,
    FuriHalAdcChannel10,
    FuriHalAdcChannel11,
    FuriHalAdcChannel12,
    FuriHalAdcChannel13,
    FuriHalAdcChannel14,
    FuriHalAdcChannel15,
    FuriHalAdcChannel16,
    FuriHalAdcChannel17,
    FuriHalAdcChannel18,
    FuriHalAdcChannelNone = 0xFF,
    FuriHalAdcChannelVREFINT = FuriHalAdcChannel0,
    FuriHalAdcChannelTEMPSENSOR = FuriHalAdcChannel17,
    FuriHalAdcChannelVBAT = FuriHalAdcChannel18,
} FuriHalAdcChannel;

#ifdef __cplusplus
}
#endif
