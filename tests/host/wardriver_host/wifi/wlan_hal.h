#pragma once
const char* wlan_hal_survey_status(void);

#include <stdbool.h>
bool wlan_hal_is_connected(void);
bool wlan_hal_run_in_worker(void (*fn)(void*),void* context);
