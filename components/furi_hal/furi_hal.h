#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <furi_hal_cortex.h>
#include <furi_hal_debug.h>
#include <furi_hal_rtc.h>
#include <furi_hal_power.h>
#include <furi_hal_interrupt.h>
#include <furi_hal_bt.h>
#include <furi_hal_crypto.h>
#include <furi_hal_info.h>
#include <furi_hal_gpio.h>
#include <furi_hal_resources.h>
#include <furi_hal_usb.h>
#include <furi_hal_random.h>
#include <furi_hal_memory.h>
#include <furi_hal_version.h>
#include <furi_hal_light.h>
#include <furi_hal_vibro.h>
#include <furi_hal_speaker.h>
#include <furi_hal_display.h>
#include <furi_hal_touch.h>
#include <furi_hal_spi.h>
#include <furi_hal_subghz.h>
#include <furi_hal_nfc.h>
#include <furi_hal_infrared.h>
#include <furi_hal_rfid.h>

void furi_hal_init_early(void);
void furi_hal_deinit_early(void);
void furi_hal_init(void);

#ifdef __cplusplus
}
#endif
