#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* MouseJacker — vendor-aware HID injection over NRF24L01+.
 *
 * Translates the Bruce-firmware MouseJack module
 * (`bruce/firmware-main/src/modules/NRF24/nrf_mousejack.{h,cpp}`) into the
 * Furi/ESP-IDF world. The radio interface goes through `nrf24_hw_*`. UI lives
 * in scenes.
 *
 * Caller responsibility: wrap any sequence of mj_* radio calls inside
 * `nrf24_hw_acquire()` / `nrf24_hw_release()`.
 */

#define MJ_MAX_TARGETS 16

typedef enum {
    MjVendorUnknown = 0,
    MjVendorMicrosoft,
    MjVendorMsCrypt,
    MjVendorLogitech,
} MjVendor;

typedef struct {
    uint8_t address[5];
    uint8_t addr_len;
    uint8_t channel;
    MjVendor vendor;
    uint32_t hits;
} MjTarget;

/* HID modifier bits */
#define MJ_MOD_NONE   0x00
#define MJ_MOD_LCTRL  0x01
#define MJ_MOD_LSHIFT 0x02
#define MJ_MOD_LALT   0x04
#define MJ_MOD_LGUI   0x08

/* HID keycodes used by the parser. Full table in nrf24_mj_core.c. */
#define MJ_KEY_NONE      0x00
#define MJ_KEY_A         0x04
#define MJ_KEY_1         0x1E
#define MJ_KEY_ENTER     0x28
#define MJ_KEY_ESC       0x29
#define MJ_KEY_BACKSPACE 0x2A
#define MJ_KEY_TAB       0x2B
#define MJ_KEY_SPACE     0x2C
#define MJ_KEY_MINUS     0x2D
#define MJ_KEY_EQUAL     0x2E
#define MJ_KEY_LBRACKET  0x2F
#define MJ_KEY_RBRACKET  0x30
#define MJ_KEY_BACKSLASH 0x31
#define MJ_KEY_SEMICOLON 0x33
#define MJ_KEY_QUOTE     0x34
#define MJ_KEY_GRAVE     0x35
#define MJ_KEY_COMMA     0x36
#define MJ_KEY_DOT       0x37
#define MJ_KEY_SLASH     0x38
#define MJ_KEY_CAPSLOCK  0x39
#define MJ_KEY_F1        0x3A
#define MJ_KEY_F12       0x45
#define MJ_KEY_PRINTSCR  0x46
#define MJ_KEY_SCROLLLOCK 0x47
#define MJ_KEY_PAUSE     0x48
#define MJ_KEY_INSERT    0x49
#define MJ_KEY_HOME      0x4A
#define MJ_KEY_PAGEUP    0x4B
#define MJ_KEY_DELETE    0x4C
#define MJ_KEY_END       0x4D
#define MJ_KEY_PAGEDOWN  0x4E
#define MJ_KEY_RIGHT     0x4F
#define MJ_KEY_LEFT      0x50
#define MJ_KEY_DOWN      0x51
#define MJ_KEY_UP        0x52

typedef struct {
    uint8_t modifier;
    uint8_t keycode;
} MjHidKey;

/* Map ASCII (0x20..0x7E) to HID modifier+keycode (US layout).
 * Returns false for non-printable/unknown chars. */
bool mj_ascii_to_hid(char c, MjHidKey* out);

/* Look up a DuckyScript key name (case-insensitive, e.g. "ENTER", "F5", "GUI").
 * Returns true on hit, sets modifier (mask bits) and keycode (may be MJ_KEY_NONE
 * for pure modifier names like "CTRL"). */
bool mj_lookup_ducky_key(const char* name, uint8_t* modifier, uint8_t* keycode);

/* ---- Scan / fingerprint ---- */

/* Decode a 32-byte raw promiscuous frame, validate CRC16-CCITT, fingerprint
 * the inner ESB payload as Logitech / Microsoft / MS-encrypted. On match the
 * target is added to (or refreshed in) `targets` (capacity MJ_MAX_TARGETS).
 * Returns the index of the matched target (>=0) or -1.
 * Caller must hold nrf24_hw_acquire (this only crunches bytes — no SPI). */
int mj_fingerprint(
    const uint8_t* raw_buf,
    uint8_t size,
    uint8_t channel,
    MjTarget* targets,
    uint8_t* target_count);

/* Reset the global Microsoft sequence counter. Call once before starting an
 * attack against a Microsoft target. */
void mj_reset_ms_sequence(void);

/* ---- Transmit primitives ---- */

/* Configure NRF24 for TX against the given target (address width, channel,
 * payload size). Caller must hold nrf24_hw_acquire. */
void mj_setup_tx_for_target(const MjTarget* target);

/* Send a Logitech wake/keepalive frame to nudge sleeping receivers, then a
 * neutral keystroke. No-op for non-Logitech vendors.
 * Caller must hold nrf24_hw_acquire. */
void mj_logitech_wake(const MjTarget* target);

/* Inject a single keystroke (modifier + keycode) plus implicit release.
 * Vendor-aware. Returns true if the radio accepted at least one frame.
 * Caller must hold nrf24_hw_acquire. */
bool mj_inject_keystroke(const MjTarget* target, uint8_t modifier, uint8_t keycode);

/* Type a printable ASCII string (use '\n' for ENTER, '\t' for TAB).
 * Stops on `*abort` becoming true if non-NULL.
 * Caller must hold nrf24_hw_acquire. */
void mj_inject_string(const MjTarget* target, const char* text, volatile bool* abort);

/* ---- Display helpers ---- */

const char* mj_vendor_label(MjVendor v); /* "MS", "MS*", "LG", "??" */

/* Render a target as e.g. "AB:CD (LG) ch72". Truncates to fit `cap`. */
void mj_format_target(const MjTarget* t, char* buf, size_t cap);
