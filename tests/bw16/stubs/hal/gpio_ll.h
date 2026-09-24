#pragma once
#include <soc/gpio_struct.h>
void gpio_ll_set_level(TestGpio* gpio,unsigned pin,unsigned level);
void gpio_ll_iomux_func_sel(uintptr_t pin,unsigned function);
void gpio_ll_output_enable(TestGpio* gpio,unsigned pin);
