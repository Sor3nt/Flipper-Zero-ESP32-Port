#include "u2f.h"
#include "u2f_data.h"

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_random.h>

#include <mbedtls/sha256.h>
#include <mbedtls/md.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/error.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/platform_util.h>

#define U2F_USER_PRESENCE_APPROVAL_MS 3000

#define TAG "U2f"

#define WORKER_TAG TAG "Worker"

#define U2F_CMD_REGISTER     0x01
#define U2F_CMD_AUTHENTICATE 0x02
#define U2F_CMD_VERSION      0x03

typedef enum {
    U2fCheckOnly = 0x07, // "check-only" - only check key handle, don't send auth response
    U2fEnforce =
        0x03, // "enforce-user-presence-and-sign" - send auth response only if user is present
    U2fDontEnforce =
        0x08, // "dont-enforce-user-presence-and-sign" - send auth response even if user is missing
} U2fAuthMode;

#define U2F_HASH_SIZE      32
#define U2F_NONCE_SIZE     32
#define U2F_CHALLENGE_SIZE 32
#define U2F_APP_ID_SIZE    32
#define U2F_APDU_LE_SIZE   2
#define U2F_ATTESTATION_CERT_MAX_SIZE 2048

#define U2F_EC_KEY_SIZE    32
#define U2F_EC_BIGNUM_SIZE 32
#define U2F_EC_POINT_SIZE  65

typedef struct {
    uint8_t format;
    uint8_t xy[64];
} FURI_PACKED U2fPubKey;
_Static_assert(sizeof(U2fPubKey) == U2F_EC_POINT_SIZE, "U2fPubKey size mismatch");

typedef struct {
    uint8_t len;
    uint8_t hash[U2F_HASH_SIZE];
    uint8_t nonce[U2F_NONCE_SIZE];
} FURI_PACKED U2fKeyHandle;

typedef struct {
    uint8_t cla;
    uint8_t ins;
    uint8_t p1;
    uint8_t p2;
    uint8_t len[3];
    uint8_t challenge[U2F_CHALLENGE_SIZE];
    uint8_t app_id[U2F_APP_ID_SIZE];
} FURI_PACKED U2fRegisterReq;

typedef struct {
    uint8_t reserved;
    U2fPubKey pub_key;
    U2fKeyHandle key_handle;
    uint8_t cert[];
} FURI_PACKED U2fRegisterResp;

typedef struct {
    uint8_t cla;
    uint8_t ins;
    uint8_t p1;
    uint8_t p2;
    uint8_t len[3];
    uint8_t challenge[U2F_CHALLENGE_SIZE];
    uint8_t app_id[U2F_APP_ID_SIZE];
    U2fKeyHandle key_handle;
} FURI_PACKED U2fAuthReq;

typedef struct {
    uint8_t user_present;
    uint32_t counter;
    uint8_t signature[];
} FURI_PACKED U2fAuthResp;

static const uint8_t ver_str[] = {"U2F_V2"};

static const uint8_t state_no_error[] = {0x90, 0x00};
static const uint8_t state_not_supported[] = {0x6D, 0x00};
static const uint8_t state_user_missing[] = {0x69, 0x85};
static const uint8_t state_wrong_data[] = {0x6A, 0x80};
static const uint8_t state_internal_error[] = {0x6F, 0x00};

struct U2fData {
    uint8_t device_key[U2F_EC_KEY_SIZE];
    uint8_t cert_key[U2F_EC_KEY_SIZE];
    uint32_t counter;
    bool ready;
    bool connected;
    bool shutting_down;
    bool presence_pending;
    bool presence_approved;
    uint32_t presence_approved_tick;
    FuriMutex* presence_mutex;
    U2fEvtCallback callback;
    void* context;
    mbedtls_ecp_group group;
};

static int u2f_uecc_random_cb(void* context, uint8_t* dest, size_t size) {
    UNUSED(context);
    furi_hal_random_fill_buf(dest, (uint32_t)size);
    return 0;
}

