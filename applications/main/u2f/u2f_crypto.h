#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool u2f_crypto_encrypt(
    const uint8_t* plaintext,
    uint8_t* ciphertext_and_tag,
    size_t length,
    const uint8_t nonce[16]);

bool u2f_crypto_decrypt(
    const uint8_t* ciphertext_and_tag,
    uint8_t* plaintext,
    size_t length,
    const uint8_t nonce[16]);
