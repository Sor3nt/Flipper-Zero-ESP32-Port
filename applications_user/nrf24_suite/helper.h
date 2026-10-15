#pragma once

//#include "settings.h"
#include <furi.h>

#define HEX_MAC_LEN            10
#define HEX_MAC_LEN_1BYTE      2
#define BYTE_MAC_LEN           5
#define BYTE_MAC_LEN_1BYTE     1
#define MAX_SETTINGS(setting)  (((setting)->max - (setting)->min) / (setting)->step + 1)
#define BYTE_TO_BINARY_PATTERN "%c%c%c%c%c%c%c%c"
#define BYTE_TO_BINARY(byte)                                                             \
    (byte & 0x80 ? '1' : '0'), (byte & 0x40 ? '1' : '0'), (byte & 0x20 ? '1' : '0'),     \
        (byte & 0x10 ? '1' : '0'), (byte & 0x08 ? '1' : '0'), (byte & 0x04 ? '1' : '0'), \
        (byte & 0x02 ? '1' : '0'), (byte & 0x01 ? '1' : '0')

extern char EMPTY_HEX[HEX_MAC_LEN + 1];
extern const uint8_t u8g2_font_5x7_tr[841];

typedef struct Setting Setting;

void hexlify(uint8_t* in, uint8_t size, char* out);
void unhexlify(const char* in, uint8_t size, uint8_t* out);
const char* setting_value_to_string(Setting* setting, char* buffer, size_t buffer_size);
int32_t get_setting_value(Setting* setting);
uint8_t get_setting_index(Setting* setting);
void set_setting_value(Setting* setting, int32_t new_value);
bool is_hex_address(const char* str);
bool is_hex_address_furi(const FuriString* furi_str);
bool is_hex_line(const char* line);
bool is_hex_line_furi(FuriString* line);
void buffer_to_ascii(uint8_t* buffer, uint8_t size, char* output);
uint64_t bytes_to_int64(uint8_t* bytes, uint8_t size, bool bigendian);
void int64_to_bytes(uint64_t val, uint8_t* out, bool bigendian);
uint32_t bytes_to_int32(uint8_t* bytes, bool bigendian);
void int32_to_bytes(uint32_t val, uint8_t* out, bool bigendian);
uint64_t bytes_to_int16(uint8_t* bytes, bool bigendian);
void int16_to_bytes(uint16_t val, uint8_t* out, bool bigendian);
