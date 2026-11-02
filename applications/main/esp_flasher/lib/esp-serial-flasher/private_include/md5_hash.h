/*
 * MD5 hash implementation and interface functions
 * Copyright (c) 2003-2005, Jouni Malinen <j@w1.fi>
 *
 * This software may be distributed under the terms of the BSD license.
 * See README for more details.
 */

#pragma once

#include <stdint.h>

/* ESP32 port: the ESP32-S3 ROM linker script already defines MD5Init/MD5Update/MD5Final
 * (esp32s3.rom.ld) and ESP-IDF routes esp_rom_md5_* to them. Rename this copy so the
 * flasher never interposes on (or is replaced by) the ROM implementation. */
#define MD5Init   esp_sf_md5_init
#define MD5Update esp_sf_md5_update
#define MD5Final  esp_sf_md5_final

#ifdef __cplusplus
extern "C" {
#endif
struct MD5Context {
    uint32_t buf[4];
    uint32_t bits[2];
    uint8_t in[64];
};

void MD5Init(struct MD5Context *context);
void MD5Update(struct MD5Context *context, unsigned char const *buf, unsigned len);
void MD5Final(unsigned char digest[16], struct MD5Context *context);

#ifdef __cplusplus
}
#endif
