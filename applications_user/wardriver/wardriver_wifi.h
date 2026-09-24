#pragma once
#include "wardriver_types.h"
#include <furi.h>

typedef struct WardriverRadio WardriverRadio;
typedef void (*WardriverRadioProgress)(void* context,const char* stage);
WardriverRadio* wardriver_radio_alloc(FuriMessageQueue* observations,WardriverRadioProgress progress,void* context);
void wardriver_radio_free(WardriverRadio* radio);
bool wardriver_radio_start(WardriverRadio* radio);
void wardriver_radio_poll(WardriverRadio* radio, uint32_t now_ms);
void wardriver_radio_stop(WardriverRadio* radio);
/* Stop/drain FAP callbacks before saving; release restores the prior radio state. */
void wardriver_radio_quiesce(WardriverRadio* radio);
uint32_t wardriver_radio_dropped(WardriverRadio* radio);
const char* wardriver_radio_status(WardriverRadio* radio);
const char* wardriver_radio_wifi_status(WardriverRadio* radio);
const char* wardriver_radio_ble_status(WardriverRadio* radio);