static bool u2f_attestation_key_matches_certificate(const U2fData* U2F) {
    // Heap, not stack: this runs on the 8 KB app thread on top of x509/pk/ECDSA call depth.
    uint8_t* certificate = malloc(U2F_ATTESTATION_CERT_MAX_SIZE);
    if(!certificate) {
        FURI_LOG_E(TAG, "Out of memory while validating the attestation certificate");
        return false;
    }
    uint32_t certificate_size = u2f_data_cert_load(certificate);
    if(certificate_size == 0) {
        FURI_LOG_E(TAG, "Unable to load the attestation certificate");
        free(certificate);
        return false;
    }

    mbedtls_x509_crt parsed_certificate;
    mbedtls_x509_crt_init(&parsed_certificate);
    int result = mbedtls_x509_crt_parse_der(&parsed_certificate, certificate, certificate_size);
    mbedtls_platform_zeroize(certificate, U2F_ATTESTATION_CERT_MAX_SIZE);
    free(certificate);
    if(result != 0 || mbedtls_pk_get_type(&parsed_certificate.pk) != MBEDTLS_PK_ECKEY) {
        mbedtls_x509_crt_free(&parsed_certificate);
        FURI_LOG_E(TAG, "Unable to parse an EC attestation certificate");
        return false;
    }

    mbedtls_pk_context private_key;
    mbedtls_pk_init(&private_key);
    const mbedtls_ecp_keypair* public_key = mbedtls_pk_ec(parsed_certificate.pk);
    if(!public_key ||
       mbedtls_ecp_keypair_get_group_id(public_key) != MBEDTLS_ECP_DP_SECP256R1) {
        result = MBEDTLS_ERR_ECP_BAD_INPUT_DATA;
    }
    if(result == 0) {
        const mbedtls_pk_info_t* key_info = mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY);
        result = key_info ? mbedtls_pk_setup(&private_key, key_info) :
                            MBEDTLS_ERR_PK_BAD_INPUT_DATA;
    }
    if(result == 0) {
        result = mbedtls_ecp_read_key(
            MBEDTLS_ECP_DP_SECP256R1,
            mbedtls_pk_ec(private_key),
            U2F->cert_key,
            sizeof(U2F->cert_key));
    }
    if(result == 0) {
        result = mbedtls_ecp_keypair_calc_public(
            mbedtls_pk_ec(private_key), u2f_uecc_random_cb, NULL);
    }
    if(result == 0) {
        result = mbedtls_pk_check_pair(
            &parsed_certificate.pk, &private_key, u2f_uecc_random_cb, NULL);
    }

    mbedtls_pk_free(&private_key);
    mbedtls_x509_crt_free(&parsed_certificate);
    if(result != 0) {
        FURI_LOG_E(TAG, "Attestation key/certificate validation failed: %d", result);
    }
    return result == 0;
}

U2fData* u2f_alloc(void) {
    U2fData* U2F = malloc(sizeof(U2fData));
    if(!U2F) return NULL;

    memset(U2F, 0, sizeof(U2fData));
    mbedtls_ecp_group_init(&U2F->group);
    U2F->presence_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    if(!U2F->presence_mutex) {
        mbedtls_ecp_group_free(&U2F->group);
        free(U2F);
        return NULL;
    }
    return U2F;
}

void u2f_free(U2fData* U2F) {
    furi_assert(U2F);
    mbedtls_ecp_group_free(&U2F->group);
    furi_mutex_free(U2F->presence_mutex);
    mbedtls_platform_zeroize(U2F, sizeof(*U2F));
    free(U2F);
}

