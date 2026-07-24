/**
 * \file des.h
 * 
 * \brief DES stub for ESP-IDF 6.2 compatibility
 * DES is not available in TF-PSA Crypto used by ESP-IDF 6.2
 * This is a minimal stub to allow compilation of NFC components
 */

#ifndef MBEDTLS_DES_H
#define MBEDTLS_DES_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MBEDTLS_DES_ENCRYPT     1
#define MBEDTLS_DES_DECRYPT     0

/**
 * DES context structure (stub - not functional)
 */
typedef struct {
    uint32_t sk[32];
} mbedtls_des_context;

/**
 * Triple DES context structure (stub - not functional)
 */
typedef struct {
    uint32_t sk[96];
} mbedtls_des3_context;

/**
 * \brief          Initialize a DES context (stub)
 */
static inline void mbedtls_des_init(mbedtls_des_context *ctx) {
    (void)ctx;
}

/**
 * \brief          Clear a DES context (stub)
 */
static inline void mbedtls_des_free(mbedtls_des_context *ctx) {
    (void)ctx;
}

/**
 * \brief          Set key parity bits (stub)
 */
static inline int mbedtls_des_key_set_parity(unsigned char key[8]) {
    (void)key;
    return 0;
}

/**
 * \brief          Check key parity (stub)
 */
static inline int mbedtls_des_key_check_key_parity(const unsigned char key[8]) {
    (void)key;
    return 0;
}

/**
 * \brief          Check weak keys (stub)
 */
static inline int mbedtls_des_key_check_weak_key(const unsigned char key[8]) {
    (void)key;
    return 0;
}

/**
 * \brief          DES key schedule (stub)
 */
static inline int mbedtls_des_setkey_enc(mbedtls_des_context *ctx,
                                         const unsigned char key[8]) {
    (void)ctx;
    (void)key;
    return 0;
}

/**
 * \brief          DES key schedule (stub)
 */
static inline int mbedtls_des_setkey_dec(mbedtls_des_context *ctx,
                                         const unsigned char key[8]) {
    (void)ctx;
    (void)key;
    return 0;
}

/**
 * \brief          DES-ECB block encryption/decryption (stub)
 */
static inline int mbedtls_des_crypt_ecb(mbedtls_des_context *ctx,
                                        const unsigned char input[8],
                                        unsigned char output[8]) {
    (void)ctx;
    (void)input;
    (void)output;
    return 0;
}

/**
 * \brief          DES-CBC buffer encryption/decryption (stub)
 */
static inline int mbedtls_des_crypt_cbc(mbedtls_des_context *ctx,
                                        int mode,
                                        size_t length,
                                        unsigned char iv[8],
                                        const unsigned char *input,
                                        unsigned char *output) {
    (void)ctx;
    (void)mode;
    (void)length;
    (void)iv;
    (void)input;
    (void)output;
    return 0;
}

/**
 * \brief          Initialize a Triple DES context (stub)
 */
static inline void mbedtls_des3_init(mbedtls_des3_context *ctx) {
    (void)ctx;
}

/**
 * \brief          Clear a Triple DES context (stub)
 */
static inline void mbedtls_des3_free(mbedtls_des3_context *ctx) {
    (void)ctx;
}

/**
 * \brief          Triple DES key schedule (stub)
 */
static inline int mbedtls_des3_set2key_enc(mbedtls_des3_context *ctx,
                                           const unsigned char key[16]) {
    (void)ctx;
    (void)key;
    return 0;
}

/**
 * \brief          Triple DES key schedule (stub)
 */
static inline int mbedtls_des3_set2key_dec(mbedtls_des3_context *ctx,
                                           const unsigned char key[16]) {
    (void)ctx;
    (void)key;
    return 0;
}

/**
 * \brief          Triple DES key schedule (stub)
 */
static inline int mbedtls_des3_set3key_enc(mbedtls_des3_context *ctx,
                                           const unsigned char key[24]) {
    (void)ctx;
    (void)key;
    return 0;
}

/**
 * \brief          Triple DES key schedule (stub)
 */
static inline int mbedtls_des3_set3key_dec(mbedtls_des3_context *ctx,
                                           const unsigned char key[24]) {
    (void)ctx;
    (void)key;
    return 0;
}

/**
 * \brief          Triple DES-ECB block encryption/decryption (stub)
 */
static inline int mbedtls_des3_crypt_ecb(mbedtls_des3_context *ctx,
                                         const unsigned char input[8],
                                         unsigned char output[8]) {
    (void)ctx;
    (void)input;
    (void)output;
    return 0;
}

/**
 * \brief          Triple DES-CBC buffer encryption/decryption (stub)
 */
static inline int mbedtls_des3_crypt_cbc(mbedtls_des3_context *ctx,
                                         int mode,
                                         size_t length,
                                         unsigned char iv[8],
                                         const unsigned char *input,
                                         unsigned char *output) {
    (void)ctx;
    (void)mode;
    (void)length;
    (void)iv;
    (void)input;
    (void)output;
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* MBEDTLS_DES_H */
