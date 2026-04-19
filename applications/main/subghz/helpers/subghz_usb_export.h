#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void subghz_usb_export_start(uint32_t frequency, const char* preset_name);
void subghz_usb_export_send_data(const int32_t* data, uint16_t count);
void subghz_usb_export_stop(void);

#ifdef __cplusplus
}
#endif
