#include "helper.h"
#include "libnrf24/nrf24.h"
#include "settings.h"

char EMPTY_HEX[] = "0000000000";

void hexlify(uint8_t* in, uint8_t size, char* out) {
    const char hex_digits[] = "0123456789ABCDEF";

    for(int i = 0; i < size; i++) {
        out[i * 2] = hex_digits[(in[i] >> 4) & 0x0F];
        out[i * 2 + 1] = hex_digits[in[i] & 0x0F];
    }
    out[size * 2] = '\0';
}

void unhexlify(const char* in, uint8_t size, uint8_t* out) {
    size_t in_len = strlen(in);

    for(size_t i = 0; i < size; i++) {
        if((i * 2 + 1) >= in_len) {
            return;
        }
        char high_nibble = toupper(in[i * 2]);
        char low_nibble = toupper(in[i * 2 + 1]);

        out[i] = ((high_nibble >= 'A') ? (high_nibble - 'A' + 10) : (high_nibble - '0')) << 4;
        out[i] |= (low_nibble >= 'A') ? (low_nibble - 'A' + 10) : (low_nibble - '0');
    }
}

// Function to convert the setting value to a string based on the setting type
const char* setting_value_to_string(Setting* setting, char* buffer, size_t buffer_size) {
    switch(setting->type) {
    case SETTING_TYPE_UINT8:
        snprintf(buffer, buffer_size, "%u", setting->value.u8);
        break;
    case SETTING_TYPE_UINT16:
        snprintf(buffer, buffer_size, "%u", setting->value.u16);
        break;
    case SETTING_TYPE_UINT32:
        snprintf(buffer, buffer_size, "%lu", setting->value.u32);
        break;
    case SETTING_TYPE_BOOL:
        snprintf(buffer, buffer_size, "%s", setting->value.b ? "ON" : "OFF");
        break;
    case SETTING_TYPE_DATA_RATE:
        switch(setting->value.d_r) {
        case DATA_RATE_1MBPS:
            snprintf(buffer, buffer_size, "1 Mbps");
            break;
        case DATA_RATE_2MBPS:
            snprintf(buffer, buffer_size, "2 Mbps");
            break;
        case DATA_RATE_250KBPS:
            snprintf(buffer, buffer_size, "250 kbps");
            break;
        default:
            snprintf(buffer, buffer_size, "Unknown");
            break;
        }
        break;
    case SETTING_TYPE_TX_POWER:
        switch(setting->value.t_p) {
        case TX_POWER_M18DBM:
            snprintf(buffer, buffer_size, "-18 dBm");
            break;
        case TX_POWER_M12DBM:
            snprintf(buffer, buffer_size, "-12 dBm");
            break;
        case TX_POWER_M6DBM:
            snprintf(buffer, buffer_size, "-6 dBm");
            break;
        case TX_POWER_0DBM:
            snprintf(buffer, buffer_size, "0 dBm");
            break;
        default:
            snprintf(buffer, buffer_size, "Unknown");
            break;
        }
        break;
    case SETTING_TYPE_ADDR_WIDTH:
        switch(setting->value.a_w) {
        case ADDR_WIDTH_2_BYTES:
            snprintf(buffer, buffer_size, "2 Bytes");
            break;
        case ADDR_WIDTH_3_BYTES:
            snprintf(buffer, buffer_size, "3 Bytes");
            break;
        case ADDR_WIDTH_4_BYTES:
            snprintf(buffer, buffer_size, "4 Bytes");
            break;
        case ADDR_WIDTH_5_BYTES:
            snprintf(buffer, buffer_size, "5 Bytes");
            break;
        default:
            snprintf(buffer, buffer_size, "Unknown");
            break;
        }
        break;
    case SETTING_TYPE_ADDR:
        hexlify(setting->value.addr, BYTE_MAC_LEN, buffer);
        break;
    case SETTING_TYPE_ADDR_1BYTE:
        hexlify(setting->value.addr, BYTE_MAC_LEN_1BYTE, buffer);
        break;
    case SETTING_TYPE_CRC_LENGHT:
        switch(setting->value.crc) {
        case CRC_DISABLED:
            snprintf(buffer, buffer_size, "OFF");
            break;
        case CRC_1_BYTE:
            snprintf(buffer, buffer_size, "1 Byte");
            break;
        case CRC_2_BYTES:
            snprintf(buffer, buffer_size, "2 Bytes");
            break;
        default:
            snprintf(buffer, buffer_size, "Unknown");
            break;
        }
        break;
    case SETTING_TYPE_PAYLOAD_SIZE:
        if(setting->value.u8 < MIN_PAYLOAD_SIZE) {
            snprintf(buffer, buffer_size, "PIPE OFF");
        } else if(setting->value.u8 > MAX_PAYLOAD_SIZE) {
            snprintf(buffer, buffer_size, "DYN");
        } else {
            snprintf(buffer, buffer_size, "%u", setting->value.u8);
        }
        break;
    case SETTING_TYPE_PIPE_NUM:
        if(setting->value.i8 < MIN_PIPE) {
            snprintf(buffer, buffer_size, "OFF");
        } else {
            snprintf(buffer, buffer_size, "%d", setting->value.i8);
        }
        break;
    }
    return buffer;
}

