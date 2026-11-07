#include <furi.h>
#include "u2f_data.h"
#include <furi_hal.h>
#include <storage/storage.h>
#include <furi_hal_random.h>
#include <flipper_format/flipper_format.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/pk.h>
#include <mbedtls/ecp.h>
#include <mbedtls/x509_crt.h>
#include "u2f_crypto.h"

#define TAG "U2f"

#define U2F_DATA_FOLDER   EXT_PATH("u2f/")
#define U2F_CERT_FILE     U2F_DATA_FOLDER "assets/cert.der"
#define U2F_CERT_KEY_FILE U2F_DATA_FOLDER "assets/cert_key.u2f"
#define U2F_CERT_KEY_TEMP_FILE U2F_CERT_KEY_FILE ".tmp"
#define U2F_KEY_FILE      U2F_DATA_FOLDER "key.u2f"
#define U2F_KEY_TEMP_FILE U2F_KEY_FILE ".tmp"
#define U2F_CNT_FILE      U2F_DATA_FOLDER "cnt.u2f"
#define U2F_CNT_FILE_B    U2F_CNT_FILE ".b"
#define U2F_PROVISION_FILE U2F_DATA_FOLDER ".provisioning"

#define U2F_CERT_STOCK 0 // Stock certificate, private key is encrypted with factory key
#define U2F_CERT_USER  1 // User certificate, private key is encrypted with unique key
#define U2F_CERT_USER_UNENCRYPTED \
    2 // Unencrypted user certificate, will be encrypted after first load

#define U2F_CERT_KEY_FILE_TYPE "Flipper U2F Certificate Key File"
#define U2F_CERT_KEY_VERSION   2
#define U2F_CERT_KEY_VERSION_PLAINTEXT 1

#define U2F_DEVICE_KEY_FILE_TYPE "Flipper U2F Device Key File"
#define U2F_DEVICE_KEY_VERSION   2

#define U2F_COUNTER_FILE_TYPE   "Flipper U2F Counter File"
#define U2F_COUNTER_VERSION     3

#define U2F_COUNTER_CONTROL_VAL 0xAA5500FF
#define U2F_CERT_MAX_SIZE       2048
#define U2F_DATA_KEY_SIZE       32

typedef struct {
    uint32_t counter;
    uint8_t random_salt[24];
    uint32_t control;
} FURI_PACKED U2fCounterData;

static bool u2f_data_sync_file(Storage* storage, const char* path) {
    File* file = storage_file_alloc(storage);
    bool synced = storage_file_open(file, path, FSAM_READ_WRITE, FSOM_OPEN_EXISTING);
    if(synced) synced = storage_file_sync(file);
    bool closed = storage_file_close(file);
    storage_file_free(file);
    return synced && closed;
}

static bool u2f_data_promote_temp(Storage* storage, const char* temp_path, const char* path) {
    if(storage_common_stat(storage, path, NULL) != FSE_NOT_EXIST) {
        FURI_LOG_E(TAG, "Refusing to replace existing persistent file %s", path);
        return false;
    }
    FS_Error error = storage_common_rename(storage, temp_path, path);
    if(error != FSE_OK) {
        FURI_LOG_E(TAG, "Unable to promote %s to %s: %d", temp_path, path, error);
        return false;
    }
    return true;
}

bool u2f_data_check(bool cert_only) {
    bool state = false;
    Storage* fs_api = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(fs_api);

    do {
        if(!storage_file_open(file, U2F_CERT_FILE, FSAM_READ, FSOM_OPEN_EXISTING)) break;
        storage_file_close(file);
        // cert_key.u2f can be absent while a verified cert_key.u2f.tmp awaits promotion (power
        // loss between remove and rename). Accept it so u2f_data_cert_key_load() can recover it
        // instead of the app reporting missing files and never reaching that code.
        if(storage_common_stat(fs_api, U2F_CERT_KEY_FILE, NULL) != FSE_OK &&
           storage_common_stat(fs_api, U2F_CERT_KEY_TEMP_FILE, NULL) != FSE_OK) {
            break;
        }
        if(cert_only) {
            state = true;
            break;
        }
        if(!storage_file_open(file, U2F_KEY_FILE, FSAM_READ, FSOM_OPEN_EXISTING)) break;
        storage_file_close(file);
        if(storage_common_stat(fs_api, U2F_CNT_FILE, NULL) != FSE_OK &&
           storage_common_stat(fs_api, U2F_CNT_FILE_B, NULL) != FSE_OK) {
            break;
        }
        state = true;
    } while(0);

    storage_file_close(file);
    storage_file_free(file);

    furi_record_close(RECORD_STORAGE);

    return state;
}

