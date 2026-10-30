#include "furi_hal_crypto.h"
#include <furi.h>
#include <string.h>
#include <stdlib.h>
#include <esp_mac.h>   

//  AES-128 KEY(16 byte = 128 bit)
static const uint8_t HARDCODED_KEY[16] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F
};

static const uint8_t sbox[256] = {
    0x63, 0x7C, 0x77, 0x7B, 0xF2, 0x6B, 0x6F, 0xC5, 0x30, 0x01, 0x67, 0x2B, 0xFE, 0xD7, 0xAB, 0x76,
    0xCA, 0x82, 0xC9, 0x7D, 0xFA, 0x59, 0x47, 0xF0, 0xAD, 0xD4, 0xA2, 0xAF, 0x9C, 0xA4, 0x72, 0xC0,
    0xB7, 0xFD, 0x93, 0x26, 0x36, 0x3F, 0xF7, 0xCC, 0x34, 0xA5, 0xE5, 0xF1, 0x71, 0xD8, 0x31, 0x15,
    0x04, 0xC7, 0x23, 0xC3, 0x18, 0x96, 0x05, 0x9A, 0x07, 0x12, 0x80, 0xE2, 0xEB, 0x27, 0xB2, 0x75,
    0x09, 0x83, 0x2C, 0x1A, 0x1B, 0x6E, 0x5A, 0xA0, 0x52, 0x3B, 0xD6, 0xB3, 0x29, 0xE3, 0x2F, 0x84,
    0x53, 0xD1, 0x00, 0xED, 0x20, 0xFC, 0xB1, 0x5B, 0x6A, 0xCB, 0xBE, 0x39, 0x4A, 0x4C, 0x58, 0xCF,
    0xD0, 0xEF, 0xAA, 0xFB, 0x43, 0x4D, 0x33, 0x85, 0x45, 0xF9, 0x02, 0x7F, 0x50, 0x3C, 0x9F, 0xA8,
    0x51, 0xA3, 0x40, 0x8F, 0x92, 0x9D, 0x38, 0xF5, 0xBC, 0xB6, 0xDA, 0x21, 0x10, 0xFF, 0xF3, 0xD2,
    0xCD, 0x0C, 0x13, 0xEC, 0x5F, 0x97, 0x44, 0x17, 0xC4, 0xA7, 0x7E, 0x3D, 0x64, 0x5D, 0x19, 0x73,
    0x60, 0x81, 0x4F, 0xDC, 0x22, 0x2A, 0x90, 0x88, 0x46, 0xEE, 0xB8, 0x14, 0xDE, 0x5E, 0x0B, 0xDB,
    0xE0, 0x32, 0x3A, 0x0A, 0x49, 0x06, 0x24, 0x5C, 0xC2, 0xD3, 0xAC, 0x62, 0x91, 0x95, 0xE4, 0x79,
    0xE7, 0xC8, 0x37, 0x6D, 0x8D, 0xD5, 0x4E, 0xA9, 0x6C, 0x56, 0xF4, 0xEA, 0x65, 0x7A, 0xAE, 0x08,
    0xBA, 0x78, 0x25, 0x2E, 0x1C, 0xA6, 0xB4, 0xC6, 0xE8, 0xDD, 0x74, 0x1F, 0x4B, 0xBD, 0x8B, 0x8A,
    0x70, 0x3E, 0xB5, 0x66, 0x48, 0x03, 0xF6, 0x0E, 0x61, 0x35, 0x57, 0xB9, 0x86, 0xC1, 0x1D, 0x9E,
    0xE1, 0xF8, 0x98, 0x11, 0x69, 0xD9, 0x8E, 0x94, 0x9B, 0x1E, 0x87, 0xE9, 0xCE, 0x55, 0x28, 0xDF,
    0x8C, 0xA1, 0x89, 0x0D, 0xBF, 0xE6, 0x42, 0x68, 0x41, 0x99, 0x2D, 0x0F, 0xB0, 0x54, 0xBB, 0x16
};