bool u2f_init(U2fData* U2F) {
    furi_assert(U2F);

    if(u2f_data_cert_check() == false) {
        // No usable attestation certificate. The UI offers to generate a
        // self-signed one (u2f_data_cert_generate) and then re-initializes.
        FURI_LOG_E(TAG, "Certificate load error");
        return false;
    }
    if(u2f_data_cert_key_load(U2F->cert_key) == false) {
        FURI_LOG_E(TAG, "Certificate key load error");
        return false;
    }
    U2fDataLoadResult key_result = u2f_data_key_load(U2F->device_key);
    U2fDataLoadResult counter_result = u2f_data_cnt_read(&U2F->counter);
    U2fDataLoadResult provisioning_result = u2f_data_provisioning_state();
    if(key_result == U2fDataLoadInvalid || counter_result == U2fDataLoadInvalid) {
        FURI_LOG_E(TAG, "Existing authenticator state is invalid; refusing to reset it");
        return false;
    }
    if(provisioning_result == U2fDataLoadInvalid) {
        FURI_LOG_E(TAG, "Provisioning marker is invalid; refusing authenticator initialization");
        return false;
    }
    bool provisioning = provisioning_result == U2fDataLoadOk;
    if(!provisioning && key_result == U2fDataLoadMissing &&
       counter_result == U2fDataLoadMissing) {
        if(!u2f_data_provisioning_begin()) {
            FURI_LOG_E(TAG, "Unable to persist U2F provisioning marker");
            return false;
        }
        provisioning = true;
    }

    if(provisioning) {
        if(counter_result == U2fDataLoadOk && U2F->counter != 0) {
            FURI_LOG_E(TAG, "Provisioning marker conflicts with a nonzero counter");
            return false;
        }
        if(key_result == U2fDataLoadMissing && !u2f_data_key_generate(U2F->device_key)) {
            FURI_LOG_E(TAG, "Device key provisioning failed");
            return false;
        }
        if(counter_result == U2fDataLoadMissing) {
            U2F->counter = 0;
            if(!u2f_data_cnt_write(0)) {
                FURI_LOG_E(TAG, "Counter provisioning failed");
                return false;
            }
        }
        if(!u2f_data_provisioning_finish()) return false;
    } else if(key_result != U2fDataLoadOk || counter_result != U2fDataLoadOk) {
        FURI_LOG_E(TAG, "Existing key/counter state is incomplete; refusing reset");
        return false;
    }

    if(mbedtls_ecp_group_load(&U2F->group, MBEDTLS_ECP_DP_SECP256R1) != 0) {
        FURI_LOG_E(TAG, "Unable to initialize the U2F elliptic-curve group");
        return false;
    }
    if(!u2f_attestation_key_matches_certificate(U2F)) return false;

    U2F->ready = true;
    return true;
}

void u2f_set_event_callback(U2fData* U2F, U2fEvtCallback callback, void* context) {
    furi_assert(U2F);
    furi_assert(callback);
    U2F->callback = callback;
    U2F->context = context;
}

static bool u2f_wait_for_user_presence(U2fData* U2F, U2fNotifyEvent event) {
    furi_mutex_acquire(U2F->presence_mutex, FuriWaitForever);
    if(!U2F->connected || U2F->shutting_down) {
        furi_mutex_release(U2F->presence_mutex);
        return false;
    }
    uint32_t now = furi_get_tick();
    bool approved = U2F->presence_approved &&
                    (uint32_t)(now - U2F->presence_approved_tick) <=
                        furi_ms_to_ticks(U2F_USER_PRESENCE_APPROVAL_MS);
    if(approved) {
        U2F->presence_approved = false;
        U2F->presence_pending = false;
    } else {
        U2F->presence_approved = false;
    }
    bool notify = !approved && !U2F->presence_pending;
    if(notify) U2F->presence_pending = true;
    furi_mutex_release(U2F->presence_mutex);

    if(notify && U2F->callback) U2F->callback(event, U2F->context);
    return approved;
}

void u2f_confirm_user_present(U2fData* U2F) {
    furi_assert(U2F);
    furi_mutex_acquire(U2F->presence_mutex, FuriWaitForever);
    bool pending = U2F->presence_pending;
    if(pending) {
        U2F->presence_pending = false;
        U2F->presence_approved = true;
        U2F->presence_approved_tick = furi_get_tick();
    }
    furi_mutex_release(U2F->presence_mutex);
}

void u2f_cancel_user_presence(U2fData* U2F) {
    furi_assert(U2F);
    furi_mutex_acquire(U2F->presence_mutex, FuriWaitForever);
    U2F->presence_pending = false;
    U2F->presence_approved = false;
    furi_mutex_release(U2F->presence_mutex);
}