bool u2f_data_cert_check(void) {
    bool state = false;
    Storage* fs_api = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(fs_api);
    uint8_t file_buf[8];

    if(storage_file_open(file, U2F_CERT_FILE, FSAM_READ, FSOM_OPEN_EXISTING)) {
        do {
            // Read header to check certificate size
            size_t file_size = storage_file_size(file);
            size_t len_cur = storage_file_read(file, file_buf, 4);
            if(len_cur != 4 || file_size < 4) break;

            if(file_buf[0] != 0x30) {
                FURI_LOG_E(TAG, "Wrong certificate header");
                break;
            }

            size_t header_size;
            size_t content_size;
            if(file_buf[1] < 0x80) {
                header_size = 2;
                content_size = file_buf[1];
            } else if(file_buf[1] == 0x81) {
                header_size = 3;
                content_size = file_buf[2];
            } else if(file_buf[1] == 0x82) {
                header_size = 4;
                content_size = ((size_t)file_buf[2] << 8) | file_buf[3];
            } else {
                FURI_LOG_E(TAG, "Unsupported or invalid certificate DER length");
                break;
            }
            size_t temp_len = header_size + content_size;
            if(temp_len != file_size || file_size > U2F_CERT_MAX_SIZE) {
                FURI_LOG_E(TAG, "Wrong certificate length");
                break;
            }
            state = true;
        } while(0);
    }

    storage_file_close(file);
    storage_file_free(file);

    furi_record_close(RECORD_STORAGE);

    return state;
}

uint32_t u2f_data_cert_load(uint8_t* cert) {
    furi_assert(cert);

    Storage* fs_api = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(fs_api);
    uint32_t file_size = 0;
    uint32_t len_cur = 0;

    if(storage_file_open(file, U2F_CERT_FILE, FSAM_READ, FSOM_OPEN_EXISTING)) {
        file_size = storage_file_size(file);
        if(file_size > U2F_CERT_MAX_SIZE) {
            FURI_LOG_E(TAG, "Certificate exceeds the supported size");
            file_size = 0;
        }
        len_cur = storage_file_read(file, cert, file_size);
        if(len_cur != file_size) len_cur = 0;
    }

    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);

    return len_cur;
}

bool u2f_data_cert_key_check(void) {
    Storage* storage = furi_record_open(RECORD_STORAGE);

    const char* path = U2F_CERT_KEY_FILE;
    FS_Error st = storage_common_stat(storage, U2F_CERT_KEY_FILE, NULL);
    if(st == FSE_NOT_EXIST) {
        if(storage_common_stat(storage, U2F_CERT_KEY_TEMP_FILE, NULL) == FSE_OK) {
            path = U2F_CERT_KEY_TEMP_FILE;
        } else {
            furi_record_close(RECORD_STORAGE);
            return false;
        }
    } else if(st != FSE_OK) {
        furi_record_close(RECORD_STORAGE);
        return false;
    }

    bool usable = false;
    FuriString* filetype = furi_string_alloc();
    FlipperFormat* ff = flipper_format_file_alloc(storage);
    uint32_t version = 0;
    uint32_t cert_type = 0xFFFFFFFF;
    if(flipper_format_file_open_existing(ff, path) &&
       flipper_format_read_header(ff, filetype, &version) &&
       strcmp(furi_string_get_cstr(filetype), U2F_CERT_KEY_FILE_TYPE) == 0 &&
       flipper_format_read_uint32(ff, "Type", &cert_type, 1)) {
        usable = cert_type == U2F_CERT_USER || cert_type == U2F_CERT_USER_UNENCRYPTED;
    }
    flipper_format_free(ff);
    furi_string_free(filetype);
    furi_record_close(RECORD_STORAGE);
    return usable;
}

static int u2f_data_rng_cb(void* ctx, unsigned char* buf, size_t len) {
    UNUSED(ctx);
    furi_hal_random_fill_buf(buf, (uint32_t)len);
    return 0;
}