static const uint8_t inv_sbox[256] = {
    0x52, 0x09, 0x6A, 0xD5, 0x30, 0x36, 0xA5, 0x38, 0xBF, 0x40, 0xA3, 0x9E, 0x81, 0xF3, 0xD7, 0xFB,
    0x7C, 0xE3, 0x39, 0x82, 0x9B, 0x2F, 0xFF, 0x87, 0x34, 0x8E, 0x43, 0x44, 0xC4, 0xDE, 0xE9, 0xCB,
    0x54, 0x7B, 0x94, 0x32, 0xA6, 0xC2, 0x23, 0x3D, 0xEE, 0x4C, 0x95, 0x0B, 0x42, 0xFA, 0xC3, 0x4E,
    0x08, 0x2E, 0xA1, 0x66, 0x28, 0xD9, 0x24, 0xB2, 0x76, 0x5B, 0xA2, 0x49, 0x6D, 0x8B, 0xD1, 0x25,
    0x72, 0xF8, 0xF6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xD4, 0xA4, 0x5C, 0xCC, 0x5D, 0x65, 0xB6, 0x92,
    0x6C, 0x70, 0x48, 0x50, 0xFD, 0xED, 0xB9, 0xDA, 0x5E, 0x15, 0x46, 0x57, 0xA7, 0x8D, 0x9D, 0x84,
    0x90, 0xD8, 0xAB, 0x00, 0x8C, 0xBC, 0xD3, 0x0A, 0xF7, 0xE4, 0x58, 0x05, 0xB8, 0xB3, 0x45, 0x06,
    0xD0, 0x2C, 0x1E, 0x8F, 0xCA, 0x3F, 0x0F, 0x02, 0xC1, 0xAF, 0xBD, 0x03, 0x01, 0x13, 0x8A, 0x6B,
    0x3A, 0x91, 0x11, 0x41, 0x4F, 0x67, 0xDC, 0xEA, 0x97, 0xF2, 0xCF, 0xCE, 0xF0, 0xB4, 0xE6, 0x73,
    0x96, 0xAC, 0x74, 0x22, 0xE7, 0xAD, 0x35, 0x85, 0xE2, 0xF9, 0x37, 0xE8, 0x1C, 0x75, 0xDF, 0x6E,
    0x47, 0xF1, 0x1A, 0x71, 0x1D, 0x29, 0xC5, 0x89, 0x6F, 0xB7, 0x62, 0x0E, 0xAA, 0x18, 0xBE, 0x1B,
    0xFC, 0x56, 0x3E, 0x4B, 0xC6, 0xD2, 0x79, 0x20, 0x9A, 0xDB, 0xC0, 0xFE, 0x78, 0xCD, 0x5A, 0xF4,
    0x1F, 0xDD, 0xA8, 0x33, 0x88, 0x07, 0xC7, 0x31, 0xB1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xEC, 0x5F,
    0x60, 0x51, 0x7F, 0xA9, 0x19, 0xB5, 0x4A, 0x0D, 0x2D, 0xE5, 0x7A, 0x9F, 0x93, 0xC9, 0x9C, 0xEF,
    0xA0, 0xE0, 0x3B, 0x4D, 0xAE, 0x2A, 0xF5, 0xB0, 0xC8, 0xEB, 0xBB, 0x3C, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2B, 0x04, 0x7E, 0xBA, 0x77, 0xD6, 0x26, 0xE1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0C, 0x7D
};

static const uint8_t Rcon[11] = {
    0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36
};

static uint8_t current_iv[16];
static uint8_t roundKeys[11][16];
static bool key_scheduled = false;

static void SubBytes(uint8_t *state) {
    for(int i = 0; i < 16; i++) state[i] = sbox[state[i]];
}

static void InvSubBytes(uint8_t *state) {
    for(int i = 0; i < 16; i++) state[i] = inv_sbox[state[i]];
}

