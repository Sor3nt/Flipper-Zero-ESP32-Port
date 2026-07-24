/**
 * \file sha1.h
 * 
 * \brief SHA1 stub for ESP-IDF 6.2 compatibility
 * SHA1 is not available in TF-PSA Crypto used by ESP-IDF 6.2
 * This is a minimal stub to allow compilation of components using SHA1
 */

#ifndef MBEDTLS_SHA1_H
#define MBEDTLS_SHA1_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \brief SHA1 context structure
 */
typedef struct {
    uint32_t total[2];
    uint32_t state[5];
    unsigned char buffer[64];
} mbedtls_sha1_context;

/**
 * \brief          Initialize a SHA1 context (stub)
 */
static inline void mbedtls_sha1_init(mbedtls_sha1_context *ctx) {
    (void)ctx;
}

/**
 * \brief          Clear a SHA1 context (stub)
 */
static inline void mbedtls_sha1_free(mbedtls_sha1_context *ctx) {
    (void)ctx;
}

/**
 * \brief          Clone a SHA1 context (stub)
 */
static inline void mbedtls_sha1_clone(mbedtls_sha1_context *dst,
                                      const mbedtls_sha1_context *src) {
    (void)dst;
    (void)src;
}

/**
 * \brief          SHA1 context setup (stub)
 */
static inline int mbedtls_sha1_starts(mbedtls_sha1_context *ctx) {
    (void)ctx;
    return 0;
}

/**
 * \brief          SHA1 process buffer (stub)
 */
static inline int mbedtls_sha1_update(mbedtls_sha1_context *ctx,
                                      const unsigned char *input,
                                      size_t ilen) {
    (void)ctx;
    (void)input;
    (void)ilen;
    return 0;
}

/**
 * \brief          SHA1 final digest (stub)
 */
static inline int mbedtls_sha1_finish(mbedtls_sha1_context *ctx,
                                      unsigned char output[20]) {
    (void)ctx;
    (void)output;
    return 0;
}

/**
 * \brief          Output = SHA1(input buffer) (stub)
 */
static inline int mbedtls_sha1(const unsigned char *input,
                               size_t ilen,
                               unsigned char output[20]) {
    (void)input;
    (void)ilen;
    (void)output;
    return 0;
}

/**
 * \brief          Checksum for internal use (stub)
 */
static inline void mbedtls_sha1_process(mbedtls_sha1_context *ctx,
                                        const unsigned char data[64]) {
    (void)ctx;
    (void)data;
}

#ifdef __cplusplus
}
#endif

#endif /* MBEDTLS_SHA1_H */
