#pragma once
#include <stdint.h>
#include <stdbool.h>
bool esp_gpio_is_reserved(uint64_t mask);
uint64_t esp_gpio_revoke(uint64_t mask);