void u2f_shutdown(U2fData* U2F) {
    furi_assert(U2F);
    furi_mutex_acquire(U2F->presence_mutex, FuriWaitForever);
    U2F->shutting_down = true;
    U2F->presence_pending = false;
    U2F->presence_approved = false;
    furi_mutex_release(U2F->presence_mutex);
}

static uint8_t u2f_der_encode_int(uint8_t* der, uint8_t* val, uint8_t val_len) {
    der[0] = 0x02; // Integer

    uint8_t len = 2;
    // Omit leading zeros, but keep one byte so a zero value never reads past the buffer
    while(val_len > 1 && val[0] == 0) {
        ++val;
        --val_len;
    }

    // Check if integer is negative
    if(val[0] > 0x7f) der[len++] = 0;

    memcpy(der + len, val, val_len);
    len += val_len;

    der[1] = len - 2;
    return len;
}

static uint8_t u2f_der_encode_signature(uint8_t* der, uint8_t* sig) {
    der[0] = 0x30;

    uint8_t len = 2;
    len += u2f_der_encode_int(der + len, sig, U2F_HASH_SIZE);
    len += u2f_der_encode_int(der + len, sig + U2F_HASH_SIZE, U2F_HASH_SIZE);

    der[1] = len - 2;
    return len;
}

static bool u2f_constant_time_equal(const uint8_t* left, const uint8_t* right, size_t size) {
    uint8_t difference = 0;
    for(size_t i = 0; i < size; i++) {
        difference |= left[i] ^ right[i];
    }
    return difference == 0;
}

static bool u2f_derive_key_handle(
    const uint8_t* device_key,
    const uint8_t* app_id,
    const uint8_t* nonce,
    uint8_t* private_key,
    uint8_t* key_handle_hash) {
    const mbedtls_md_info_t* md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_context_t hmac_ctx;
    mbedtls_md_init(&hmac_ctx);

    int result = md_info ? mbedtls_md_setup(&hmac_ctx, md_info, 1) : -1;
    if(result == 0) result = mbedtls_md_hmac_starts(&hmac_ctx, device_key, U2F_EC_KEY_SIZE);
    if(result == 0) result = mbedtls_md_hmac_update(&hmac_ctx, app_id, U2F_APP_ID_SIZE);
    if(result == 0) result = mbedtls_md_hmac_update(&hmac_ctx, nonce, U2F_NONCE_SIZE);
    if(result == 0) result = mbedtls_md_hmac_finish(&hmac_ctx, private_key);
    if(result == 0) result = mbedtls_md_hmac_reset(&hmac_ctx);
    if(result == 0) result = mbedtls_md_hmac_update(&hmac_ctx, private_key, U2F_EC_KEY_SIZE);
    if(result == 0) result = mbedtls_md_hmac_update(&hmac_ctx, app_id, U2F_APP_ID_SIZE);
    if(result == 0) result = mbedtls_md_hmac_finish(&hmac_ctx, key_handle_hash);
    mbedtls_md_free(&hmac_ctx);

    if(result != 0) {
        FURI_LOG_E(TAG, "Unable to derive U2F key handle: %d", result);
        mbedtls_platform_zeroize(private_key, U2F_EC_KEY_SIZE);
        mbedtls_platform_zeroize(key_handle_hash, U2F_HASH_SIZE);
        return false;
    }
    return true;
}

static bool u2f_sha256_parts(
    const uint8_t* const* parts,
    const size_t* part_sizes,
    size_t part_count,
    uint8_t hash[U2F_HASH_SIZE]) {
    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);

    int result = mbedtls_sha256_starts(&sha_ctx, 0);
    for(size_t i = 0; result == 0 && i < part_count; i++) {
        result = mbedtls_sha256_update(&sha_ctx, parts[i], part_sizes[i]);
    }
    if(result == 0) result = mbedtls_sha256_finish(&sha_ctx, hash);
    mbedtls_sha256_free(&sha_ctx);

    if(result != 0) {
        FURI_LOG_E(TAG, "Unable to hash U2F message: %d", result);
        mbedtls_platform_zeroize(hash, U2F_HASH_SIZE);
        return false;
    }
    return true;
}

