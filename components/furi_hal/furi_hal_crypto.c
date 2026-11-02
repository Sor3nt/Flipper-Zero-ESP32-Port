#include "furi_hal_crypto.h"

#include <string.h>

#include <core/common_defines.h>
#include <esp_err.h>
#include <furi.h>
#include <furi_hal_random.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#include <esp_mac.h>
#include <esp_partition.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/sha256.h>
#include <nvs.h>
#include <nvs_flash.h>

#define TAG "FuriHalCrypto"

#define U2F_STORAGE_KEY_SIZE 32
#define U2F_GCM_TAG_SIZE 16
#define U2F_NVS_PARTITION "u2fkeys"
#define U2F_NVS_NAMESPACE "u2f"
#define U2F_NVS_KEY_NAME "storage_key"

#define U2F_DERIVE_LABEL "FlipperESP32-U2F-storage-key-v1"

static uint8_t s_u2f_storage_key[U2F_STORAGE_KEY_SIZE];
static bool s_u2f_storage_key_ready;
static FuriMutex* s_u2f_storage_mutex;

/* Deterministic per-chip key: HMAC-SHA256(eFuse unique ID || base MAC, label).
 * It can be recomputed at any time, so erasing/relocating the u2fkeys NVS
 * partition or reflashing the device can no longer make U2F data undecryptable.
 * Like the NVS key it is not protected against someone with physical access
 * unless ESP32 flash encryption / eFuse protection is enabled. */
static bool furi_hal_crypto_u2f_derive_key(uint8_t* key) {
    uint8_t material[16 + 6] = {0};
#ifdef ESP_EFUSE_OPTIONAL_UNIQUE_ID
    if(esp_efuse_read_field_blob(ESP_EFUSE_OPTIONAL_UNIQUE_ID, material, 16 * 8) != ESP_OK) {
        FURI_LOG_E(TAG, "Unable to read the chip unique ID");
        return false;
    }
#endif
    if(esp_efuse_mac_get_default(material + 16) != ESP_OK) {
        FURI_LOG_E(TAG, "Unable to read the chip MAC address");
        return false;
    }
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    int result = md ? mbedtls_md_hmac(
                          md,
                          material,
                          sizeof(material),
                          (const uint8_t*)U2F_DERIVE_LABEL,
                          sizeof(U2F_DERIVE_LABEL) - 1,
                          key) :
                      -1;
    mbedtls_platform_zeroize(material, sizeof(material));
    if(result != 0) {
        FURI_LOG_E(TAG, "Storage key derivation failed: -0x%04x", -result);
        return false;
    }
    return true;
}

static void furi_hal_crypto_u2f_log_key(const char* source) {
    uint8_t digest[32];
    if(mbedtls_sha256(s_u2f_storage_key, sizeof(s_u2f_storage_key), digest, 0) == 0) {
        FURI_LOG_I(
            TAG,
            "U2F storage key source=%s id=%02x%02x%02x%02x",
            source,
            digest[0],
            digest[1],
            digest[2],
            digest[3]);
    }
    mbedtls_platform_zeroize(digest, sizeof(digest));
}

