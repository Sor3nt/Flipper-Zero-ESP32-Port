#pragma once

#include <stdint.h>

struct IrCode {
    uint8_t timer_val;
    uint8_t numpairs;
    uint8_t bitcompression;
    uint16_t const *times;
    uint8_t const *codes;
};

#define freq_to_timerval(x) (x / 1000)

const uint16_t code_na000Times[] = {60, 60, 60, 2700, 120, 60, 240, 60};
const uint8_t code_na000Codes[] = {0xE2, 0x20, 0x80, 0x78, 0x88, 0x20, 0x10};
const struct IrCode code_na000Code = {freq_to_timerval(38400), 26, 2, code_na000Times, code_na000Codes};

const uint16_t code_na001Times[] = {50, 100, 50, 200, 50, 800, 400, 400};
const uint8_t code_na001Codes[] = {0xD5, 0x41, 0x11, 0x00, 0x14, 0x44, 0x6D, 0x54, 0x11, 0x10, 0x01, 0x44, 0x45};
const struct IrCode code_na001Code = {freq_to_timerval(57143), 52, 2, code_na001Times, code_na001Codes};

const uint16_t code_eu005Times[] = {24, 190, 25, 80, 25, 190, 25, 4199, 25, 4799};
const uint8_t code_eu005Codes[] = {0x04, 0x92, 0x52, 0x28, 0x92, 0x8C, 0x44, 0x92, 0x89, 0x45, 0x24, 0x53, 0x44, 0x92, 0x52, 0x28, 0x92, 0x8C, 0x44, 0x92, 0x89, 0x45, 0x24, 0x51};
const struct IrCode code_eu005Code = {freq_to_timerval(38610), 64, 3, code_eu005Times, code_eu005Codes};

const uint16_t code_eu006Times[] = {53, 63, 53, 172, 53, 4472, 54, 0, 455, 468};
const uint8_t code_eu006Codes[] = {0x84, 0x90, 0x00, 0x04, 0x90, 0x00, 0x00, 0x80, 0x00, 0x04, 0x12, 0x49, 0x2A, 0x12, 0x40, 0x00, 0x12, 0x40, 0x00, 0x02, 0x00, 0x00, 0x10, 0x49, 0x24, 0xB0};
const struct IrCode code_eu006Code = {freq_to_timerval(38462), 68, 3, code_eu006Times, code_eu006Codes};

const uint16_t code_eu140Times[] = {448, 448, 56, 168, 56, 56, 56, 4526};
const uint8_t code_eu140Codes[] = {0x15, 0xAA, 0x95, 0xAA, 0xAA, 0x5A, 0x55, 0xA5, 0xB1, 0x5A, 0xA9, 0x5A, 0xAA, 0xA5, 0xA5, 0x5A, 0x5B};
const struct IrCode code_eu140Code = {freq_to_timerval(38462), 68, 2, code_eu140Times, code_eu140Codes};

#define NA_CODES_COUNT 2
#define EU_CODES_COUNT 3

const IrCode *const NApowerCodes[] = {
    &code_na000Code,
    &code_na001Code,
};

const IrCode *const EUpowerCodes[] = {
    &code_eu005Code,
    &code_eu006Code,
    &code_eu140Code,
};
