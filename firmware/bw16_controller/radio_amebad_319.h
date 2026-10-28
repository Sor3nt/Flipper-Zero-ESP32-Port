/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef BW16_RADIO_AMEBAD_319_H
#define BW16_RADIO_AMEBAD_319_H
#include <stdint.h>
void bwn_radio_deauth(const uint8_t source[6], const uint8_t destination[6], uint16_t reason);
#endif