static void ShiftRows(uint8_t *state) {
    uint8_t temp[16];
    temp[0]  = state[0];  temp[1]  = state[5];  temp[2]  = state[10]; temp[3]  = state[15];
    temp[4]  = state[4];  temp[5]  = state[9];  temp[6]  = state[14]; temp[7]  = state[3];
    temp[8]  = state[8];  temp[9]  = state[13]; temp[10] = state[2];  temp[11] = state[7];
    temp[12] = state[12]; temp[13] = state[1];  temp[14] = state[6];  temp[15] = state[11];
    memcpy(state, temp, 16);
}

static void InvShiftRows(uint8_t *state) {
    uint8_t temp[16];
    temp[0]  = state[0];  temp[1]  = state[13]; temp[2]  = state[10]; temp[3]  = state[7];
    temp[4]  = state[4];  temp[5]  = state[1];  temp[6]  = state[14]; temp[7]  = state[11];
    temp[8]  = state[8];  temp[9]  = state[5];  temp[10] = state[2];  temp[11] = state[15];
    temp[12] = state[12]; temp[13] = state[9];  temp[14] = state[6];  temp[15] = state[3];
    memcpy(state, temp, 16);
}

static uint8_t xtime(uint8_t x) {
    return (x << 1) ^ ((x >> 7) & 1 ? 0x1B : 0x00);
}

static uint8_t mul(uint8_t a, uint8_t b) {
    uint8_t result = 0;
    for(int i = 0; i < 8; i++) {
        if(b & 1) result ^= a;
        uint8_t hi = a & 0x80;
        a <<= 1;
        if(hi) a ^= 0x1B;
        b >>= 1;
    }
    return result;
}

static void MixColumns(uint8_t *state) {
    for(int c = 0; c < 4; c++) {
        uint8_t a[4], b[4];
        for(int i = 0; i < 4; i++) {
            a[i] = state[c * 4 + i];
            b[i] = xtime(a[i]);
        }
        state[c * 4 + 0] = b[0] ^ a[3] ^ a[2] ^ b[1] ^ a[1];
        state[c * 4 + 1] = b[1] ^ a[0] ^ a[3] ^ b[2] ^ a[2];
        state[c * 4 + 2] = b[2] ^ a[1] ^ a[0] ^ b[3] ^ a[3];
        state[c * 4 + 3] = b[3] ^ a[2] ^ a[1] ^ b[0] ^ a[0];
    }
}

static void InvMixColumns(uint8_t *state) {
    for(int c = 0; c < 4; c++) {
        uint8_t a[4];
        for(int i = 0; i < 4; i++) a[i] = state[c * 4 + i];
        state[c * 4 + 0] = mul(a[0], 0x0E) ^ mul(a[1], 0x0B) ^ mul(a[2], 0x0D) ^ mul(a[3], 0x09);
        state[c * 4 + 1] = mul(a[0], 0x09) ^ mul(a[1], 0x0E) ^ mul(a[2], 0x0B) ^ mul(a[3], 0x0D);
        state[c * 4 + 2] = mul(a[0], 0x0D) ^ mul(a[1], 0x09) ^ mul(a[2], 0x0E) ^ mul(a[3], 0x0B);
        state[c * 4 + 3] = mul(a[0], 0x0B) ^ mul(a[1], 0x0D) ^ mul(a[2], 0x09) ^ mul(a[3], 0x0E);
    }
}

static void AddRoundKey(uint8_t *state, const uint8_t *roundKey) {
    for(int i = 0; i < 16; i++) state[i] ^= roundKey[i];
}

