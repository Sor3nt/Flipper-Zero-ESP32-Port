#include "u2f_crypto.h"

#include <furi_hal_crypto.h>

bool u2f_crypto_encrypt(
    const uint8_t* plaintext,
    uint8_t* ciphertext_and_tag,
    size_t length,
    const uint8_t nonce[16]) {
    return furi_hal_crypto_u2f_encrypt(plaintext, ciphertext_and_tag, length, nonce);
}

bool u2f_crypto_decrypt(
    const uint8_t* ciphertext_and_tag,
    uint8_t* plaintext,
    size_t length,
    const uint8_t nonce[16]) {
    return furi_hal_crypto_u2f_decrypt(ciphertext_and_tag, plaintext, length, nonce);
}
