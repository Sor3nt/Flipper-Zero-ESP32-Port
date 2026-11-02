#pragma once

#include <furi_hal_pwm.h>

// ESP32 port: the generator drives the board's external GPIO header pins (the
// STM32 pins of upstream, PA7/PA4/13(TX), do not exist here and gpio_ext_pa*
// are the CC1101 SPI pins). Names come from the board header the FAP is built
// for, so they always match the HAL's furi_hal_pwm table.
#include BOARD_INCLUDE

#if !defined(BOARD_PIN_EXT_GPIO_1) || !defined(BOARD_PIN_EXT_GPIO_2) || \
    !defined(BOARD_PIN_EXT_GPIO_3) || !defined(BOARD_PIN_EXT_GPIO_4) || \
    !defined(BOARD_PIN_EXT_GPIO_5)
#error "Signal Generator needs a board with external GPIO header pins (paradoxt)"
#endif

#define SIGNAL_GEN_STR_(x) #x
#define SIGNAL_GEN_STR(x)  SIGNAL_GEN_STR_(x)

#define SIGNAL_GEN_PIN_COUNT 5

static const FuriHalPwmOutputId signal_gen_pwm_ids[SIGNAL_GEN_PIN_COUNT] = {
    FuriHalPwmOutputIdExtGpio1,
    FuriHalPwmOutputIdExtGpio2,
    FuriHalPwmOutputIdExtGpio3,
    FuriHalPwmOutputIdExtGpio4,
    FuriHalPwmOutputIdExtGpio5,
};

static const char* const signal_gen_pin_names[SIGNAL_GEN_PIN_COUNT] = {
    "GPIO" SIGNAL_GEN_STR(BOARD_PIN_EXT_GPIO_1),
    "GPIO" SIGNAL_GEN_STR(BOARD_PIN_EXT_GPIO_2),
    "GPIO" SIGNAL_GEN_STR(BOARD_PIN_EXT_GPIO_3),
    "GPIO" SIGNAL_GEN_STR(BOARD_PIN_EXT_GPIO_4),
    "GPIO" SIGNAL_GEN_STR(BOARD_PIN_EXT_GPIO_5),
};
