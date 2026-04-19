#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FURI_HAL_CRYPTO_ENCLAVE_UNIQUE_KEY_SLOT (11u)

typedef enum {
    FuriHalCryptoKeyTypeMaster,
    FuriHalCryptoKeyTypeSimple,
    FuriHalCryptoKeyTypeEncrypted,
} FuriHalCryptoKeyType;

typedef enum {
    FuriHalCryptoKeySize128,
    FuriHalCryptoKeySize256,
} FuriHalCryptoKeySize;

typedef struct {
    FuriHalCryptoKeyType type;
    FuriHalCryptoKeySize size;
    uint8_t* data;
} FuriHalCryptoKey;

void furi_hal_crypto_init(void);
bool furi_hal_crypto_enclave_verify(uint8_t* keys_nb, uint8_t* valid_keys_nb);
bool furi_hal_crypto_enclave_ensure_key(uint8_t key_slot);
bool furi_hal_crypto_enclave_store_key(FuriHalCryptoKey* key, uint8_t* slot);
bool furi_hal_crypto_enclave_load_key(uint8_t slot, const uint8_t* iv);
bool furi_hal_crypto_enclave_unload_key(uint8_t slot);
bool furi_hal_crypto_load_key(const uint8_t* key, const uint8_t* iv);
bool furi_hal_crypto_unload_key(void);
bool furi_hal_crypto_encrypt(const uint8_t* input, uint8_t* output, size_t size);
bool furi_hal_crypto_decrypt(const uint8_t* input, uint8_t* output, size_t size);

bool furi_hal_crypto_aes128_ecb_encrypt(
    const uint8_t* key,
    const uint8_t* input,
    uint8_t* output);
bool furi_hal_crypto_aes128_ecb_decrypt(
    const uint8_t* key,
    const uint8_t* input,
    uint8_t* output);

#ifdef __cplusplus
}
#endif