// Function to return the value of the parameter based on the type
int32_t get_setting_value(Setting* setting) {
    switch(setting->type) {
    case SETTING_TYPE_UINT8:
        return setting->value.u8;
    case SETTING_TYPE_UINT16:
        return setting->value.u16;
    case SETTING_TYPE_UINT32:
        return setting->value.u32;
    case SETTING_TYPE_BOOL:
        return setting->value.b ? 1 : 0; // Return 1 for true, 0 for false
    case SETTING_TYPE_DATA_RATE:
        return setting->value.d_r; // Assuming uint8_t for data rate
    case SETTING_TYPE_TX_POWER:
        return setting->value.t_p; // Assuming uint8_t for TX power
    case SETTING_TYPE_ADDR_WIDTH:
        return setting->value.a_w; // Assuming uint8_t for address width
    case SETTING_TYPE_CRC_LENGHT:
        return setting->value.crc;
    case SETTING_TYPE_PAYLOAD_SIZE:
        return setting->value.u8;
    case SETTING_TYPE_PIPE_NUM:
        return setting->value.i8;
    case SETTING_TYPE_ADDR:
    case SETTING_TYPE_ADDR_1BYTE:
        return 0;
    }
    return 0;
}

uint8_t get_setting_index(Setting* setting) {
    int32_t value = get_setting_value(setting);
    return (uint8_t)((value / setting->step) - (setting->min / setting->step));
}

// Function to modify the setting value based on the type
void set_setting_value(Setting* setting, int32_t new_value) {
    switch(setting->type) {
    case SETTING_TYPE_UINT8:
        setting->value.u8 = (uint8_t)new_value;
        break;
    case SETTING_TYPE_UINT16:
        setting->value.u16 = (uint16_t)new_value;
        break;
    case SETTING_TYPE_UINT32:
        setting->value.u32 = (uint32_t)new_value;
        break;
    case SETTING_TYPE_BOOL:
        setting->value.b = (new_value != 0); // Any non-zero value is considered true
        break;
    case SETTING_TYPE_DATA_RATE:
        setting->value.d_r = (nrf24_data_rate)new_value;
        break;
    case SETTING_TYPE_TX_POWER:
        setting->value.t_p = (nrf24_tx_power)new_value;
        break;
    case SETTING_TYPE_ADDR_WIDTH:
        setting->value.a_w = (nrf24_addr_width)new_value;
        break;
    case SETTING_TYPE_CRC_LENGHT:
        setting->value.crc = (nrf24_crc_lenght)new_value;
        break;
    case SETTING_TYPE_PAYLOAD_SIZE:
        setting->value.u8 = (nrf24_payload)new_value;
        break;
    case SETTING_TYPE_PIPE_NUM:
        setting->value.i8 = (int8_t)new_value;
        break;
    case SETTING_TYPE_ADDR:
    case SETTING_TYPE_ADDR_1BYTE:
        break;
    }
}