static bool
    u2f_ecc_sign(mbedtls_ecp_group* grp, const uint8_t* key, uint8_t* hash, uint8_t* signature) {
    mbedtls_mpi r, s, d;
    int result;

    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);
    mbedtls_mpi_init(&d);

    memset(signature, 0, U2F_EC_BIGNUM_SIZE * 2);
    result = mbedtls_mpi_read_binary(&d, key, U2F_EC_KEY_SIZE);
    if(result == 0) {
        result = mbedtls_ecdsa_sign_det_ext(
            grp,
            &r,
            &s,
            &d,
            hash,
            U2F_HASH_SIZE,
            MBEDTLS_MD_SHA256,
            u2f_uecc_random_cb,
            NULL);
    }
    if(result == 0) result = mbedtls_mpi_write_binary(&r, signature, U2F_EC_BIGNUM_SIZE);
    if(result == 0) {
        result = mbedtls_mpi_write_binary(
            &s, signature + U2F_EC_BIGNUM_SIZE, U2F_EC_BIGNUM_SIZE);
    }

    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&d);
    if(result != 0) mbedtls_platform_zeroize(signature, U2F_EC_BIGNUM_SIZE * 2);
    return result == 0;
}

static bool u2f_ecc_compute_public_key(
    mbedtls_ecp_group* grp,
    const uint8_t* private_key,
    U2fPubKey* public_key) {
    mbedtls_ecp_point Q;
    mbedtls_mpi d;
    size_t olen;
    int result;

    mbedtls_ecp_point_init(&Q);
    mbedtls_mpi_init(&d);

    memset(public_key, 0, sizeof(*public_key));
    result = mbedtls_mpi_read_binary(&d, private_key, U2F_EC_KEY_SIZE);
    if(result == 0) result = mbedtls_ecp_check_privkey(grp, &d);
    if(result == 0) {
        result = mbedtls_ecp_mul(grp, &Q, &d, &grp->G, u2f_uecc_random_cb, NULL);
    }
    if(result == 0) {
        result = mbedtls_ecp_point_write_binary(
            grp,
            &Q,
            MBEDTLS_ECP_PF_UNCOMPRESSED,
            &olen,
            (unsigned char*)public_key,
            sizeof(U2fPubKey));
    }

    mbedtls_ecp_point_free(&Q);
    mbedtls_mpi_free(&d);
    if(result != 0) mbedtls_platform_zeroize(public_key, sizeof(*public_key));
    return result == 0;
}

///////////////////////////////////////////