static void KeyExpansion(const uint8_t *key, uint8_t roundKeys[11][16]) {
    uint8_t temp[4];
    memcpy(roundKeys[0], key, 16);
    for(int i = 1; i <= 10; i++) {
        // RotWord: [12,13,14,15] -> [13,14,15,12]  (era mancante!)
        temp[0] = roundKeys[i-1][13];
        temp[1] = roundKeys[i-1][14];
        temp[2] = roundKeys[i-1][15];
        temp[3] = roundKeys[i-1][12];
        temp[0] = sbox[temp[0]];
        temp[1] = sbox[temp[1]];
        temp[2] = sbox[temp[2]];
        temp[3] = sbox[temp[3]];
        temp[0] ^= Rcon[i];
        roundKeys[i][0]  = roundKeys[i-1][0]  ^ temp[0];
        roundKeys[i][1]  = roundKeys[i-1][1]  ^ temp[1];
        roundKeys[i][2]  = roundKeys[i-1][2]  ^ temp[2];
        roundKeys[i][3]  = roundKeys[i-1][3]  ^ temp[3];
        roundKeys[i][4]  = roundKeys[i-1][4]  ^ roundKeys[i][0];
        roundKeys[i][5]  = roundKeys[i-1][5]  ^ roundKeys[i][1];
        roundKeys[i][6]  = roundKeys[i-1][6]  ^ roundKeys[i][2];
        roundKeys[i][7]  = roundKeys[i-1][7]  ^ roundKeys[i][3];
        roundKeys[i][8]  = roundKeys[i-1][8]  ^ roundKeys[i][4];
        roundKeys[i][9]  = roundKeys[i-1][9]  ^ roundKeys[i][5];
        roundKeys[i][10] = roundKeys[i-1][10] ^ roundKeys[i][6];
        roundKeys[i][11] = roundKeys[i-1][11] ^ roundKeys[i][7];
        roundKeys[i][12] = roundKeys[i-1][12] ^ roundKeys[i][8];
        roundKeys[i][13] = roundKeys[i-1][13] ^ roundKeys[i][9];
        roundKeys[i][14] = roundKeys[i-1][14] ^ roundKeys[i][10];
        roundKeys[i][15] = roundKeys[i-1][15] ^ roundKeys[i][11];
    }
}

static void AES_EncryptBlock(uint8_t *block, const uint8_t roundKeys[11][16]) {
    AddRoundKey(block, roundKeys[0]);
    for(int round = 1; round <= 9; round++) {
        SubBytes(block);
        ShiftRows(block);
        MixColumns(block);
        AddRoundKey(block, roundKeys[round]);
    }
    SubBytes(block);
    ShiftRows(block);
    AddRoundKey(block, roundKeys[10]);
}

static void AES_DecryptBlock(uint8_t *block, const uint8_t roundKeys[11][16]) {
    AddRoundKey(block, roundKeys[10]);
    for(int round = 9; round >= 1; round--) {
        InvShiftRows(block);
        InvSubBytes(block);
        AddRoundKey(block, roundKeys[round]);
        InvMixColumns(block);
    }
    InvShiftRows(block);
    InvSubBytes(block);
    AddRoundKey(block, roundKeys[0]);
}

static void xor_blocks(uint8_t *a, const uint8_t *b) {
    for(int i = 0; i < 16; i++) a[i] ^= b[i];
}


// PKCS#7 PADDING
size_t furi_hal_crypto_padded_size(size_t data_size) {
    if(data_size == 0) return 16;
    size_t remainder = data_size % 16;
    if(remainder == 0) return data_size + 16;
    return data_size + (16 - remainder);
}

static bool add_padding(const uint8_t* input, size_t input_size,  uint8_t* output, size_t* output_size) {
    if(!input || !output || !output_size) return false;
    
    size_t padded = furi_hal_crypto_padded_size(input_size);
    size_t pad_len = padded - input_size;
    
    memcpy(output, input, input_size);
    for(size_t i = input_size; i < padded; i++) {
        output[i] = (uint8_t)pad_len;
    }
    *output_size = padded;
    return true;
}

////////////////

