#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
struct MockSerial {
    unsigned baud;
    unsigned used,position;
    char input[256];
    void begin(unsigned value) { baud=value; }
    int available() { return (int)(used-position); }
    int read() { return position<used ? (unsigned char)input[position++] : -1; }
    void println(const char* text);
};
extern MockSerial Serial,Serial1;
uint32_t millis();
uint32_t micros();
void delay(unsigned value);

void* pvPortMalloc(size_t size);

size_t xPortGetFreeHeapSize();