static uint16_t u2f_register(U2fData* U2F, uint8_t* buf) {
    U2fRegisterReq* req = (U2fRegisterReq*)buf;
    U2fRegisterResp* resp = (U2fRegisterResp*)buf;
    U2fKeyHandle handle = {0};
    uint8_t private[U2F_EC_KEY_SIZE] = {0};
    U2fPubKey pub_key = {0};
    uint8_t hash[U2F_HASH_SIZE] = {0};
    uint8_t signature[U2F_EC_BIGNUM_SIZE * 2] = {0};

    if(u2f_data_check(false) == false) {
        FURI_LOG_E(TAG, "U2F data files are missing or unreadable (register); disabling");
        U2F->ready = false;
        if(U2F->callback != NULL) U2F->callback(U2fNotifyError, U2F->context);
        memcpy(&buf[0], state_not_supported, 2);
        return 2;
    }

    if(!u2f_wait_for_user_presence(U2F, U2fNotifyRegister)) {
        memcpy(&buf[0], state_user_missing, 2);
        return 2;
    }

    handle.len = U2F_HASH_SIZE * 2;

    // Generate random nonce
    furi_hal_random_fill_buf(handle.nonce, 32);

    if(!u2f_derive_key_handle(
           U2F->device_key, req->app_id, handle.nonce, private, handle.hash)) {
        goto registration_internal_error;
    }

    // Generate public key
    if(!u2f_ecc_compute_public_key(&U2F->group, private, &pub_key)) {
        FURI_LOG_E(TAG, "Unable to compute registration public key");
        goto registration_internal_error;
    }

    // Generate signature
    {
        uint8_t reserved_byte = 0;
        const uint8_t* parts[] = {
            &reserved_byte, req->app_id, req->challenge, handle.hash, (uint8_t*)&pub_key};
        const size_t part_sizes[] = {
            sizeof(reserved_byte), sizeof(req->app_id), sizeof(req->challenge), handle.len,
            sizeof(pub_key)};
        if(!u2f_sha256_parts(parts, part_sizes, COUNT_OF(parts), hash)) {
            goto registration_internal_error;
        }
    }

    // Sign hash
    if(!u2f_ecc_sign(&U2F->group, U2F->cert_key, hash, signature)) {
        FURI_LOG_E(TAG, "Unable to sign registration response");
        goto registration_internal_error;
    }

    // Encode response message
    resp->reserved = 0x05;
    memcpy(&(resp->pub_key), &pub_key, sizeof(U2fPubKey));
    memcpy(&(resp->key_handle), &handle, sizeof(U2fKeyHandle));
    uint32_t cert_len = u2f_data_cert_load(resp->cert);
    if(cert_len == 0 || cert_len > U2F_ATTESTATION_CERT_MAX_SIZE) {
        FURI_LOG_E(TAG, "Unable to load U2F attestation certificate");
        memcpy(buf, state_internal_error, sizeof(state_internal_error));
        if(U2F->callback) U2F->callback(U2fNotifyError, U2F->context);
        mbedtls_platform_zeroize(&handle, sizeof(handle));
        mbedtls_platform_zeroize(private, sizeof(private));
        mbedtls_platform_zeroize(&pub_key, sizeof(pub_key));
        mbedtls_platform_zeroize(hash, sizeof(hash));
        mbedtls_platform_zeroize(signature, sizeof(signature));
        return sizeof(state_internal_error);
    }
    uint8_t signature_len = u2f_der_encode_signature(resp->cert + cert_len, signature);
    memcpy(resp->cert + cert_len + signature_len, state_no_error, 2);

    if(U2F->callback) U2F->callback(U2fNotifyRegisterSuccess, U2F->context);
    mbedtls_platform_zeroize(&handle, sizeof(handle));
    mbedtls_platform_zeroize(private, sizeof(private));
    mbedtls_platform_zeroize(&pub_key, sizeof(pub_key));
    mbedtls_platform_zeroize(hash, sizeof(hash));
    mbedtls_platform_zeroize(signature, sizeof(signature));
    return sizeof(U2fRegisterResp) + cert_len + signature_len + 2;

registration_internal_error:
    mbedtls_platform_zeroize(&handle, sizeof(handle));
    mbedtls_platform_zeroize(private, sizeof(private));
    mbedtls_platform_zeroize(&pub_key, sizeof(pub_key));
    mbedtls_platform_zeroize(hash, sizeof(hash));
    mbedtls_platform_zeroize(signature, sizeof(signature));
    memcpy(buf, state_internal_error, sizeof(state_internal_error));
    if(U2F->callback) U2F->callback(U2fNotifyError, U2F->context);
    return sizeof(state_internal_error);
}

static inline uint32_t u2f_to_big_endian(uint32_t a) {
    return __builtin_bswap32(a);
}

