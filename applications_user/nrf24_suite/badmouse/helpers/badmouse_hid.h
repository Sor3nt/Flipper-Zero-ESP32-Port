#pragma once

#include <furi.h>
#include "nrf24tool.h"
#include "libnrf24/nrf24.h"

#define LOGITECH_MIN_CHANNEL    2
#define LOGITECH_MAX_CHANNEL    85
#define DEFAULT_TX_RETRY        50
#define DEFAULT_FIND_CHAN_RETRY 2
#define KEEP_ALIVE_DELAY        10 // ms

bool find_channel(NRF24L01_Config* config);
bool bm_press_key(Nrf24Tool* app, uint16_t hid_code);
void bm_release_key(Nrf24Tool* app, uint16_t hid_code);
void bm_release_all(Nrf24Tool* app);
bool bm_send_keep_alive(Nrf24Tool* app);
bool bm_start_transmission(Nrf24Tool* app);
bool bm_end_transmission(Nrf24Tool* app);
