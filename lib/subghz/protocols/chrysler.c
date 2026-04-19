#include "chrysler.h"

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"
#include "../blocks/custom_btn_i.h"

#define TAG "Chrysler"

//   Chrysler keyfob rolling code protocol
//   Found on: PT Cruiser, Dodge, Jeep (~2004-2010)
//
//   RF: 433.92 MHz, OOK PWM encoding
//   Bit timing: ~4000us total period
//     Bit 0: ~300us HIGH + ~3700us LOW
//     Bit 1: ~600us HIGH + ~3400us LOW
//   Frame: 24-bit zero preamble + gap ~15600us + 80-bit data
//   Retransmission: same frame sent twice per press
//
//   80-bit frame layout (10 bytes):
//     Byte 0: [counter:4 | device_id:4]
//       Counter: 4-bit, bit-reversed, decrementing
//       Device ID: constant per keyfob (e.g. 0xB)
//     Bytes 1-4: nibble-interleaved rolling code + button
//       MSB(b0)=0: high nibbles = rolling, low nibbles = button
//       MSB(b0)=1: low nibbles = rolling, high nibbles = button
//     Byte 5: check byte (b1 XOR 0xC3 when MSB=0, b1 when MSB=1)
//     Byte 6: b1 XOR mask (mask depends on MSB and button)
//     Bytes 7-9: b2-b4 XOR fixed mask (redundancy copy)
//
//   Rolling code: single 8-bit value XOR'd with per-device serial offsets
//   across all 4 byte positions. The 4 bytes are related by constant XOR
//   (the serial).

static const SubGhzBlockConst subghz_protocol_chrysler_const = {
    .te_short = 300,
    .te_long = 600,
    .te_delta = 150,
    .min_count_bit_for_found = 80,
};

#define CHRYSLER_BIT_PERIOD     4000u
#define CHRYSLER_BIT_TOLERANCE  800u
#define CHRYSLER_PREAMBLE_MIN   15u
#define CHRYSLER_PREAMBLE_GAP   10000u
#define CHRYSLER_DATA_BITS      80u
#define CHRYSLER_SHORT_MAX      450u
#define CHRYSLER_LONG_MIN       450u
#define CHRYSLER_LONG_MAX       800u

struct SubGhzProtocolDecoderChrysler {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
    uint8_t decoder_state;
    uint16_t preamble_count;
    uint8_t raw_data[10];
    uint8_t bit_count;
    uint32_t te_last;
};

struct SubGhzProtocolEncoderChrysler {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
    uint8_t raw_data[10];
};

typedef enum {
    ChryslerDecoderStepReset = 0,
    ChryslerDecoderStepPreamble,
    ChryslerDecoderStepGap,
    ChryslerDecoderStepData,
} ChryslerDecoderStep;