bool u2f_data_cert_generate(void) {
    Storage* storage = furi_record_open(RECORD_STORAGE);

    // Explicit user action: replace any leftover/broken attestation material with
    // a fresh self-signed pair. The caller only reaches this when no usable
    // certificate is present, so clearing stale files here is the recovery path.
    storage_common_remove(storage, U2F_CERT_FILE);
    storage_common_remove(storage, U2F_CERT_KEY_FILE);
    storage_common_remove(storage, U2F_CERT_KEY_TEMP_FILE);

    FURI_LOG_I(TAG, "Generating self-signed U2F attestation certificate");

    bool state = false;
    mbedtls_pk_context key;
    mbedtls_x509write_cert crt;
    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&crt);

    uint8_t priv[U2F_DATA_KEY_SIZE] = {0};
    uint8_t serial[16] = {0};
    // mbedtls_x509write_crt_der() writes to the END of this buffer and returns
    // the length; the DER then starts at der + U2F_CERT_MAX_SIZE - der_len.
    uint8_t* der = malloc(U2F_CERT_MAX_SIZE);
    if(!der) {
        FURI_LOG_E(TAG, "Out of memory generating attestation certificate");
    } else {
        do {
            const mbedtls_pk_info_t* info = mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY);
            if(!info || mbedtls_pk_setup(&key, info) != 0) break;
            if(mbedtls_ecp_gen_key(
                   MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key), u2f_data_rng_cb, NULL) != 0)
                break;

            size_t priv_len = 0;
            if(mbedtls_ecp_write_key_ext(mbedtls_pk_ec(key), &priv_len, priv, sizeof(priv)) != 0 ||
               priv_len != sizeof(priv))
                break;

            mbedtls_x509write_crt_set_subject_key(&crt, &key);
            mbedtls_x509write_crt_set_issuer_key(&crt, &key); // self-signed
            mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
            mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
            if(mbedtls_x509write_crt_set_subject_name(
                   &crt, "CN=ESP32 U2F Token,O=flipper-esp32") != 0)
                break;
            if(mbedtls_x509write_crt_set_issuer_name(
                   &crt, "CN=ESP32 U2F Token,O=flipper-esp32") != 0)
                break;
            furi_hal_random_fill_buf(serial, sizeof(serial));
            serial[0] &= 0x7f; // keep the serial a positive DER integer
            if(serial[0] == 0) serial[0] = 1;
            if(mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial)) != 0) break;
            if(mbedtls_x509write_crt_set_validity(&crt, "20240101000000", "20440101000000") != 0)
                break;

            int der_len =
                mbedtls_x509write_crt_der(&crt, der, U2F_CERT_MAX_SIZE, u2f_data_rng_cb, NULL);
            if(der_len <= 0) {
                FURI_LOG_E(TAG, "Attestation certificate encoding failed: %d", der_len);
                break;
            }
            uint8_t* der_start = der + U2F_CERT_MAX_SIZE - der_len;

            storage_common_mkdir(storage, U2F_DATA_FOLDER);
            storage_common_mkdir(storage, EXT_PATH("u2f/assets"));

            File* file = storage_file_alloc(storage);
            bool cert_written =
                storage_file_open(file, U2F_CERT_FILE, FSAM_WRITE, FSOM_CREATE_ALWAYS) &&
                storage_file_write(file, der_start, der_len) == (size_t)der_len &&
                storage_file_sync(file);
            storage_file_close(file);
            storage_file_free(file);
            if(!cert_written) {
                FURI_LOG_E(TAG, "Unable to write attestation certificate");
                storage_common_remove(storage, U2F_CERT_FILE);
                break;
            }

            // Plaintext Type-2 key file; the firmware encrypts it on first load.
            uint32_t cert_type = U2F_CERT_USER_UNENCRYPTED;
            FlipperFormat* ff = flipper_format_file_alloc(storage);
            bool key_written = false;
            if(flipper_format_file_open_always(ff, U2F_CERT_KEY_FILE)) {
                key_written =
                    flipper_format_write_header_cstr(
                        ff, U2F_CERT_KEY_FILE_TYPE, U2F_CERT_KEY_VERSION_PLAINTEXT) &&
                    flipper_format_write_uint32(ff, "Type", &cert_type, 1) &&
                    flipper_format_write_hex(ff, "Data", priv, sizeof(priv)) &&
                    flipper_format_file_close(ff);
            }
            flipper_format_free(ff);
            if(key_written) key_written = u2f_data_sync_file(storage, U2F_CERT_KEY_FILE);
            if(!key_written) {
                FURI_LOG_E(TAG, "Unable to write attestation certificate key");
                storage_common_remove(storage, U2F_CERT_KEY_FILE);
                storage_common_remove(storage, U2F_CERT_FILE);
                break;
            }
            state = true;
        } while(0);
    }

    if(der) {
        mbedtls_platform_zeroize(der, U2F_CERT_MAX_SIZE);
        free(der);
    }
    mbedtls_platform_zeroize(priv, sizeof(priv));
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&key);
    furi_record_close(RECORD_STORAGE);
    return state;
}

