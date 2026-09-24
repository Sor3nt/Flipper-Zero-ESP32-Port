#pragma once
#include <stdint.h>
typedef struct { uint32_t val; } TestRegister;
typedef struct {
    TestRegister pin[49],func_out_sel_cfg[49],func_in_sel_cfg[256];
    TestRegister out1,enable1,enable1_w1tc,out1_w1ts,out1_w1tc,enable1_w1ts;
} TestGpio;
extern TestGpio GPIO;
