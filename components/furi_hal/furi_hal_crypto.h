#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FURI_HAL_CRYPTO_ENCLAVE_UNIQUE_KEY_SLOT (11u)

// Upstream STM32 firmware splits the enclave into factory slots (1-10, shared
// across all devices) and user slots (12-100, per-device). This port stubs the
// enclave and only provides the per-device UNIQUE slot (11). Apps that pick a
// "user" slot to avoid the shared factory keys (e.g. the authenticator) map
// onto the unique slot here.
#define FURI_HAL_CRYPTO_ENCLAVE_USER_KEY_SLOT_START FURI_HAL_CRYPTO_ENCLAVE_UNIQUE_KEY_SLOT
#define FURI_HAL_CRYPTO_ENCLAVE_USER_KEY_SLOT_END   FURI_HAL_CRYPTO_ENCLAVE_UNIQUE_KEY_SLOT

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

// Funzioni originali
void furi_hal_crypto_init(void);
bool furi_hal_crypto_enclave_verify(uint8_t* keys_nb, uint8_t* valid_keys_nb);
bool furi_hal_crypto_enclave_ensure_key(uint8_t key_slot);
bool furi_hal_crypto_enclave_store_key(FuriHalCryptoKey* key, uint8_t* slot);
bool furi_hal_crypto_enclave_load_key(uint8_t slot, const uint8_t* iv);
bool furi_hal_crypto_enclave_unload_key(uint8_t slot);
bool furi_hal_crypto_load_key(const uint8_t* key, const uint8_t* iv);
bool furi_hal_crypto_unload_key(void);
bool furi_hal_crypto_encrypt(const uint8_t* input, uint8_t* output, size_t size);
bool furi_hal_crypto_decrypt(const uint8_t* input, uint8_t* output, size_t size, size_t* output_size);

//new apis
void furi_hal_crypto_generate_device_key(uint8_t* output_key);
void furi_hal_crypto_generate_device_iv(uint8_t* iv_out, const char* salt);
bool furi_hal_crypto_encrypt_with_key(const uint8_t* key, const uint8_t* iv, const uint8_t* input, uint8_t* output, size_t size);
bool furi_hal_crypto_decrypt_with_key(const uint8_t* key, const uint8_t* iv, const uint8_t* input, uint8_t* output, size_t size, size_t* output_size);
bool furi_hal_crypto_encrypt_with_device_key(const uint8_t* iv, const uint8_t* input, uint8_t* output, size_t size);
bool furi_hal_crypto_decrypt_with_device_key(const uint8_t* iv, const uint8_t* input, uint8_t* output, size_t size, size_t* output_size);
void furi_hal_crypto_debug_test(void);

#ifdef __cplusplus
}
#endif