static bool u2f_data_cert_key_encrypt(uint8_t* cert_key) {
    furi_assert(cert_key);

    bool state = false;
    uint8_t iv[16];
    uint8_t key[48];
    uint32_t cert_type = U2F_CERT_USER;

    FURI_LOG_I(TAG, "Encrypting user cert key");

    // Generate random IV
    furi_hal_random_fill_buf(iv, 16);

    if(!u2f_crypto_encrypt(cert_key, key, 32, iv)) {
        FURI_LOG_E(TAG, "Encryption failed");
        mbedtls_platform_zeroize(iv, sizeof(iv));
        mbedtls_platform_zeroize(key, sizeof(key));
        return false;
    }

    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* flipper_format = flipper_format_file_alloc(storage);

    if(flipper_format_file_open_always(flipper_format, U2F_CERT_KEY_TEMP_FILE)) {
        do {
            if(!flipper_format_write_header_cstr(
                   flipper_format, U2F_CERT_KEY_FILE_TYPE, U2F_CERT_KEY_VERSION))
                break;
            if(!flipper_format_write_uint32(flipper_format, "Type", &cert_type, 1)) break;
            if(!flipper_format_write_hex(flipper_format, "IV", iv, 16)) break;
            if(!flipper_format_write_hex(flipper_format, "Data", key, 48)) break;
            state = flipper_format_file_close(flipper_format);
        } while(0);
    }

    flipper_format_free(flipper_format);
    if(state) state = u2f_data_sync_file(storage, U2F_CERT_KEY_TEMP_FILE);

    uint8_t verified_key[32] = {0};
    if(state) {
        FuriString* filetype = furi_string_alloc();
        FlipperFormat* verify_format = flipper_format_file_alloc(storage);
        uint8_t verify_iv[16] = {0};
        uint8_t verify_ciphertext[48] = {0};
        uint32_t verify_version = 0;
        uint32_t verify_type = 0;
        state = flipper_format_file_open_existing(verify_format, U2F_CERT_KEY_TEMP_FILE) &&
                flipper_format_read_header(verify_format, filetype, &verify_version) &&
                strcmp(furi_string_get_cstr(filetype), U2F_CERT_KEY_FILE_TYPE) == 0 &&
                verify_version == U2F_CERT_KEY_VERSION &&
                flipper_format_read_uint32(verify_format, "Type", &verify_type, 1) &&
                verify_type == U2F_CERT_USER &&
                flipper_format_read_hex(verify_format, "IV", verify_iv, sizeof(verify_iv)) &&
                flipper_format_read_hex(
                    verify_format, "Data", verify_ciphertext, sizeof(verify_ciphertext)) &&
                u2f_crypto_decrypt(verify_ciphertext, verified_key, sizeof(verified_key), verify_iv) &&
                memcmp(verified_key, cert_key, sizeof(verified_key)) == 0;
        flipper_format_free(verify_format);
        furi_string_free(filetype);
        mbedtls_platform_zeroize(verify_iv, sizeof(verify_iv));
        mbedtls_platform_zeroize(verify_ciphertext, sizeof(verify_ciphertext));
        mbedtls_platform_zeroize(verified_key, sizeof(verified_key));
    }
    if(state) {
        FS_Error remove_error = storage_common_remove(storage, U2F_CERT_KEY_FILE);
        state = remove_error == FSE_OK &&
                u2f_data_promote_temp(storage, U2F_CERT_KEY_TEMP_FILE, U2F_CERT_KEY_FILE);
    }
    furi_record_close(RECORD_STORAGE);

    mbedtls_platform_zeroize(iv, sizeof(iv));
    mbedtls_platform_zeroize(key, sizeof(key));
    return state;
}

