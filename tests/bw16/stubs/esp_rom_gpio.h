#pragma once
#include <stdbool.h>
void esp_rom_gpio_connect_out_signal(unsigned pin,unsigned signal,bool invert,bool enable_invert);