static uint16_t u2f_authenticate(U2fData* U2F, uint8_t* buf) {
    U2fAuthReq* req = (U2fAuthReq*)buf;
    U2fAuthResp* resp = (U2fAuthResp*)buf;
    uint8_t priv_key[U2F_EC_KEY_SIZE] = {0};
    uint8_t mac_control[32] = {0};
    uint8_t flags = 0;
    uint8_t hash[U2F_HASH_SIZE] = {0};
    uint8_t signature[U2F_HASH_SIZE * 2] = {0};
    uint32_t be_u2f_counter;

    if(u2f_data_check(false) == false) {
        FURI_LOG_E(TAG, "U2F data files are missing or unreadable (authenticate); disabling");
        U2F->ready = false;
        if(U2F->callback != NULL) U2F->callback(U2fNotifyError, U2F->context);
        memcpy(&buf[0], state_not_supported, 2);
        return 2;
    }

    if(!u2f_derive_key_handle(
           U2F->device_key, req->app_id, req->key_handle.nonce, priv_key, mac_control)) {
        mbedtls_platform_zeroize(priv_key, sizeof(priv_key));
        memcpy(buf, state_internal_error, sizeof(state_internal_error));
        if(U2F->callback) U2F->callback(U2fNotifyError, U2F->context);
        return sizeof(state_internal_error);
    }

    if(!u2f_constant_time_equal(req->key_handle.hash, mac_control, sizeof(mac_control))) {
        FURI_LOG_W(TAG, "Wrong handle!");
        mbedtls_platform_zeroize(priv_key, sizeof(priv_key));
        mbedtls_platform_zeroize(mac_control, sizeof(mac_control));
        memcpy(&buf[0], state_wrong_data, 2);
        return 2;
    }
    mbedtls_platform_zeroize(mac_control, sizeof(mac_control));

    if(req->p1 == U2fCheckOnly) {
        mbedtls_platform_zeroize(priv_key, sizeof(priv_key));
        memcpy(&buf[0], state_user_missing, 2);
        return 2;
    }
    if(req->p1 == U2fEnforce) {
        if(!u2f_wait_for_user_presence(U2F, U2fNotifyAuth)) {
            mbedtls_platform_zeroize(priv_key, sizeof(priv_key));
            memcpy(&buf[0], state_user_missing, 2);
            return 2;
        }
        flags = 1;
    } else if(req->p1 != U2fDontEnforce) {
        mbedtls_platform_zeroize(priv_key, sizeof(priv_key));
        memcpy(&buf[0], state_wrong_data, 2);
        return 2;
    }

    if(U2F->counter == UINT32_MAX) {
        FURI_LOG_E(TAG, "U2F signature counter exhausted");
        mbedtls_platform_zeroize(priv_key, sizeof(priv_key));
        memcpy(&buf[0], state_internal_error, sizeof(state_internal_error));
        if(U2F->callback) U2F->callback(U2fNotifyError, U2F->context);
        return 2;
    }

    // The 4 byte counter is represented in big endian. Increment it before use.
    uint32_t next_counter = U2F->counter + 1;
    be_u2f_counter = u2f_to_big_endian(next_counter);

    // Generate hash.
    {
        const uint8_t* parts[] = {
            req->app_id, &flags, (uint8_t*)&be_u2f_counter, req->challenge};
        const size_t part_sizes[] = {
            sizeof(req->app_id), sizeof(flags), sizeof(be_u2f_counter), sizeof(req->challenge)};
        if(!u2f_sha256_parts(parts, part_sizes, COUNT_OF(parts), hash)) {
            mbedtls_platform_zeroize(priv_key, sizeof(priv_key));
            memcpy(buf, state_internal_error, sizeof(state_internal_error));
            if(U2F->callback) U2F->callback(U2fNotifyError, U2F->context);
            return sizeof(state_internal_error);
        }
    }

    // Sign hash
    if(!u2f_ecc_sign(&U2F->group, priv_key, hash, signature)) {
        FURI_LOG_E(TAG, "Unable to sign authentication response");
        mbedtls_platform_zeroize(priv_key, sizeof(priv_key));
        mbedtls_platform_zeroize(mac_control, sizeof(mac_control));
        mbedtls_platform_zeroize(hash, sizeof(hash));
        mbedtls_platform_zeroize(signature, sizeof(signature));
        memcpy(&buf[0], state_internal_error, sizeof(state_internal_error));
        if(U2F->callback) U2F->callback(U2fNotifyError, U2F->context);
        return sizeof(state_internal_error);
    }

    if(!u2f_data_cnt_write(next_counter)) {
        FURI_LOG_E(TAG, "Unable to persist U2F signature counter");
        mbedtls_platform_zeroize(priv_key, sizeof(priv_key));
        mbedtls_platform_zeroize(hash, sizeof(hash));
        mbedtls_platform_zeroize(signature, sizeof(signature));
        memcpy(&buf[0], state_internal_error, sizeof(state_internal_error));
        if(U2F->callback) U2F->callback(U2fNotifyError, U2F->context);
        return 2;
    }

    mbedtls_platform_zeroize(priv_key, sizeof(priv_key));
    resp->user_present = flags;
    resp->counter = be_u2f_counter;
    uint8_t signature_len = u2f_der_encode_signature(resp->signature, signature);
    memcpy(resp->signature + signature_len, state_no_error, 2);

    U2F->counter = next_counter;
    FURI_LOG_D(TAG, "Counter: %lu", U2F->counter);

    if(U2F->callback != NULL) U2F->callback(U2fNotifyAuthSuccess, U2F->context);

    mbedtls_platform_zeroize(hash, sizeof(hash));
    mbedtls_platform_zeroize(signature, sizeof(signature));
    return sizeof(U2fAuthResp) + signature_len + 2;
}