const SubGhzProtocolDecoder subghz_protocol_chrysler_decoder = {
    .alloc = subghz_protocol_decoder_chrysler_alloc,
    .free = subghz_protocol_decoder_chrysler_free,
    .feed = subghz_protocol_decoder_chrysler_feed,
    .reset = subghz_protocol_decoder_chrysler_reset,
    .get_hash_data = subghz_protocol_decoder_chrysler_get_hash_data,
    .serialize = subghz_protocol_decoder_chrysler_serialize,
    .deserialize = subghz_protocol_decoder_chrysler_deserialize,
    .get_string = subghz_protocol_decoder_chrysler_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_chrysler_encoder = {
    .alloc = subghz_protocol_encoder_chrysler_alloc,
    .free = subghz_protocol_encoder_chrysler_free,
    .deserialize = subghz_protocol_encoder_chrysler_deserialize,
    .stop = subghz_protocol_encoder_chrysler_stop,
    .yield = subghz_protocol_encoder_chrysler_yield,
};

const SubGhzProtocol subghz_protocol_chrysler = {
    .name = CHRYSLER_PROTOCOL_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send | SubGhzProtocolFlag_Automotive,
    .decoder = &subghz_protocol_chrysler_decoder,
    .encoder = &subghz_protocol_chrysler_encoder,
};

static uint8_t chrysler_reverse_nibble(uint8_t n) {
    return (uint8_t)(((n & 1) << 3) | ((n & 2) << 1) | ((n & 4) >> 1) | ((n & 8) >> 3));
}

// Encoder

#define CHRYSLER_ENCODER_UPLOAD_MAX 800
#define CHRYSLER_ENCODER_REPEAT     3
#define CHRYSLER_PREAMBLE_BITS      24
#define CHRYSLER_PREAMBLE_GAP_US    15600

static uint8_t chrysler_custom_to_btn(uint8_t custom) {
    switch(custom) {
    case 1:
        return 0x01; // Lock
    case 2:
        return 0x02; // Unlock
    default:
        return 0;
    }
}

static void chrysler_advance_rolling(uint8_t* d) {
    // Advance the counter and rolling code for the next transmission.
    //
    // Counter: 4-bit bit-reversed in upper nibble of b0, decrementing.
    // Rolling code: nibble-interleaved into bytes 1-4, swapped based on MSB(b0).
    //
    // Step 1: Extract current rolling nibbles and button nibbles
    uint8_t msb = (d[0] >> 7) & 1;
    uint8_t rolling[4], button[4];
    for(int i = 0; i < 4; i++) {
        if(msb == 0) {
            rolling[i] = (d[1 + i] >> 4) & 0xF;
            button[i] = d[1 + i] & 0xF;
        } else {
            rolling[i] = d[1 + i] & 0xF;
            button[i] = (d[1 + i] >> 4) & 0xF;
        }
    }

    // Step 2: Decrement the bit-reversed counter
    uint8_t cnt_raw = (d[0] >> 4) & 0xF;
    uint8_t cnt = chrysler_reverse_nibble(cnt_raw);
    cnt = (cnt - 1) & 0xF;
    cnt_raw = chrysler_reverse_nibble(cnt);
    uint8_t new_msb = (cnt_raw >> 3) & 1;

    // Step 3: Reassemble byte 0
    d[0] = (cnt_raw << 4) | (d[0] & 0x0F);

    // Step 4: Re-interleave nibbles with new MSB
    // The rolling nibbles stay the same for one step (they change every 2 presses,
    // i.e. when MSB returns to the same value). The button nibbles may differ
    // between MSB=0 and MSB=1 states.
    for(int i = 0; i < 4; i++) {
        if(new_msb == 0) {
            d[1 + i] = (rolling[i] << 4) | (button[i] & 0xF);
        } else {
            d[1 + i] = ((button[i] & 0xF) << 4) | rolling[i];
        }
    }
}

static void chrysler_encoder_rebuild(SubGhzProtocolEncoderChrysler* instance) {
    uint8_t* d = instance->raw_data;
    uint8_t msb = (d[0] >> 7) & 1;
    uint8_t btn = instance->generic.btn;

    uint8_t custom = subghz_custom_btn_get();
    if(custom != 0) {
        uint8_t new_btn = chrysler_custom_to_btn(custom);
        if(new_btn != 0) btn = new_btn;
    }

    // Determine b1^b6 mask based on button and MSB
    uint8_t b1_xor_b6;
    if(msb == 0) {
        b1_xor_b6 = (btn == 0x01) ? 0x04 : 0x08;
    } else {
        b1_xor_b6 = 0x62;
    }

    // Rebuild byte 5
    d[5] = (msb == 0) ? (d[1] ^ 0xC3) : d[1];

    // Rebuild byte 6
    d[6] = d[1] ^ b1_xor_b6;

    // Rebuild bytes 7-9 from bytes 2-4
    if(msb == 0) {
        d[7] = d[2] ^ 0x63;
        d[8] = d[3] ^ 0x59;
        d[9] = d[4] ^ 0x46;
    } else {
        d[7] = d[2] ^ 0x9A;
        d[8] = d[3] ^ 0xC6;
        d[9] = d[4] ^ ((btn == 0x01) ? 0x20 : 0x10);
    }
}

static bool chrysler_encoder_get_upload(SubGhzProtocolEncoderChrysler* instance) {
    uint32_t te_short = subghz_protocol_chrysler_const.te_short;
    uint32_t te_bit_period = CHRYSLER_BIT_PERIOD;
    size_t index = 0;
    size_t max_upload = CHRYSLER_ENCODER_UPLOAD_MAX;

    // Preamble: 24 zero bits (short HIGH + long LOW each)
    for(uint8_t i = 0; i < CHRYSLER_PREAMBLE_BITS && (index + 1) < max_upload; i++) {
        instance->encoder.upload[index++] = level_duration_make(true, te_short);
        instance->encoder.upload[index++] =
            level_duration_make(false, te_bit_period - te_short);
    }

    // Gap between preamble and data
    if(index > 0) {
        instance->encoder.upload[index - 1] =
            level_duration_make(false, CHRYSLER_PREAMBLE_GAP_US);
    }

    // Data: 80 bits PWM
    for(uint8_t bit_i = 0; bit_i < CHRYSLER_DATA_BITS && (index + 1) < max_upload; bit_i++) {
        uint8_t byte_idx = bit_i / 8;
        uint8_t bit_pos = 7 - (bit_i % 8);
        bool data_bit = (instance->raw_data[byte_idx] >> bit_pos) & 1;

        uint32_t high_dur = data_bit ? 600 : te_short;
        uint32_t low_dur = te_bit_period - high_dur;

        instance->encoder.upload[index++] = level_duration_make(true, high_dur);
        instance->encoder.upload[index++] = level_duration_make(false, low_dur);
    }

    // Final gap after frame
    if(index > 0) {
        instance->encoder.upload[index - 1] =
            level_duration_make(false, CHRYSLER_PREAMBLE_GAP_US);
    }

    instance->encoder.size_upload = index;
    return index > 0;
}

void* subghz_protocol_encoder_chrysler_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderChrysler* instance = calloc(1, sizeof(SubGhzProtocolEncoderChrysler));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_chrysler;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = CHRYSLER_ENCODER_REPEAT;
    instance->encoder.size_upload = CHRYSLER_ENCODER_UPLOAD_MAX;
    instance->encoder.upload = malloc(CHRYSLER_ENCODER_UPLOAD_MAX * sizeof(LevelDuration));
    furi_check(instance->encoder.upload);
    instance->encoder.is_running = false;
    return instance;
}

