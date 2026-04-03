#include "furi_hal_crypto.h"

#include <string.h>

#include <core/common_defines.h>

void furi_hal_crypto_init(void) {
}

bool furi_hal_crypto_enclave_verify(uint8_t* keys_nb, uint8_t* valid_keys_nb) {
    if(keys_nb) {
        *keys_nb = 1;
    }
    if(valid_keys_nb) {
        *valid_keys_nb = 1;
    }
    return true;
}

bool furi_hal_crypto_enclave_ensure_key(uint8_t key_slot) {
    return key_slot <= FURI_HAL_CRYPTO_ENCLAVE_UNIQUE_KEY_SLOT;
}

bool furi_hal_crypto_enclave_store_key(FuriHalCryptoKey* key, uint8_t* slot) {
    if(!key || !slot) {
        return false;
    }

    *slot = FURI_HAL_CRYPTO_ENCLAVE_UNIQUE_KEY_SLOT;
    return true;
}

bool furi_hal_crypto_enclave_load_key(uint8_t slot, const uint8_t* iv) {
    UNUSED(slot);
    UNUSED(iv);
    return true;
}

bool furi_hal_crypto_enclave_unload_key(uint8_t slot) {
    UNUSED(slot);
    return true;
}

bool furi_hal_crypto_load_key(const uint8_t* key, const uint8_t* iv) {
    UNUSED(key);
    UNUSED(iv);
    return true;
}

bool furi_hal_crypto_unload_key(void) {
    return true;
}

bool furi_hal_crypto_encrypt(const uint8_t* input, uint8_t* output, size_t size) {
    if(!input || !output) {
        return false;
    }

    memcpy(output, input, size);
    return true;
}

bool furi_hal_crypto_decrypt(const uint8_t* input, uint8_t* output, size_t size) {
    if(!input || !output) {
        return false;
    }

    memcpy(output, input, size);
    return true;
}