static bool remove_padding(const uint8_t* input, size_t input_size,
                            size_t* output_size) {
    /*FURI_LOG_I("Crypto", "remove_padding: input_size=%zu", input_size);*/
    
    if(!input || !output_size) {
        FURI_LOG_E("Crypto", "remove_padding: null pointer");
        return false;
    }
    if(input_size == 0 || input_size % 16 != 0) {
        FURI_LOG_E("Crypto", "remove_padding: bad size=%zu", input_size);
        return false;
    }
    
    uint8_t pad_len = input[input_size - 1];
    /*FURI_LOG_I("Crypto", "remove_padding: pad_len=%u", pad_len);*/
    
    if(pad_len == 0 || pad_len > 16) {
        FURI_LOG_E("Crypto", "remove_padding: invalid pad_len=%u", pad_len);
        return false;
    }
    
    for(size_t i = input_size - pad_len; i < input_size; i++) {
        if(input[i] != pad_len) {
            FURI_LOG_E("Crypto", "remove_padding: byte mismatch at %zu, expected 0x%02X got 0x%02X", i, pad_len, input[i]);
            return false;
        }
    }
    
    *output_size = input_size - pad_len;
    /*FURI_LOG_I("Crypto", "remove_padding: OK, output_size=%zu", *output_size);*/
    return true;
}



static bool ensure_key_ready(void) {
    if(!key_scheduled) {
        KeyExpansion(HARDCODED_KEY, roundKeys);
        key_scheduled = true;
    }
    return key_scheduled;
}

// API 
void furi_hal_crypto_init(void) {
    key_scheduled = false;
    memset(current_iv, 0, 16);
}

bool furi_hal_crypto_enclave_verify(uint8_t* keys_nb, uint8_t* valid_keys_nb) {
    if(keys_nb) *keys_nb = 1;
    if(valid_keys_nb) *valid_keys_nb = 1;
    return true;
}

bool furi_hal_crypto_enclave_ensure_key(uint8_t key_slot) {
    (void)key_slot;
    return true;
}

bool furi_hal_crypto_enclave_store_key(FuriHalCryptoKey* key, uint8_t* slot) {
    if(!key || !slot) return false;
    *slot = 0;
    return false; 
}

bool furi_hal_crypto_enclave_load_key(uint8_t slot, const uint8_t* iv) {
    (void)slot;
    (void)iv;
    return ensure_key_ready();
}

bool furi_hal_crypto_enclave_unload_key(uint8_t slot) {
    (void)slot;
    return true;
}

bool furi_hal_crypto_load_key(const uint8_t* key, const uint8_t* iv) {
    (void)key;
    (void)iv;
    if(iv) memcpy(current_iv, iv, 16);
    return ensure_key_ready();
}

bool furi_hal_crypto_unload_key(void) {
    return true;
}

bool furi_hal_crypto_encrypt(const uint8_t* input, uint8_t* output, size_t size) {
    if(!input || !output || size == 0) {
        return false;
    }
    if(!ensure_key_ready()) {
        return false;
    }
    
    size_t padded_size = furi_hal_crypto_padded_size(size);
    
    uint8_t* padded = malloc(padded_size);
    if(!padded) return false;
    
    size_t actual_padded;
    if(!add_padding(input, size, padded, &actual_padded)) {
        free(padded);
        return false;
    }
    
    uint8_t iv_copy[16];
    uint8_t block[16];
    memcpy(iv_copy, current_iv, 16);
    
    for(size_t i = 0; i < actual_padded; i += 16) {
        memcpy(block, padded + i, 16);
        xor_blocks(block, iv_copy);
        AES_EncryptBlock(block, roundKeys);
        memcpy(iv_copy, block, 16);
        memcpy(output + i, block, 16);
    }
    
    free(padded);
    return true;
}

//////////////////