void subghz_protocol_encoder_chrysler_free(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderChrysler* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

SubGhzProtocolStatus
    subghz_protocol_encoder_chrysler_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolEncoderChrysler* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    do {
        ret = subghz_block_generic_deserialize(&instance->generic, flipper_format);
        if(ret != SubGhzProtocolStatusOk) break;

        // Rebuild raw_data from generic.data (bytes 0-7)
        memset(instance->raw_data, 0, sizeof(instance->raw_data));
        uint64_t key = instance->generic.data;
        for(int i = 0; i < 8; i++) {
            instance->raw_data[i] = (uint8_t)(key >> (56 - i * 8));
        }

        // Read extra bytes 8-9
        uint32_t extra = 0;
        if(flipper_format_read_uint32(flipper_format, "Extra", &extra, 1)) {
            instance->raw_data[8] = (extra >> 8) & 0xFF;
            instance->raw_data[9] = extra & 0xFF;
        }

        // Advance rolling code (decrement counter, swap nibble interleaving)
        chrysler_advance_rolling(instance->raw_data);

        // Rebuild check bytes with (possibly changed) button
        chrysler_encoder_rebuild(instance);

        if(!chrysler_encoder_get_upload(instance)) {
            ret = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }

        instance->encoder.repeat = CHRYSLER_ENCODER_REPEAT;
        instance->encoder.front = 0;
        instance->encoder.is_running = true;
    } while(false);

    return ret;
}

