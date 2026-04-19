#include "furi_hal_crypto.h"

#include <string.h>

#include <core/common_defines.h>
<<<<<<< HEAD
#include <mbedtls/aes.h>
=======
>>>>>>> 05c91cb486590019377b94b79a37919e1c650685

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
<<<<<<< HEAD

bool furi_hal_crypto_aes128_ecb_encrypt(
    const uint8_t* key,
    const uint8_t* input,
    uint8_t* output) {
    if(!key || !input || !output) {
        return false;
    }

    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    if(mbedtls_aes_setkey_enc(&ctx, key, 128) != 0) {
        mbedtls_aes_free(&ctx);
        return false;
    }
    uint8_t buf[16];
    memcpy(buf, input, 16);
    const int err = mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, buf, output);
    mbedtls_aes_free(&ctx);
    return err == 0;
}

bool furi_hal_crypto_aes128_ecb_decrypt(
    const uint8_t* key,
    const uint8_t* input,
    uint8_t* output) {
    if(!key || !input || !output) {
        return false;
    }

    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    if(mbedtls_aes_setkey_dec(&ctx, key, 128) != 0) {
        mbedtls_aes_free(&ctx);
        return false;
    }
    uint8_t buf[16];
    memcpy(buf, input, 16);
    const int err = mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_DECRYPT, buf, output);
    mbedtls_aes_free(&ctx);
    return err == 0;
}
=======
>>>>>>> 05c91cb486590019377b94b79a37919e1c650685