bool furi_hal_crypto_decrypt(const uint8_t* input, uint8_t* output, size_t size, size_t* output_size) {
    /*FURI_LOG_I("Crypto", "Decrypt called, size=%zu", size);*/
    
    if(!input || !output || size == 0 || size % 16 != 0 || !output_size) {
        FURI_LOG_E("Crypto", "Invalid parameters");
        return false;
    }
    if(!ensure_key_ready()) {
        FURI_LOG_E("Crypto", "Key not ready");
        return false;
    }
    
    /*FURI_LOG_I("Crypto", "current_iv: %02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
        current_iv[0], current_iv[1], current_iv[2], current_iv[3],
        current_iv[4], current_iv[5], current_iv[6], current_iv[7],
        current_iv[8], current_iv[9], current_iv[10], current_iv[11],
        current_iv[12], current_iv[13], current_iv[14], current_iv[15]);*/
    
    /*FURI_LOG_I("Crypto", "Input first block: %02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
        input[0], input[1], input[2], input[3], input[4], input[5], input[6], input[7],
        input[8], input[9], input[10], input[11], input[12], input[13], input[14], input[15]);*/
    
    uint8_t iv_copy[16];
    uint8_t block[16];
    uint8_t prev_cipher[16];
    
    memcpy(iv_copy, current_iv, 16);
    
    for(size_t i = 0; i < size; i += 16) {
        memcpy(block, input + i, 16);
        memcpy(prev_cipher, block, 16);
        AES_DecryptBlock(block, roundKeys);
        xor_blocks(block, iv_copy);
        memcpy(iv_copy, prev_cipher, 16);
        memcpy(output + i, block, 16);
    }
    
    /*FURI_LOG_I("Crypto", "After AES, last byte=0x%02X", output[size-1]);*/
    
    // REMOVE PKCS#7 padding
    if(!remove_padding(output, size, output_size)) {
        FURI_LOG_E("Crypto", "Padding check failed, last_byte=0x%02X, size=%zu", output[size-1], size);
        memset(output, 0, size);
        return false;
    }
    
    /*FURI_LOG_I("Crypto", "Decrypt OK, output_size=%zu", *output_size);*/
    return true;
}



// MAC KEY GENERATION

void furi_hal_crypto_generate_device_key(uint8_t* output_key) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    uint8_t block[16] = {0};
    memcpy(block, mac, 6);

    uint8_t rk[11][16];
    KeyExpansion(HARDCODED_KEY, rk);
    AES_EncryptBlock(block, rk);

    memcpy(output_key, block, 16);
}

void furi_hal_crypto_generate_device_iv(uint8_t* iv_out, const char* salt) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    uint8_t buffer[16] = {0};
    memcpy(buffer, mac, 6);
    if (salt) {
        size_t len = strlen(salt);
        if (len > 10) len = 10;
        memcpy(buffer + 6, salt, len);
    }

    uint8_t rk[11][16];
    KeyExpansion(HARDCODED_KEY, rk);
    AES_EncryptBlock(buffer, rk);
    memcpy(iv_out, buffer, 16);
}

bool furi_hal_crypto_encrypt_with_key(const uint8_t* key, const uint8_t* iv, const uint8_t* input, uint8_t* output,  size_t size) {
    if (!key || !iv || !input || !output || size == 0) return false;

    uint8_t rk[11][16];
    KeyExpansion(key, rk);

    size_t padded_size = furi_hal_crypto_padded_size(size);
    uint8_t* padded = malloc(padded_size);
    if (!padded) return false;

    size_t actual_padded;
    if (!add_padding(input, size, padded, &actual_padded)) {
        free(padded);
        return false;
    }

    uint8_t iv_copy[16];
    memcpy(iv_copy, iv, 16);
    uint8_t block[16];

    for (size_t i = 0; i < actual_padded; i += 16) {
        memcpy(block, padded + i, 16);
        xor_blocks(block, iv_copy);
        AES_EncryptBlock(block, rk);
        memcpy(iv_copy, block, 16);
        memcpy(output + i, block, 16);
    }

    free(padded);
    return true;
}

bool furi_hal_crypto_decrypt_with_key(const uint8_t* key, const uint8_t* iv,  const uint8_t* input, uint8_t* output,  size_t size, size_t* output_size) {
    if (!key || !iv || !input || !output || size == 0 || size % 16 != 0 || !output_size)
        return false;

    uint8_t rk[11][16];
    KeyExpansion(key, rk);

    uint8_t iv_copy[16];
    memcpy(iv_copy, iv, 16);
    uint8_t block[16];
    uint8_t prev_cipher[16];

    for (size_t i = 0; i < size; i += 16) {
        memcpy(block, input + i, 16);
        memcpy(prev_cipher, block, 16);
        AES_DecryptBlock(block, rk);
        xor_blocks(block, iv_copy);
        memcpy(iv_copy, prev_cipher, 16);
        memcpy(output + i, block, 16);
    }

    if (!remove_padding(output, size, output_size)) {
        memset(output, 0, size);
        return false;
    }
    return true;
}

