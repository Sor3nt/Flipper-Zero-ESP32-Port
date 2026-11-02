#pragma once

typedef enum {
    GpioStartEventManualControl = 0,
    GpioStartEventUsbUart,
    GpioStartEventVbusSettings,

    GpioCustomEventErrorBack,

    GpioUsbUartEventConfig,
    GpioUsbUartEventConfigSet,
} GpioCustomEvent;