bool u2f_data_cert_key_load(uint8_t* cert_key) {
    furi_assert(cert_key);

    bool state = false;
    uint8_t iv[16];
    uint8_t key[48];
    uint32_t cert_type = 0;
    uint32_t version = 0;

    FuriString* filetype;
    filetype = furi_string_alloc();

    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* flipper_format = flipper_format_file_alloc(storage);
    const char* source_path = U2F_CERT_KEY_FILE;
    bool source_is_temp = false;
    FS_Error main_stat = storage_common_stat(storage, U2F_CERT_KEY_FILE, NULL);
    if(main_stat == FSE_NOT_EXIST) {
        FS_Error temp_stat = storage_common_stat(storage, U2F_CERT_KEY_TEMP_FILE, NULL);
        if(temp_stat == FSE_OK) {
            source_path = U2F_CERT_KEY_TEMP_FILE;
            source_is_temp = true;
        } else if(temp_stat != FSE_NOT_EXIST) {
            FURI_LOG_E(TAG, "Unable to inspect temporary certificate key: %d", temp_stat);
        }
    } else if(main_stat != FSE_OK) {
        FURI_LOG_E(TAG, "Unable to inspect certificate key: %d", main_stat);
    }

    if((main_stat == FSE_OK || source_is_temp) &&
       flipper_format_file_open_existing(flipper_format, source_path)) {
        do {
            if(!flipper_format_read_header(flipper_format, filetype, &version)) {
                FURI_LOG_E(TAG, "Missing or incorrect header");
                break;
            }

            if(strcmp(furi_string_get_cstr(filetype), U2F_CERT_KEY_FILE_TYPE) != 0) {
                FURI_LOG_E(TAG, "Type or version mismatch");
                break;
            }

            if(!flipper_format_read_uint32(flipper_format, "Type", &cert_type, 1)) {
                FURI_LOG_E(TAG, "Missing cert type");
                break;
            }

            if(cert_type == U2F_CERT_STOCK) {
                FURI_LOG_E(
                    TAG,
                    "Factory cert key is unsupported; install generated cert.der and Type 2 "
                    "cert_key.u2f in /ext/u2f/assets/");
                break;
            } else if(cert_type == U2F_CERT_USER) {
                if(version != U2F_CERT_KEY_VERSION) {
                    FURI_LOG_E(TAG, "Legacy certificate-key encryption is not supported");
                    break;
                }
                if(!flipper_format_read_hex(flipper_format, "IV", iv, 16) ||
                   !flipper_format_read_hex(flipper_format, "Data", key, sizeof(key))) {
                    FURI_LOG_E(TAG, "Missing encrypted certificate key data");
                    break;
                }
                state = u2f_crypto_decrypt(key, cert_key, 32, iv);
                if(!state) FURI_LOG_E(TAG, "Certificate key decryption failed");
                break;
            } else if(cert_type == U2F_CERT_USER_UNENCRYPTED) {
                if(version != U2F_CERT_KEY_VERSION_PLAINTEXT) {
                    FURI_LOG_E(TAG, "Unsupported plaintext certificate-key version");
                    break;
                }
            } else {
                FURI_LOG_E(TAG, "Unknown cert type");
                break;
            }
            if(!flipper_format_read_hex(flipper_format, "Data", cert_key, 32)) {
                FURI_LOG_E(TAG, "Missing plaintext certificate key");
                break;
            }
            state = true;
        } while(0);
    }

    flipper_format_free(flipper_format);
    furi_string_free(filetype);

    if(state && cert_type == U2F_CERT_USER_UNENCRYPTED) {
        state = u2f_data_cert_key_encrypt(cert_key);
    } else if(state && source_is_temp) {
        state = u2f_data_promote_temp(storage, U2F_CERT_KEY_TEMP_FILE, U2F_CERT_KEY_FILE);
    }
    furi_record_close(RECORD_STORAGE);

    if(!state) mbedtls_platform_zeroize(cert_key, U2F_DATA_KEY_SIZE);
    mbedtls_platform_zeroize(iv, sizeof(iv));
    mbedtls_platform_zeroize(key, sizeof(key));
    return state;
}

