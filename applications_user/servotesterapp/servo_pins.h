#pragma once

#include <furi_hal_pwm.h>

// ESP32 port: the servo signal goes out on the board's external GPIO header. Upstream's
// pin A7 (STM32 TIM1) does not exist here, and gpio_ext_pa* are the CC1101 SPI pins on
// this port. Pin numbers come from the board header the FAP is built for, so they always
// match the furi_hal_pwm table.
#include BOARD_INCLUDE

#if !defined(BOARD_PIN_EXT_GPIO_1) || !defined(BOARD_PIN_EXT_GPIO_2) || \
    !defined(BOARD_PIN_EXT_GPIO_3) || !defined(BOARD_PIN_EXT_GPIO_4) || \
    !defined(BOARD_PIN_EXT_GPIO_5)
#error "Servo Tester needs a board with external GPIO header pins (paradoxt)"
#endif

#define SERVO_PIN_STR_(x) #x
#define SERVO_PIN_STR(x)  SERVO_PIN_STR_(x)

#define SERVO_PIN_COUNT 5

static const FuriHalPwmOutputId servo_pwm_ids[SERVO_PIN_COUNT] = {
    FuriHalPwmOutputIdExtGpio1,
    FuriHalPwmOutputIdExtGpio2,
    FuriHalPwmOutputIdExtGpio3,
    FuriHalPwmOutputIdExtGpio4,
    FuriHalPwmOutputIdExtGpio5,
};

static const char* const servo_pin_names[SERVO_PIN_COUNT] = {
    "GPIO" SERVO_PIN_STR(BOARD_PIN_EXT_GPIO_1),
    "GPIO" SERVO_PIN_STR(BOARD_PIN_EXT_GPIO_2),
    "GPIO" SERVO_PIN_STR(BOARD_PIN_EXT_GPIO_3),
    "GPIO" SERVO_PIN_STR(BOARD_PIN_EXT_GPIO_4),
    "GPIO" SERVO_PIN_STR(BOARD_PIN_EXT_GPIO_5),
};