uint16_t u2f_msg_parse(U2fData* U2F, uint8_t* buf, uint16_t len) {
    furi_assert(U2F);
    if(!buf || len < 2) return 0;
    if(!U2F->ready) {
        // Answer with an ISO 7816 status instead of 0 (which the HID layer reports as an
        // unrelated CTAPHID ERR_INVALID_CMD) so the host sees why the token is unusable.
        FURI_LOG_E(TAG, "Authenticator is disabled after a data error; restart the U2F app");
        memcpy(buf, state_internal_error, sizeof(state_internal_error));
        return sizeof(state_internal_error);
    }
    // P1/P2 are deliberately not validated for REGISTER: the U2F raw message spec does not
    // define them there and Chrome sends P1=0x03 (|0x80 for individual attestation).
    // AUTHENTICATE validates P1 (control byte) in u2f_authenticate(); P2 is unused.
    if(buf[1] == U2F_CMD_REGISTER) { // Register request
        if((len != sizeof(U2fRegisterReq) && len != sizeof(U2fRegisterReq) + U2F_APDU_LE_SIZE) ||
           buf[0] != 0 || buf[4] != 0 || buf[5] != 0 || buf[6] != U2F_CHALLENGE_SIZE * 2) {
            memcpy(buf, state_wrong_data, sizeof(state_wrong_data));
            return sizeof(state_wrong_data);
        }
        return u2f_register(U2F, buf);

    } else if(buf[1] == U2F_CMD_AUTHENTICATE) { // Authenticate request
        if((len != sizeof(U2fAuthReq) && len != sizeof(U2fAuthReq) + U2F_APDU_LE_SIZE) ||
           buf[0] != 0 || buf[4] != 0 || buf[5] != 0 ||
           buf[6] != (U2F_CHALLENGE_SIZE * 2 + 1 + U2F_HASH_SIZE * 2) ||
           ((U2fAuthReq*)buf)->key_handle.len != U2F_HASH_SIZE * 2) {
            memcpy(buf, state_wrong_data, sizeof(state_wrong_data));
            return sizeof(state_wrong_data);
        }
        return u2f_authenticate(U2F, buf);

    } else if(buf[1] == U2F_CMD_VERSION) { // Get U2F version string
        if((len < 4 || len > 7) || buf[0] != 0) {
            memcpy(buf, state_wrong_data, sizeof(state_wrong_data));
            return sizeof(state_wrong_data);
        }
        memcpy(&buf[0], ver_str, 6);
        memcpy(&buf[6], state_no_error, 2);
        return 8;
    } else {
        memcpy(&buf[0], state_not_supported, 2);
        return 2;
    }
    return 0;
}

void u2f_wink(U2fData* U2F) {
    if(U2F->callback != NULL) U2F->callback(U2fNotifyWink, U2F->context);
}

void u2f_set_state(U2fData* U2F, uint8_t state) {
    furi_mutex_acquire(U2F->presence_mutex, FuriWaitForever);
    U2F->connected = (state != 0);
    furi_mutex_release(U2F->presence_mutex);

    if(state == 0) {
        u2f_cancel_user_presence(U2F);
        if(U2F->callback != NULL) U2F->callback(U2fNotifyDisconnect, U2F->context);
    } else {
        if(U2F->callback != NULL) U2F->callback(U2fNotifyConnect, U2F->context);
    }
}