bool furi_hal_crypto_encrypt_with_device_key(const uint8_t* iv, const uint8_t* input, uint8_t* output, size_t size) {
    if (!iv || !input || !output || size == 0) return false;

    uint8_t device_key[16];
    furi_hal_crypto_generate_device_key(device_key);

    return furi_hal_crypto_encrypt_with_key(device_key, iv, input, output, size);
}

bool furi_hal_crypto_decrypt_with_device_key(const uint8_t* iv, const uint8_t* input, uint8_t* output, size_t size, size_t* output_size) {
    if (!iv || !input || !output || size == 0 || size % 16 != 0 || !output_size)
        return false;

    uint8_t device_key[16];
    furi_hal_crypto_generate_device_key(device_key);

    return furi_hal_crypto_decrypt_with_key(device_key, iv, input, output, size, output_size);
}


// DEBUG

static void print_hex(const char* label, const uint8_t* data, size_t len) {
    printf("%s: ", label);
    for (size_t i = 0; i < len; i++) {
        printf("%02X ", data[i]);
    }
    printf("\r\n");
}

static bool test_crypto_with_key(const char* key_name, const uint8_t* key, const uint8_t* iv,
                                 const uint8_t* plaintext, size_t plain_len) {
    printf("\r\n=== TEST %s ===\r\n", key_name);
    print_hex("Plaintext", plaintext, plain_len);
    print_hex("IV", iv, 16);

    size_t cipher_len = furi_hal_crypto_padded_size(plain_len);
    uint8_t* ciphertext = malloc(cipher_len);
    uint8_t* decrypted = malloc(cipher_len);
    if (!ciphertext || !decrypted) {
        printf("ERROR: allocation memory failed\r\n");
        free(ciphertext);
        free(decrypted);
        return false;
    }

    bool enc_ok = furi_hal_crypto_encrypt_with_key(key, iv, plaintext, ciphertext, plain_len);
    if (!enc_ok) {
        printf("ERROR: encryption failed\r\n");
        free(ciphertext);
        free(decrypted);
        return false;
    }
    print_hex("Ciphertext", ciphertext, cipher_len);

    size_t out_len;
    bool dec_ok = furi_hal_crypto_decrypt_with_key(key, iv, ciphertext, decrypted, cipher_len, &out_len);
    if (!dec_ok) {
        printf("ERROR: decryption failed\r\n");
        free(ciphertext);
        free(decrypted);
        return false;
    }
    print_hex("Decrypted", decrypted, out_len);

    bool match = (out_len == plain_len) && (memcmp(plaintext, decrypted, plain_len) == 0);
    printf("RESULT: %s\r\n", match ? "SUCCESS" : "FAILURE (data does not match)");

    free(ciphertext);
    free(decrypted);
    return match;
}

void furi_hal_crypto_debug_test(void) {
    printf("\r\n========== FURI HAL CRYPTO DEBUG TEST ==========\r\n");


    const uint8_t* fixed_key = HARDCODED_KEY;
    uint8_t fixed_iv[16] = {0};

    const char* test_plain = "Hello World by THOR! Test message.";
    size_t plain_len = strlen(test_plain);
    const uint8_t* plaintext = (const uint8_t*)test_plain;

    test_crypto_with_key("STANDARD KEY (HARDCODED)", fixed_key, fixed_iv, plaintext, plain_len);

    uint8_t device_key[16];
    furi_hal_crypto_generate_device_key(device_key);

    uint8_t device_iv[16];
    furi_hal_crypto_generate_device_iv(device_iv, "debug_salt");

    test_crypto_with_key("DERIVED KEY (FROM MAC)", device_key, device_iv, plaintext, plain_len);

    printf("\r\n==========  TEST DEBUG END==========\r\n");
}


