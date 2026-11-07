#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <furi.h>

typedef enum {
    U2fDataLoadOk,
    U2fDataLoadMissing,
    U2fDataLoadInvalid,
} U2fDataLoadResult;

bool u2f_data_check(bool cert_only);

bool u2f_data_cert_check(void);

// True only if a certificate key file exists whose type this firmware can use
// (encrypted user key or plaintext user key). Does not decrypt and has no side
// effects; a factory (Type 0) key or a missing/invalid file returns false.
bool u2f_data_cert_key_check(void);

// Generate a device-specific self-signed P-256 attestation certificate and its
// (plaintext) key file when none exist yet. Refuses to overwrite any existing
// cert/key material. Returns true on success.
bool u2f_data_cert_generate(void);

uint32_t u2f_data_cert_load(uint8_t* cert);

bool u2f_data_cert_key_load(uint8_t* cert_key);

U2fDataLoadResult u2f_data_key_load(uint8_t* device_key);

bool u2f_data_key_generate(uint8_t* device_key);

U2fDataLoadResult u2f_data_cnt_read(uint32_t* cnt);

bool u2f_data_cnt_write(uint32_t cnt);

U2fDataLoadResult u2f_data_provisioning_state(void);

bool u2f_data_provisioning_begin(void);

bool u2f_data_provisioning_finish(void);

#ifdef __cplusplus
}
#endif
