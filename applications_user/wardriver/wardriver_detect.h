#pragma once
#include "wardriver_types.h"
/* Passive observations only: no connections, scan requests or GATT queries. */
bool wardriver_detect_ble(const uint8_t* data,size_t length,WardriverObservation* out);
bool wardriver_detect_beacon(const uint8_t* frame,size_t length,WardriverObservation* out);
bool wardriver_detect_remote_id(const uint8_t* data,size_t length,WardriverObservation* out);
void wardriver_detect_wifi(WardriverObservation* out);
void wardriver_match(WardriverObservation* out,uint32_t hint,WardriverConfidence confidence,WardriverEvidence evidence);