void subghz_protocol_encoder_chrysler_stop(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderChrysler* instance = context;
    instance->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_chrysler_yield(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderChrysler* instance = context;

    if(instance->encoder.repeat == 0 || !instance->encoder.is_running) {
        instance->encoder.is_running = false;
        return level_duration_reset();
    }

    LevelDuration ret = instance->encoder.upload[instance->encoder.front];

    if(++instance->encoder.front == instance->encoder.size_upload) {
        if(!subghz_block_generic_global.endless_tx) {
            instance->encoder.repeat--;
        }
        instance->encoder.front = 0;
        chrysler_advance_rolling(instance->raw_data);
        chrysler_encoder_rebuild(instance);
        chrysler_encoder_get_upload(instance);
    }

    return ret;
}

// Decoder

static void chrysler_parse_data(SubGhzProtocolDecoderChrysler* instance) {
    uint8_t* d = instance->raw_data;

    uint8_t cnt_raw = (d[0] >> 4) & 0xF;
    uint8_t cnt = chrysler_reverse_nibble(cnt_raw);
    uint8_t dev_id = d[0] & 0xF;
    uint8_t msb = (d[0] >> 7) & 1;

    // Determine button from b1^b6 mask
    uint8_t b1_xor_b6 = d[1] ^ d[6];
    uint8_t btn = 0;
    if(msb == 0) {
        if(b1_xor_b6 == 0x04)
            btn = 0x01; // Lock
        else if(b1_xor_b6 == 0x08)
            btn = 0x02; // Unlock
        else
            btn = 0x00;
    } else {
        btn = 0xFF; // Can't distinguish from MSB=1 mask (both = 0x62)
    }

    // Serial: XOR offsets between byte positions (constant per device)
    // We derive it from the relationship between byte positions
    // serial_bytes[i] = rolling_value XOR bytes[1+i]_rolling_nibble
    // Since all positions share the same LFSR, XOR between positions is the serial

    instance->generic.serial =
        ((uint32_t)(d[1] ^ d[2]) << 24) |
        ((uint32_t)(d[1] ^ d[3]) << 16) |
        ((uint32_t)(d[1] ^ d[4]) << 8) |
        ((uint32_t)dev_id);

    instance->generic.cnt = cnt;
    instance->generic.btn = (btn != 0xFF) ? btn : 0;

    // Store full 80-bit data
    instance->generic.data =
        ((uint64_t)d[0] << 56) | ((uint64_t)d[1] << 48) |
        ((uint64_t)d[2] << 40) | ((uint64_t)d[3] << 32) |
        ((uint64_t)d[4] << 24) | ((uint64_t)d[5] << 16) |
        ((uint64_t)d[6] << 8) | ((uint64_t)d[7]);
    instance->generic.data_count_bit = CHRYSLER_DATA_BITS;
}

static bool chrysler_validate(SubGhzProtocolDecoderChrysler* instance) {
    uint8_t* d = instance->raw_data;
    uint8_t msb = (d[0] >> 7) & 1;

    // Check byte 5: should be b1 XOR 0xC3 (MSB=0) or b1 (MSB=1)
    if(msb == 0) {
        if(d[5] != (d[1] ^ 0xC3)) return false;
    } else {
        if(d[5] != d[1]) return false;
    }

    // Check bytes 6-9 vs 1-4 XOR mask consistency
    // b1^b6 should be a known mask
    uint8_t b1_xor_b6 = d[1] ^ d[6];
    if(msb == 0) {
        if(b1_xor_b6 != 0x04 && b1_xor_b6 != 0x08) return false;
    } else {
        if(b1_xor_b6 != 0x62) return false;
    }

    // Check bytes 2-4 vs 7-9 XOR mask is consistent
    // The XOR mask for bytes 2-4 vs 7-9 should be the same across all 3 pairs
    uint8_t mask2 = d[2] ^ d[7];
    uint8_t mask3 = d[3] ^ d[8];
    uint8_t mask4 = d[4] ^ d[9];

    // Masks should be one of the known patterns
    if(msb == 0) {
        if(mask2 != 0x63 || mask3 != 0x59 || mask4 != 0x46) return false;
    } else {
        // MSB=1 masks: 9A C6 20 or 9A C6 10
        if(mask2 != 0x9A || mask3 != 0xC6) return false;
        if(mask4 != 0x20 && mask4 != 0x10) return false;
    }

    return true;
}

static void chrysler_rebuild_raw_data(SubGhzProtocolDecoderChrysler* instance) {
    memset(instance->raw_data, 0, sizeof(instance->raw_data));
    uint64_t key = instance->generic.data;
    for(int i = 0; i < 8; i++) {
        instance->raw_data[i] = (uint8_t)(key >> (56 - i * 8));
    }
    instance->bit_count = instance->generic.data_count_bit;
}

void* subghz_protocol_decoder_chrysler_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderChrysler* instance = calloc(1, sizeof(SubGhzProtocolDecoderChrysler));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_chrysler;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_chrysler_free(void* context) {
    furi_check(context);
    free(context);
}

void subghz_protocol_decoder_chrysler_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderChrysler* instance = context;
    instance->decoder_state = ChryslerDecoderStepReset;
    instance->preamble_count = 0;
    instance->bit_count = 0;
    instance->te_last = 0;
    instance->generic.data = 0;
    memset(instance->raw_data, 0, sizeof(instance->raw_data));
}