U2fDataLoadResult u2f_data_key_load(uint8_t* device_key) {
    furi_assert(device_key);

    U2fDataLoadResult state = U2fDataLoadInvalid;
    uint8_t iv[16];
    uint8_t key[48];
    uint32_t version = 0;

    FuriString* filetype;
    filetype = furi_string_alloc();

    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* flipper_format = flipper_format_file_alloc(storage);

    FS_Error main_stat = storage_common_stat(storage, U2F_KEY_FILE, NULL);
    const char* source_path = U2F_KEY_FILE;
    bool source_is_temp = false;
    FS_Error source_stat = main_stat;
    if(main_stat == FSE_NOT_EXIST) {
        source_stat = storage_common_stat(storage, U2F_KEY_TEMP_FILE, NULL);
        if(source_stat == FSE_OK) {
            source_path = U2F_KEY_TEMP_FILE;
            source_is_temp = true;
        } else if(source_stat == FSE_NOT_EXIST) {
            state = U2fDataLoadMissing;
        }
    }

    if(source_stat == FSE_OK &&
       flipper_format_file_open_existing(flipper_format, source_path)) {
        do {
            if(!flipper_format_read_header(flipper_format, filetype, &version)) {
                FURI_LOG_E(TAG, "Missing or incorrect header");
                break;
            }
            if(strcmp(furi_string_get_cstr(filetype), U2F_DEVICE_KEY_FILE_TYPE) != 0 ||
               version != U2F_DEVICE_KEY_VERSION) {
                FURI_LOG_E(TAG, "Type or version mismatch");
                break;
            }
            if(!flipper_format_read_hex(flipper_format, "IV", iv, 16)) {
                FURI_LOG_E(TAG, "Missing IV");
                break;
            }
            if(!flipper_format_read_hex(flipper_format, "Data", key, 48)) {
                FURI_LOG_E(TAG, "Missing data");
                break;
            }
            if(!u2f_crypto_decrypt(key, device_key, 32, iv)) {
                FURI_LOG_E(TAG, "Device key authentication failed");
                break;
            }
            state = U2fDataLoadOk;
        } while(0);
    } else if(source_stat != FSE_OK && source_stat != FSE_NOT_EXIST) {
        FURI_LOG_E(TAG, "Unable to inspect device key file: %d", source_stat);
    } else if(source_stat == FSE_OK) {
        FURI_LOG_E(TAG, "Unable to open existing device key file");
    }
    flipper_format_free(flipper_format);
    if(state == U2fDataLoadOk && source_is_temp) {
        state = u2f_data_promote_temp(storage, U2F_KEY_TEMP_FILE, U2F_KEY_FILE) ?
                    U2fDataLoadOk :
                    U2fDataLoadInvalid;
    }
    furi_record_close(RECORD_STORAGE);
    furi_string_free(filetype);
    mbedtls_platform_zeroize(iv, sizeof(iv));
    mbedtls_platform_zeroize(key, sizeof(key));
    if(state != U2fDataLoadOk) mbedtls_platform_zeroize(device_key, 32);
    return state;
}

bool u2f_data_key_generate(uint8_t* device_key) {
    furi_assert(device_key);

    bool state = false;
    uint8_t iv[16];
    uint8_t key[32];
    uint8_t key_encrypted[48];

    // Generate random IV and key
    furi_hal_random_fill_buf(iv, 16);
    furi_hal_random_fill_buf(key, 32);

    if(!u2f_crypto_encrypt(key, key_encrypted, 32, iv)) {
        FURI_LOG_E(TAG, "Encryption failed");
        mbedtls_platform_zeroize(key, sizeof(key));
        mbedtls_platform_zeroize(iv, sizeof(iv));
        mbedtls_platform_zeroize(key_encrypted, sizeof(key_encrypted));
        return false;
    }

    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* flipper_format = flipper_format_file_alloc(storage);

    if(flipper_format_file_open_always(flipper_format, U2F_KEY_TEMP_FILE)) {
        do {
            if(!flipper_format_write_header_cstr(
                   flipper_format, U2F_DEVICE_KEY_FILE_TYPE, U2F_DEVICE_KEY_VERSION))
                break;
            if(!flipper_format_write_hex(flipper_format, "IV", iv, 16)) break;
            if(!flipper_format_write_hex(flipper_format, "Data", key_encrypted, 48)) break;
            state = flipper_format_file_close(flipper_format);
        } while(0);
    }

    flipper_format_free(flipper_format);
    if(state) state = u2f_data_sync_file(storage, U2F_KEY_TEMP_FILE);
    furi_record_close(RECORD_STORAGE);

    uint8_t verified_key[32] = {0};
    if(state) {
        state = u2f_data_key_load(verified_key) == U2fDataLoadOk &&
                memcmp(verified_key, key, sizeof(key)) == 0;
    }
    if(state) memcpy(device_key, verified_key, sizeof(verified_key));

    if(!state) mbedtls_platform_zeroize(device_key, 32);
    mbedtls_platform_zeroize(verified_key, sizeof(verified_key));
    mbedtls_platform_zeroize(key, sizeof(key));
    mbedtls_platform_zeroize(iv, sizeof(iv));
    mbedtls_platform_zeroize(key_encrypted, sizeof(key_encrypted));
    return state;
}

