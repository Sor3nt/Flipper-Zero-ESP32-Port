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

void furi_hal_crypto_init(void);
/* Call after default NVS init to move any existing U2F key to the dedicated partition. */
bool furi_hal_crypto_u2f_storage_key_migrate(void);
bool furi_hal_crypto_enclave_verify(uint8_t* keys_nb, uint8_t* valid_keys_nb);
bool furi_hal_crypto_enclave_ensure_key(uint8_t key_slot);
bool furi_hal_crypto_enclave_store_key(FuriHalCryptoKey* key, uint8_t* slot);
bool furi_hal_crypto_enclave_load_key(uint8_t slot, const uint8_t* iv);
bool furi_hal_crypto_enclave_unload_key(uint8_t slot);
bool furi_hal_crypto_load_key(const uint8_t* key, const uint8_t* iv);
bool furi_hal_crypto_unload_key(void);
/* WARNING: on this port encrypt/decrypt (and the enclave functions above) do NOT
 * encrypt. They return true and copy the data unchanged; a warning is logged the
 * first time. Only the furi_hal_crypto_u2f_* functions below are real AES-GCM. */
bool furi_hal_crypto_encrypt(const uint8_t* input, uint8_t* output, size_t size);
bool furi_hal_crypto_decrypt(const uint8_t* input, uint8_t* output, size_t size);
/* U2F buffers require size + 16 bytes; the 16-byte nonce must never repeat. */
bool furi_hal_crypto_u2f_encrypt(
    const uint8_t* plaintext,
    uint8_t* ciphertext_and_tag,
    size_t size,
    const uint8_t nonce[16]);
bool furi_hal_crypto_u2f_decrypt(
    const uint8_t* ciphertext_and_tag,
    uint8_t* plaintext,
    size_t size,
    const uint8_t nonce[16]);

#ifdef __cplusplus
}
#endif