bool is_hex_address(const char* str) {
    // Check if the string is exactly 8 characters long
    if(strlen(str) != HEX_MAC_LEN) {
        return false;
    }

    // Check if all characters are valid hexadecimal digits
    for(int i = 0; i < HEX_MAC_LEN; i++) {
        if(!isxdigit((unsigned char)str[i])) {
            return false;
        }
    }

    // If all checks pass, it's a valid hexadecimal address
    return true;
}

bool is_hex_address_furi(const FuriString* furi_str) {
    // Get the length of the FuriString
    if(furi_string_size(furi_str) != HEX_MAC_LEN) {
        return false;
    }

    // Iterate over the string and check if each character is a valid hex digit
    for(size_t i = 0; i < HEX_MAC_LEN; i++) {
        char c = furi_string_get_char(furi_str, i); // Get character at index 'i'
        if(!isxdigit(c)) { // Check if it's a valid hex digit (0-9, a-f, A-F)
            return false;
        }
    }

    // If all checks pass, it's a valid 8-character hexadecimal address
    return true;
}

bool is_hex_line(const char* line) {
    for(size_t i = 0; i < strlen(line); i++) {
        // Ignore spaces and common separators
        if(line[i] == ' ' || line[i] == ':' || line[i] == '-') {
            continue;
        }
        // If character is not a hex digit, return false
        if(!isxdigit((uint8_t)line[i])) {
            return false;
        }
    }
    return true;
}

bool is_hex_line_furi(FuriString* line) {
    if((furi_string_size(line) & 1)) {
        return false;
    }
    for(size_t i = 0; i < furi_string_size(line); i++) {
        char c = furi_string_get_char(line, i);
        // Ignore spaces and common separators
        if(c == ' ' || c == ':' || c == '-') {
            continue;
        }
        // If character is not a hex digit, return false
        if(!isxdigit(c)) {
            return false;
        }
    }
    return true;
}

void buffer_to_ascii(uint8_t* buffer, uint8_t size, char* output) {
    for(uint8_t i = 0; i < size; i++) {
        if(isprint(buffer[i])) {
            output[i] = buffer[i];
        } else {
            output[i] = ' ';
        }
    }
    output[size] = '\0';
}

uint64_t bytes_to_int64(uint8_t* bytes, uint8_t size, bool bigendian) {
    uint64_t ret = 0;
    for(int i = 0; i < size; i++) {
        if(bigendian)
            ret |= bytes[i] << ((size - 1 - i) * 8);
        else
            ret |= bytes[i] << (i * 8);
    }

    return ret;
}

void int64_to_bytes(uint64_t val, uint8_t* out, bool bigendian) {
    for(int i = 0; i < 8; i++) {
        if(bigendian)
            out[i] = (val >> ((7 - i) * 8)) & 0xff;
        else
            out[i] = (val >> (i * 8)) & 0xff;
    }
}

uint32_t bytes_to_int32(uint8_t* bytes, bool bigendian) {
    uint32_t ret = 0;
    for(int i = 0; i < 4; i++) {
        if(bigendian)
            ret |= bytes[i] << ((3 - i) * 8);
        else
            ret |= bytes[i] << (i * 8);
    }

    return ret;
}

void int32_to_bytes(uint32_t val, uint8_t* out, bool bigendian) {
    for(int i = 0; i < 4; i++) {
        if(bigendian)
            out[i] = (val >> ((3 - i) * 8)) & 0xff;
        else
            out[i] = (val >> (i * 8)) & 0xff;
    }
}

uint64_t bytes_to_int16(uint8_t* bytes, bool bigendian) {
    uint16_t ret = 0;
    for(int i = 0; i < 2; i++) {
        if(bigendian)
            ret |= bytes[i] << ((1 - i) * 8);
        else
            ret |= bytes[i] << (i * 8);
    }

    return ret;
}

void int16_to_bytes(uint16_t val, uint8_t* out, bool bigendian) {
    for(int i = 0; i < 2; i++) {
        if(bigendian)
            out[i] = (val >> ((1 - i) * 8)) & 0xff;
        else
            out[i] = (val >> (i * 8)) & 0xff;
    }
}
