#pragma once
#include <stddef.h>
#include <stdint.h>
size_t wardriver_csv_quote(char* output,size_t size,const char* input);
const char* wardriver_security(uint8_t auth);