static bool furi_hal_crypto_u2f_partition_init(void) {
    esp_err_t err = nvs_flash_init_partition(U2F_NVS_PARTITION);
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        FURI_LOG_E(TAG, "U2F key partition initialization failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool furi_hal_crypto_u2f_partition_open(nvs_handle_t* nvs, nvs_open_mode_t mode) {
    if(!furi_hal_crypto_u2f_partition_init()) return false;
    esp_err_t err = nvs_open_from_partition(
        U2F_NVS_PARTITION, U2F_NVS_NAMESPACE, mode, nvs);
    if(err != ESP_OK) {
        FURI_LOG_E(TAG, "Unable to open U2F key partition: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool furi_hal_crypto_u2f_legacy_key_read(uint8_t* key, bool* found) {
    *found = false;
    nvs_handle_t legacy_nvs;
    esp_err_t err = nvs_open(U2F_NVS_NAMESPACE, NVS_READONLY, &legacy_nvs);
    if(err == ESP_ERR_NVS_NOT_FOUND) return true;
    if(err != ESP_OK) {
        FURI_LOG_E(TAG, "Unable to open legacy U2F key namespace: %s", esp_err_to_name(err));
        return false;
    }

    size_t key_size = U2F_STORAGE_KEY_SIZE;
    err = nvs_get_blob(legacy_nvs, U2F_NVS_KEY_NAME, key, &key_size);
    nvs_close(legacy_nvs);
    if(err == ESP_ERR_NVS_NOT_FOUND) return true;
    if(err != ESP_OK || key_size != U2F_STORAGE_KEY_SIZE) {
        FURI_LOG_E(TAG, "Legacy U2F storage key is unreadable or has an invalid size");
        return false;
    }
    *found = true;
    return true;
}

static void furi_hal_crypto_u2f_legacy_key_remove(void) {
    nvs_handle_t legacy_nvs;
    esp_err_t err = nvs_open(U2F_NVS_NAMESPACE, NVS_READWRITE, &legacy_nvs);
    if(err != ESP_OK) {
        FURI_LOG_W(TAG, "Could not open default NVS to remove the migrated U2F key");
        return;
    }
    err = nvs_erase_key(legacy_nvs, U2F_NVS_KEY_NAME);
    if(err == ESP_OK) err = nvs_commit(legacy_nvs);
    nvs_close(legacy_nvs);
    if(err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        FURI_LOG_W(TAG, "Could not remove migrated U2F key from default NVS: %s",
                   esp_err_to_name(err));
    }
}

static bool furi_hal_crypto_u2f_key_migrate_locked(nvs_handle_t nvs) {
    size_t key_size = 0;
    esp_err_t err = nvs_get_blob(nvs, U2F_NVS_KEY_NAME, NULL, &key_size);
    if(err == ESP_OK) {
        if(key_size != U2F_STORAGE_KEY_SIZE) {
            FURI_LOG_E(TAG, "U2F storage key in u2fkeys has an invalid size");
            return false;
        }
        return true;
    }
    if(err != ESP_ERR_NVS_NOT_FOUND) {
        FURI_LOG_E(TAG, "Unable to check U2F key partition: %s", esp_err_to_name(err));
        return false;
    }

    uint8_t legacy_key[U2F_STORAGE_KEY_SIZE] = {0};
    bool legacy_found = false;
    if(!furi_hal_crypto_u2f_legacy_key_read(legacy_key, &legacy_found)) {
        mbedtls_platform_zeroize(legacy_key, sizeof(legacy_key));
        return false;
    }
    if(legacy_found) {
        err = nvs_set_blob(nvs, U2F_NVS_KEY_NAME, legacy_key, sizeof(legacy_key));
        if(err == ESP_OK) err = nvs_commit(nvs);
        if(err == ESP_OK) {
            FURI_LOG_I(TAG, "Migrated existing U2F storage key to u2fkeys");
            furi_hal_crypto_u2f_legacy_key_remove();
        } else {
            FURI_LOG_E(TAG, "Unable to persist migrated U2F storage key: %s", esp_err_to_name(err));
        }
    }
    mbedtls_platform_zeroize(legacy_key, sizeof(legacy_key));
    return !legacy_found || err == ESP_OK;
}

bool furi_hal_crypto_u2f_storage_key_migrate(void) {
    if(!s_u2f_storage_mutex) {
        FURI_LOG_E(TAG, "U2F storage mutex is not initialized");
        return false;
    }
    if(furi_mutex_acquire(s_u2f_storage_mutex, FuriWaitForever) != FuriStatusOk) {
        FURI_LOG_E(TAG, "Unable to acquire U2F storage mutex for migration");
        return false;
    }

    nvs_handle_t nvs;
    bool migrated = furi_hal_crypto_u2f_partition_open(&nvs, NVS_READWRITE);
    if(migrated) {
        migrated = furi_hal_crypto_u2f_key_migrate_locked(nvs);
        nvs_close(nvs);
    }

    furi_mutex_release(s_u2f_storage_mutex);
    return migrated;
}

static bool furi_hal_crypto_u2f_storage_key_load_locked(bool create_if_missing) {
    if(s_u2f_storage_key_ready) return true;

    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, U2F_NVS_PARTITION);
    if(!part) {
        /* Flashed with a partition table that lacks u2fkeys: the derived key keeps working. */
        FURI_LOG_W(TAG, "Partition '%s' not found; using the derived U2F storage key", U2F_NVS_PARTITION);
        if(!furi_hal_crypto_u2f_derive_key(s_u2f_storage_key)) return false;
        s_u2f_storage_key_ready = true;
        furi_hal_crypto_u2f_log_key("derived/no-partition");
        return true;
    }

    nvs_handle_t nvs;
    if(!furi_hal_crypto_u2f_partition_open(&nvs, NVS_READWRITE)) return false;
    if(!furi_hal_crypto_u2f_key_migrate_locked(nvs)) {
        nvs_close(nvs);
        return false;
    }

    const char* source = "nvs";
    size_t key_size = sizeof(s_u2f_storage_key);
    esp_err_t err = nvs_get_blob(nvs, U2F_NVS_KEY_NAME, s_u2f_storage_key, &key_size);
    if(err == ESP_ERR_NVS_NOT_FOUND) {
        /* No stored key: use the recomputable one. When creating, also store it so the
         * NVS copy and the derived key stay identical. Never generate a random key here:
         * a random key lost with the partition made all existing U2F files unreadable. */
        if(!furi_hal_crypto_u2f_derive_key(s_u2f_storage_key)) {
            nvs_close(nvs);
            return false;
        }
        source = "derived";
        key_size = sizeof(s_u2f_storage_key);
        err = ESP_OK;
        if(create_if_missing) {
            esp_err_t store_err =
                nvs_set_blob(nvs, U2F_NVS_KEY_NAME, s_u2f_storage_key, sizeof(s_u2f_storage_key));
            if(store_err == ESP_OK) store_err = nvs_commit(nvs);
            if(store_err != ESP_OK) {
                FURI_LOG_W(
                    TAG,
                    "Could not cache the derived U2F key in NVS (%s); continuing",
                    esp_err_to_name(store_err));
            }
            source = "derived(new)";
        } else {
            FURI_LOG_W(
                TAG,
                "No U2F key in '%s' (partition @0x%x); using derived key. Data encrypted with a lost random key cannot be recovered",
                U2F_NVS_PARTITION,
                (unsigned)part->address);
        }
    }
    nvs_close(nvs);
    if(err != ESP_OK || key_size != sizeof(s_u2f_storage_key)) {
        mbedtls_platform_zeroize(s_u2f_storage_key, sizeof(s_u2f_storage_key));
        FURI_LOG_E(TAG, "Unable to load U2F storage key: %s", esp_err_to_name(err));
        return false;
    }

    s_u2f_storage_key_ready = true;
    furi_hal_crypto_u2f_log_key(source);
    return true;
}

static bool furi_hal_crypto_u2f_storage_key_get(bool create_if_missing) {
    if(!s_u2f_storage_mutex) {
        FURI_LOG_E(TAG, "U2F storage mutex is not initialized");
        return false;
    }

    if(furi_mutex_acquire(s_u2f_storage_mutex, FuriWaitForever) != FuriStatusOk) {
        FURI_LOG_E(TAG, "Unable to acquire U2F storage mutex");
        return false;
    }
    bool ready = furi_hal_crypto_u2f_storage_key_load_locked(create_if_missing);
    furi_mutex_release(s_u2f_storage_mutex);
    return ready;
}

void furi_hal_crypto_init(void) {
    if(!s_u2f_storage_mutex) {
        s_u2f_storage_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    }
    if(!s_u2f_storage_mutex) {
        FURI_LOG_E(TAG, "Unable to allocate U2F storage mutex");
    }
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

/* The generic AES API (load_key / enclave_* / encrypt / decrypt) is a pass-through on
 * this port: it reports success and copies the data unchanged. Callers (the Sub-GHz
 * keystore, the crypto CLI, external apps such as an authenticator) therefore keep
 * their "encrypted" data in the clear. Only the U2F storage below uses real AES-GCM.
 * Making it real would change the on-disk format of everything already saved this way,
 * so that is a deliberate decision, not a silent fix: until then say so once at run
 * time instead of letting the failure stay invisible. */
static void furi_hal_crypto_warn_pass_through(void) {
    static bool warned = false;
    if(warned) return;
    warned = true;
    FURI_LOG_W(
        TAG,
        "furi_hal_crypto_encrypt/decrypt are pass-through on this port: data is NOT encrypted");
}

bool furi_hal_crypto_encrypt(const uint8_t* input, uint8_t* output, size_t size) {
    if(!input || !output) {
        return false;
    }

    furi_hal_crypto_warn_pass_through();
    if(output != input) memmove(output, input, size);
    return true;
}

bool furi_hal_crypto_decrypt(const uint8_t* input, uint8_t* output, size_t size) {
    if(!input || !output) {
        return false;
    }

    furi_hal_crypto_warn_pass_through();
    if(output != input) memmove(output, input, size);
    return true;
}

bool furi_hal_crypto_u2f_encrypt(
    const uint8_t* plaintext,
    uint8_t* ciphertext_and_tag,
    size_t size,
    const uint8_t nonce[16]) {
    if(!plaintext || !ciphertext_and_tag || !nonce ||
       !furi_hal_crypto_u2f_storage_key_get(true)) {
        return false;
    }

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int result = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, s_u2f_storage_key, 256);
    if(result == 0) {
        result = mbedtls_gcm_crypt_and_tag(
            &gcm,
            MBEDTLS_GCM_ENCRYPT,
            size,
            nonce,
            16,
            NULL,
            0,
            plaintext,
            ciphertext_and_tag,
            U2F_GCM_TAG_SIZE,
            ciphertext_and_tag + size);
    }
    mbedtls_gcm_free(&gcm);
    if(result != 0) {
        mbedtls_platform_zeroize(ciphertext_and_tag, size + U2F_GCM_TAG_SIZE);
    }
    return result == 0;
}

bool furi_hal_crypto_u2f_decrypt(
    const uint8_t* ciphertext_and_tag,
    uint8_t* plaintext,
    size_t size,
    const uint8_t nonce[16]) {
    if(!ciphertext_and_tag || !plaintext || !nonce ||
       !furi_hal_crypto_u2f_storage_key_get(false)) {
        return false;
    }

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int result = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, s_u2f_storage_key, 256);
    if(result == 0) {
        result = mbedtls_gcm_auth_decrypt(
            &gcm,
            size,
            nonce,
            16,
            NULL,
            0,
            ciphertext_and_tag + size,
            U2F_GCM_TAG_SIZE,
            ciphertext_and_tag,
            plaintext);
    }
    mbedtls_gcm_free(&gcm);
    if(result != 0) {
        FURI_LOG_E(TAG, "U2F data authentication failed (mbedtls -0x%04x); wrong storage key", -result);
        mbedtls_platform_zeroize(plaintext, size);
    }
    return result == 0;
}