void subghz_protocol_decoder_chrysler_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderChrysler* instance = context;

    switch(instance->decoder_state) {
    case ChryslerDecoderStepReset:
        if(level && duration <= CHRYSLER_SHORT_MAX && duration > 100) {
            instance->te_last = duration;
            instance->decoder_state = ChryslerDecoderStepPreamble;
            instance->preamble_count = 1;
        }
        break;

    case ChryslerDecoderStepPreamble:
        if(!level) {
            uint32_t total = instance->te_last + duration;
            if(DURATION_DIFF(total, CHRYSLER_BIT_PERIOD) < CHRYSLER_BIT_TOLERANCE &&
               instance->te_last <= CHRYSLER_SHORT_MAX) {
                instance->preamble_count++;
            } else if(duration > CHRYSLER_PREAMBLE_GAP &&
                      instance->preamble_count >= CHRYSLER_PREAMBLE_MIN) {
                instance->decoder_state = ChryslerDecoderStepGap;
            } else {
                instance->decoder_state = ChryslerDecoderStepReset;
            }
        } else {
            if(duration <= CHRYSLER_SHORT_MAX && duration > 100) {
                instance->te_last = duration;
            } else {
                instance->decoder_state = ChryslerDecoderStepReset;
            }
        }
        break;

    case ChryslerDecoderStepGap:
        if(level) {
            instance->te_last = duration;
            instance->bit_count = 0;
            memset(instance->raw_data, 0, sizeof(instance->raw_data));
            instance->decoder_state = ChryslerDecoderStepData;
        } else {
            instance->decoder_state = ChryslerDecoderStepReset;
        }
        break;

    case ChryslerDecoderStepData:
        if(level) {
            instance->te_last = duration;
        } else {
            uint32_t total = instance->te_last + duration;
            if(DURATION_DIFF(total, CHRYSLER_BIT_PERIOD) < CHRYSLER_BIT_TOLERANCE) {
                bool bit_val = (instance->te_last >= CHRYSLER_LONG_MIN);

                if(instance->bit_count < CHRYSLER_DATA_BITS) {
                    uint8_t byte_idx = instance->bit_count / 8;
                    uint8_t bit_pos = 7 - (instance->bit_count % 8);
                    if(bit_val) {
                        instance->raw_data[byte_idx] |= (1 << bit_pos);
                    }
                    instance->bit_count++;
                }

                if(instance->bit_count == CHRYSLER_DATA_BITS) {
                    if(chrysler_validate(instance)) {
                        chrysler_parse_data(instance);
                        if(instance->base.callback) {
                            instance->base.callback(&instance->base, instance->base.context);
                        }
                    }
                    instance->decoder_state = ChryslerDecoderStepReset;
                }
            } else {
                if(instance->bit_count >= CHRYSLER_DATA_BITS) {
                    if(chrysler_validate(instance)) {
                        chrysler_parse_data(instance);
                        if(instance->base.callback) {
                            instance->base.callback(&instance->base, instance->base.context);
                        }
                    }
                }
                instance->decoder_state = ChryslerDecoderStepReset;
            }
        }
        break;
    }
}

uint8_t subghz_protocol_decoder_chrysler_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderChrysler* instance = context;
    SubGhzBlockDecoder dec = {
        .decode_data = instance->generic.data,
        .decode_count_bit = instance->generic.data_count_bit > 64 ? 64 : instance->generic.data_count_bit,
    };
    return subghz_protocol_blocks_get_hash_data(&dec, (dec.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_chrysler_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderChrysler* instance = context;

    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);

    if(ret == SubGhzProtocolStatusOk) {
        uint32_t extra = ((uint32_t)instance->raw_data[8] << 8) | instance->raw_data[9];
        flipper_format_write_uint32(flipper_format, "Extra", &extra, 1);
    }

    return ret;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_chrysler_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderChrysler* instance = context;

    SubGhzProtocolStatus ret =
        subghz_block_generic_deserialize(&instance->generic, flipper_format);

    if(ret == SubGhzProtocolStatusOk) {
        chrysler_rebuild_raw_data(instance);

        uint32_t extra = 0;
        if(flipper_format_read_uint32(flipper_format, "Extra", &extra, 1)) {
            instance->raw_data[8] = (extra >> 8) & 0xFF;
            instance->raw_data[9] = extra & 0xFF;
        }
    }

    return ret;
}

static const char* chrysler_button_name(uint8_t btn) {
    switch(btn) {
    case 0x01:
        return "Lock";
    case 0x02:
        return "Unlock";
    default:
        return "Unknown";
    }
}

void subghz_protocol_decoder_chrysler_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderChrysler* instance = context;

    uint8_t* d = instance->raw_data;
    uint8_t cnt_raw = (d[0] >> 4) & 0xF;
    uint8_t cnt = chrysler_reverse_nibble(cnt_raw);
    uint8_t dev_id = d[0] & 0xF;
    uint8_t msb = (d[0] >> 7) & 1;

    uint8_t b1_xor_b6 = d[1] ^ d[6];
    uint8_t btn = instance->generic.btn;
    if(msb == 0) {
        if(b1_xor_b6 == 0x04)
            btn = 0x01;
        else if(b1_xor_b6 == 0x08)
            btn = 0x02;
    }

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Raw:%02X%02X%02X%02X%02X %02X%02X%02X%02X%02X\r\n"
        "Cnt:%X Btn:%s Dev:%X\r\n"
        "Sn:%08lX\r\n",
        instance->generic.protocol_name,
        (int)instance->generic.data_count_bit,
        d[0], d[1], d[2], d[3], d[4],
        d[5], d[6], d[7], d[8], d[9],
        (unsigned)cnt,
        chrysler_button_name(btn),
        (unsigned)dev_id,
        (unsigned long)instance->generic.serial);
}
