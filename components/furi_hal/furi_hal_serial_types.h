/**
 * @file furi_hal_serial_types.h
 * @brief Serial types stub for ESP32 port.
 */
#pragma once

#include <furi.h>

typedef enum {
    FuriHalSerialIdUsart,
    FuriHalSerialIdLpuart,

    FuriHalSerialIdMax,
} FuriHalSerialId;

typedef enum {
    FuriHalSerialDirectionTx,
    FuriHalSerialDirectionRx,

    FuriHalSerialDirectionMax,
} FuriHalSerialDirection;

typedef enum {
    FuriHalSerialDataBits6,
    FuriHalSerialDataBits7,
    FuriHalSerialDataBits8,
    FuriHalSerialDataBits9,

    FuriHalSerialDataBitsMax,
} FuriHalSerialDataBits;

typedef enum {
    FuriHalSerialParityNone,
    FuriHalSerialParityEven,
    FuriHalSerialParityOdd,

    FuriHalSerialParityMax,
} FuriHalSerialParity;

typedef enum {
    FuriHalSerialStopBits0_5,
    FuriHalSerialStopBits1,
    FuriHalSerialStopBits1_5,
    FuriHalSerialStopBits2,

    FuriHalSerialStopBits2Max,
} FuriHalSerialStopBits;

typedef struct FuriHalSerialHandle FuriHalSerialHandle;

typedef enum {
    FuriHalSerialRxEventData = (1 << 0),
    FuriHalSerialRxEventIdle = (1 << 1),
    FuriHalSerialRxEventFrameError = (1 << 2),
    FuriHalSerialRxEventNoiseError = (1 << 3),
    FuriHalSerialRxEventOverrunError = (1 << 4),
} FuriHalSerialRxEvent;