static U2fDataLoadResult u2f_data_cnt_read_path(
    Storage* storage,
    const char* path,
    uint32_t* counter,
    bool* exists) {
    U2fDataLoadResult state = U2fDataLoadInvalid;
    uint8_t iv[16] = {0};
    U2fCounterData cnt = {0};
    uint8_t encrypted[48] = {0};
    uint32_t version = 0;
    *exists = true;

    FS_Error stat_result = storage_common_stat(storage, path, NULL);
    if(stat_result == FSE_NOT_EXIST) {
        *exists = false;
        state = U2fDataLoadMissing;
    } else if(stat_result != FSE_OK) {
        FURI_LOG_E(TAG, "Unable to inspect counter slot %s: %d", path, stat_result);
    } else {
        FuriString* filetype = furi_string_alloc();
        FlipperFormat* format = flipper_format_file_alloc(storage);
        if(flipper_format_file_open_existing(format, path)) {
            do {
                if(!flipper_format_read_header(format, filetype, &version) ||
                   strcmp(furi_string_get_cstr(filetype), U2F_COUNTER_FILE_TYPE) != 0 ||
                   version != U2F_COUNTER_VERSION ||
                   !flipper_format_read_hex(format, "IV", iv, sizeof(iv)) ||
                   !flipper_format_read_hex(format, "Data", encrypted, sizeof(encrypted))) {
                    FURI_LOG_E(TAG, "Counter slot %s has an invalid format", path);
                    break;
                }
                if(!u2f_crypto_decrypt(
                       encrypted, (uint8_t*)&cnt, sizeof(cnt), iv)) {
                    FURI_LOG_E(TAG, "Counter slot %s failed authentication", path);
                    break;
                }
                if(cnt.control != U2F_COUNTER_CONTROL_VAL) {
                    FURI_LOG_E(TAG, "Counter slot %s has invalid control data", path);
                    break;
                }
                *counter = cnt.counter;
                state = U2fDataLoadOk;
            } while(false);
        } else {
            FURI_LOG_E(TAG, "Unable to open counter slot %s", path);
        }
        flipper_format_free(format);
        furi_string_free(filetype);
    }

    mbedtls_platform_zeroize(iv, sizeof(iv));
    mbedtls_platform_zeroize(&cnt, sizeof(cnt));
    mbedtls_platform_zeroize(encrypted, sizeof(encrypted));
    return state;
}

static bool u2f_data_cnt_write_path(Storage* storage, const char* path, uint32_t counter) {
    bool written = false;
    uint8_t iv[16] = {0};
    U2fCounterData data = {0};
    uint8_t encrypted[48] = {0};
    furi_hal_random_fill_buf(iv, sizeof(iv));
    furi_hal_random_fill_buf(data.random_salt, sizeof(data.random_salt));
    data.control = U2F_COUNTER_CONTROL_VAL;
    data.counter = counter;

    if(!u2f_crypto_encrypt((uint8_t*)&data, encrypted, sizeof(data), iv)) {
        FURI_LOG_E(TAG, "Counter encryption failed");
        goto cleanup;
    }

    FlipperFormat* format = flipper_format_file_alloc(storage);
    if(flipper_format_file_open_always(format, path)) {
        written = flipper_format_write_header_cstr(
                      format, U2F_COUNTER_FILE_TYPE, U2F_COUNTER_VERSION) &&
                  flipper_format_write_hex(format, "IV", iv, sizeof(iv)) &&
                  flipper_format_write_hex(format, "Data", encrypted, sizeof(encrypted)) &&
                  flipper_format_file_close(format);
    }
    flipper_format_free(format);
    if(!written || !u2f_data_sync_file(storage, path)) {
        FURI_LOG_E(TAG, "Unable to write and sync counter slot %s", path);
        written = false;
        goto cleanup;
    }

    uint32_t verified = 0;
    bool exists = false;
    written = u2f_data_cnt_read_path(storage, path, &verified, &exists) == U2fDataLoadOk &&
              verified == counter;
    if(!written) FURI_LOG_E(TAG, "Counter slot %s failed read-back verification", path);

cleanup:
    mbedtls_platform_zeroize(iv, sizeof(iv));
    mbedtls_platform_zeroize(&data, sizeof(data));
    mbedtls_platform_zeroize(encrypted, sizeof(encrypted));
    return written;
}

