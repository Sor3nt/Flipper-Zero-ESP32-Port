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
