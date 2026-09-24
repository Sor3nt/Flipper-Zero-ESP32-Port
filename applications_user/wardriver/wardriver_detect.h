#pragma once
#include "wardriver_types.h"
/* Advertisements only. Never initiates a connection, scan request or GATT query. */
bool wardriver_detect_ble(const uint8_t* data,size_t length,WardriverObservation* out);
bool wardriver_detect_beacon(const uint8_t* frame,size_t length,WardriverObservation* out);
bool wardriver_detect_remote_id(const uint8_t* data,size_t length,WardriverObservation* out);