U2fDataLoadResult u2f_data_cnt_read(uint32_t* cnt_val) {
    furi_assert(cnt_val);
    Storage* storage = furi_record_open(RECORD_STORAGE);
    uint32_t count_a = 0;
    uint32_t count_b = 0;
    bool exists_a = false;
    bool exists_b = false;
    U2fDataLoadResult result_a =
        u2f_data_cnt_read_path(storage, U2F_CNT_FILE, &count_a, &exists_a);
    U2fDataLoadResult result_b =
        u2f_data_cnt_read_path(storage, U2F_CNT_FILE_B, &count_b, &exists_b);
    furi_record_close(RECORD_STORAGE);

    if(result_a == U2fDataLoadOk && result_b == U2fDataLoadOk) {
        *cnt_val = count_a >= count_b ? count_a : count_b;
        return U2fDataLoadOk;
    }
    if(result_a == U2fDataLoadOk) {
        *cnt_val = count_a;
        return U2fDataLoadOk;
    }
    if(result_b == U2fDataLoadOk) {
        *cnt_val = count_b;
        return U2fDataLoadOk;
    }
    return exists_a || exists_b ? U2fDataLoadInvalid : U2fDataLoadMissing;
}

bool u2f_data_cnt_write(uint32_t cnt_val) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    uint32_t count_a = 0;
    uint32_t count_b = 0;
    bool exists_a = false;
    bool exists_b = false;
    U2fDataLoadResult result_a =
        u2f_data_cnt_read_path(storage, U2F_CNT_FILE, &count_a, &exists_a);
    U2fDataLoadResult result_b =
        u2f_data_cnt_read_path(storage, U2F_CNT_FILE_B, &count_b, &exists_b);
    if((result_a == U2fDataLoadOk && cnt_val <= count_a) ||
       (result_b == U2fDataLoadOk && cnt_val <= count_b)) {
        FURI_LOG_E(TAG, "Refusing non-increasing counter write");
        furi_record_close(RECORD_STORAGE);
        return false;
    }

    const char* target;
    if(result_a == U2fDataLoadOk && result_b == U2fDataLoadOk) {
        target = count_a >= count_b ? U2F_CNT_FILE_B : U2F_CNT_FILE;
    } else if(result_a == U2fDataLoadOk) {
        target = U2F_CNT_FILE_B;
    } else if(result_b == U2fDataLoadOk) {
        target = U2F_CNT_FILE;
    } else if(!exists_a && !exists_b && cnt_val == 0) {
        target = U2F_CNT_FILE;
    } else {
        FURI_LOG_E(TAG, "Refusing counter write: no valid persistent counter exists");
        furi_record_close(RECORD_STORAGE);
        return false;
    }

    bool result = u2f_data_cnt_write_path(storage, target, cnt_val);
    furi_record_close(RECORD_STORAGE);
    return result;
}

U2fDataLoadResult u2f_data_provisioning_state(void) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FS_Error status = storage_common_stat(storage, U2F_PROVISION_FILE, NULL);
    if(status == FSE_NOT_EXIST) {
        furi_record_close(RECORD_STORAGE);
        return U2fDataLoadMissing;
    }
    if(status != FSE_OK) {
        FURI_LOG_E(TAG, "Unable to inspect provisioning marker: %d", status);
        furi_record_close(RECORD_STORAGE);
        return U2fDataLoadInvalid;
    }

    static const uint8_t marker[] = {'U', '2', 'F', '-', 'I', 'N', 'I', 'T'};
    File* file = storage_file_alloc(storage);
    uint8_t data[sizeof(marker)] = {0};
    bool valid = storage_file_open(file, U2F_PROVISION_FILE, FSAM_READ, FSOM_OPEN_EXISTING) &&
                 storage_file_size(file) == sizeof(marker) &&
                 storage_file_read(file, data, sizeof(data)) == sizeof(data) &&
                 memcmp(data, marker, sizeof(marker)) == 0;
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    mbedtls_platform_zeroize(data, sizeof(data));
    return valid ? U2fDataLoadOk : U2fDataLoadInvalid;
}

bool u2f_data_provisioning_begin(void) {
    static const uint8_t marker[] = {'U', '2', 'F', '-', 'I', 'N', 'I', 'T'};
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    bool written = storage_file_open(
                       file, U2F_PROVISION_FILE, FSAM_WRITE, FSOM_CREATE_NEW) &&
                   storage_file_write(file, marker, sizeof(marker)) == sizeof(marker) &&
                   storage_file_sync(file);
    bool closed = storage_file_close(file);
    storage_file_free(file);
    if(written && closed) {
        written = u2f_data_sync_file(storage, U2F_PROVISION_FILE);
    } else {
        written = false;
    }
    furi_record_close(RECORD_STORAGE);
    return written;
}

bool u2f_data_provisioning_finish(void) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FS_Error result = storage_common_remove(storage, U2F_PROVISION_FILE);
    furi_record_close(RECORD_STORAGE);
    if(result != FSE_OK) {
        FURI_LOG_E(TAG, "Unable to remove completed provisioning marker: %d", result);
        return false;
    }
    return true;
}
