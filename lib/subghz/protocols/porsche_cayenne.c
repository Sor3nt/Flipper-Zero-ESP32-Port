#include "porsche_cayenne.h"
#include "../blocks/custom_btn_i.h"

#define TAG "PorscheCayenneProtocol"

static const SubGhzBlockConst subghz_protocol_porsche_cayenne_const = {
    .te_short = 1680,  // bit-0 LOW duration (also bit-1 HIGH duration)
    .te_long  = 3370,  // bit-1 LOW duration (also bit-0 HIGH duration, preamble half-period)
    .te_delta =  500,
    .min_count_bit_for_found = 64,
};

#define PC_TE_SYNC   3370u  // preamble: equal-period LOW+HIGH (from LONG timing)
#define PC_TE_GAP    5930u  // inter-burst gap LOW+HIGH
#define PC_SYNC_MIN    15   // minimum sync pulses before we trust the preamble
#define PC_SYNC_COUNT  73   // actual preamble pulse-pair count in the firmware
#define PC_UPLOAD_SIZE 1300 // 4 frames × (73*2 + 2 + 64*2) + margin

// =============================================================================
// Implements the 24-bit rotating-register VAG rolling-code cipher used by
// Porsche Cayenne / VW Touareg / Audi 
//
// Arguments:
//   serial24   – 24-bit serial number (bits 23:0)
//   btn        – 4-bit button code (upper nibble of pkt[0])
//   counter    – 16-bit rolling counter; the cipher increments it internally by 1
//   frame_type – 3-bit value: 0b010 / 0b001 / 0b100 (set in lower nibble of pkt[0])
//   pkt[8]     – output: fully assembled 8-byte packet ready for OOK transmission
//
// Packet layout (byte order pkt[0] -> pkt[7], transmitted MSB-first per byte):
//   pkt[0] : (btn << 4) | frame_type          — identity / frame marker
//   pkt[1] : serial[23:16]                    — serial MSB
//   pkt[2] : serial[15:8]                     — serial middle
//   pkt[3] : serial[7:0]                      — serial LSB
//   pkt[4..7] : rolling-code authentication bytes (cipher output)
// =============================================================================

static void porsche_cayenne_compute_frame(
    uint32_t serial24,
    uint8_t btn,
    uint16_t counter,
    uint8_t frame_type,
    uint8_t* pkt) {

    // Extract serial bytes (b1 = MSB, b3 = LSB — matching RAM_71/72/73 layout)
    uint8_t b0 = (uint8_t)((btn << 4) | (frame_type & 0x07));
    uint8_t b1 = (serial24 >> 16) & 0xFF;
    uint8_t b2 = (serial24 >> 8) & 0xFF;
    uint8_t b3 = serial24 & 0xFF;

    // Internal counter increment (firmware @ 0x14122)
    uint16_t cnt = counter + 1;
    uint8_t cnt_lo = cnt & 0xFF;
    uint8_t cnt_hi = (cnt >> 8) & 0xFF;

    // Seed 24-bit register (firmware @ 0x1412A):
    //   r_h <- b3 (serial LSB),  r_m <- b1 (serial MSB),  r_l <- b2 (serial mid)
    uint8_t r_h = b3;
    uint8_t r_m = b1;
    uint8_t r_l = b2;

    // 24-bit left circular rotation (firmware inner loop @ 0x14154):
    //   new_r_h bit 0 <- old r_m MSB
    //   new_r_m bit 0 <- old r_l MSB
    //   new_r_l bit 0 <- old r_h MSB  (wrap-around)
#define ROTATE24(rh, rm, rl) do { \
        uint8_t _ch = (uint8_t)(((rh) >> 7) & 1u); \
        uint8_t _cm = (uint8_t)(((rm) >> 7) & 1u); \
        uint8_t _cl = (uint8_t)(((rl) >> 7) & 1u); \
        (rh) = (uint8_t)(((rh) << 1) | _cm); \
        (rm) = (uint8_t)(((rm) << 1) | _cl); \
        (rl) = (uint8_t)(((rl) << 1) | _ch); \
    } while(0)

    // Loop 1: 4 fixed rotations (firmware @ 0x14136, counter 0..3 inclusive)
    for(int i = 0; i < 4; i++) ROTATE24(r_h, r_m, r_l);

    // Loop 2: cnt_lo additional rotations (firmware @ 0x14194, counter 0..cnt_lo-1)
    for(int j = 0; j < cnt_lo; j++) ROTATE24(r_h, r_m, r_l);

#undef ROTATE24

    // -------------------------------------------------------------------------
    // Intermediate values (firmware @ 0x141F2)
    // -------------------------------------------------------------------------

    // 9A: XOR of r_h with base byte (firmware @ 0x141F4-141F8)
    uint8_t a9A = r_h ^ b0;

    // -------------------------------------------------------------------------
    // 9B: assembled from three masked slices of (~cnt_lo / ~cnt_hi) XOR r_m
    // (firmware @ 0x141FE-14256)
    //
    // The PIC18 rlcf / comf sequences are equivalent to simple shifts because
    // the carry bits fed in are always masked out by the subsequent ANDs.
    //
    // Verified against disassembly at:
    //   0x141FE  part-1: (~cnt_lo << 2) & 0xFC ^ r_m -> & 0xCC
    //   0x14218  part-2: (~cnt_hi << 2) & 0xFC ^ r_m -> & 0x30
    //   0x14232  part-3: (~cnt_hi >> 6) & 0x03  ^ r_m -> & 0x03
    // -------------------------------------------------------------------------
    uint8_t nb9B_p1 = (uint8_t)((~cnt_lo << 2) & 0xFC) ^ r_m; nb9B_p1 &= 0xCC;
    uint8_t nb9B_p2 = (uint8_t)((~cnt_hi << 2) & 0xFC) ^ r_m; nb9B_p2 &= 0x30;
    uint8_t nb9B_p3 = (uint8_t)((~cnt_hi >> 6) & 0x03) ^ r_m; nb9B_p3 &= 0x03;
    uint8_t a9B = nb9B_p1 | nb9B_p2 | nb9B_p3;

    // -------------------------------------------------------------------------
    // 9C: assembled from three masked slices of (~cnt_lo / ~cnt_hi) XOR r_l
    // (firmware @ 0x1425A-142B2)
    //
    //   0x1425A  part-1: (~cnt_lo >> 2) & 0x3F ^ r_l -> & 0x33
    //   0x14274  part-2: (~cnt_hi & 0x03) << 6  ^ r_l -> & 0xC0
    //   0x14290  part-3: (~cnt_hi >> 2) & 0x3F  ^ r_l -> & 0x0C
    // -------------------------------------------------------------------------
    uint8_t nb9C_p1 = (uint8_t)((~cnt_lo >> 2) & 0x3F) ^ r_l; nb9C_p1 &= 0x33;
    uint8_t nb9C_p2 = (uint8_t)((~cnt_hi & 0x03) << 6) ^ r_l; nb9C_p2 &= 0xC0;
    uint8_t nb9C_p3 = (uint8_t)((~cnt_hi >> 2) & 0x3F) ^ r_l; nb9C_p3 &= 0x0C;
    uint8_t a9C = nb9C_p1 | nb9C_p2 | nb9C_p3;

    // -------------------------------------------------------------------------
    // Output byte assembly (firmware @ 0x142B4-14362)
    //
    // pkt[4] (RAM 0x54): 9A right-shifted 2 in bits 5:0;
    //   bit 6 = NOT cnt_lo[0], bit 7 = NOT cnt_lo[1]  (@ 0x142C4-142D0)
    // pkt[5] (RAM 0x55): four slices of ~cnt_lo + 9A bits 1:0 + 9B bits 3:2  (@ 0x142D2-130C)
    // pkt[6] (RAM 0x56): 9B bits 1:0 shifted to 7:6; 9C bits 5:2 shifted down; ~cnt_lo bits 5:4 (@ 0x130E-14344)
    // pkt[7] (RAM 0x57): upper nibble of 9B -> low nibble; lower nibble of 9C -> high nibble  (@ 0x14346-14362)
    // -------------------------------------------------------------------------
    pkt[0] = b0;
    pkt[1] = b1;
    pkt[2] = b2;
    pkt[3] = b3;

    pkt[4] = (uint8_t)(((a9A >> 2) & 0x3F) | ((~cnt_lo & 0x03u) << 6));

    pkt[5] = (uint8_t)(
        (~cnt_lo & 0xC0u) |
        ((a9A & 0x03u) << 4) |
        (a9B & 0x0Cu) |
        ((~cnt_lo >> 2) & 0x03u));

    pkt[6] = (uint8_t)(
        ((a9B & 0x03u) << 6) |
        ((a9C >> 2) & 0x3Cu) |
        ((~cnt_lo >> 4) & 0x03u));

    pkt[7] = (uint8_t)(((a9B >> 4) & 0x0Fu) | ((a9C & 0x0Fu) << 4));
}

// =============================================================================
// DECODER
// =============================================================================

typedef enum {
    PCDecoderStepReset = 0,
    PCDecoderStepSync,    // counting equal-period sync pulses
    PCDecoderStepGapHigh, // absorbing gap HIGH
    PCDecoderStepGapLow,  // absorbing gap LOW
    PCDecoderStepData,    // collecting 64 data bits
} PCDecoderStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder        decoder;   // provides parser_step + te_last
    SubGhzBlockGeneric        generic;   // serial / btn / cnt / data

    uint16_t sync_count;
    uint64_t raw_data;
    uint8_t  bit_count;
} SubGhzProtocolDecoderPorscheCayenne;

typedef struct {
    SubGhzProtocolEncoderBase  base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric         generic;
} SubGhzProtocolEncoderPorscheCayenne;

// Forward declarations
const SubGhzProtocolDecoder subghz_protocol_porsche_cayenne_decoder;
const SubGhzProtocolEncoder subghz_protocol_porsche_cayenne_encoder;

// --- Decoder alloc / free / reset ---

void* subghz_protocol_decoder_porsche_cayenne_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderPorscheCayenne* instance =
        malloc(sizeof(SubGhzProtocolDecoderPorscheCayenne));
    instance->base.protocol    = &subghz_protocol_porsche_cayenne;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_porsche_cayenne_free(void* context) {
    furi_check(context);
    free(context);
}

void subghz_protocol_decoder_porsche_cayenne_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderPorscheCayenne* instance = context;
    instance->decoder.parser_step = PCDecoderStepReset;
    instance->decoder.te_last     = 0;
    instance->sync_count          = 0;
    instance->raw_data            = 0;
    instance->bit_count           = 0;
}

// --- Decoder feed state machine ---
//
// Signal is OOK, signal starts LOW.
// Preamble: 73 × (LONG LOW + LONG HIGH) — equal period 3370 µs each half.
// Gap:      GAP LOW (5930 µs) + GAP HIGH (5930 µs).
// Data bit: (SHORT LOW + LONG HIGH) = 0, (LONG LOW + SHORT HIGH) = 1.
//
// We decode: on every HIGH pulse decide bit from its length after a LOW.

void subghz_protocol_decoder_porsche_cayenne_feed(
    void* context,
    bool level,
    uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderPorscheCayenne* instance = context;

    const uint32_t te_s  = subghz_protocol_porsche_cayenne_const.te_short;
    const uint32_t te_l  = subghz_protocol_porsche_cayenne_const.te_long;
    const uint32_t te_d  = subghz_protocol_porsche_cayenne_const.te_delta;

    switch(instance->decoder.parser_step) {

    case PCDecoderStepReset:
        // Wait for a LOW ≈ SYNC (LONG duration) to start preamble counting
        if(!level && DURATION_DIFF(duration, PC_TE_SYNC) < te_d) {
            instance->sync_count          = 1;
            instance->decoder.parser_step = PCDecoderStepSync;
        }
        break;

    case PCDecoderStepSync:
        if(level) {
            // HIGH pulse of a sync pair — must also be ≈ LONG
            if(DURATION_DIFF(duration, PC_TE_SYNC) < te_d) {
                // good sync HIGH — wait for next LOW
            } else if(instance->sync_count >= PC_SYNC_MIN &&
                      DURATION_DIFF(duration, PC_TE_GAP) < te_d) {
                // This HIGH is a GAP HIGH — preamble done, absorb gap LOW next
                instance->decoder.parser_step = PCDecoderStepGapLow;
            } else {
                instance->decoder.parser_step = PCDecoderStepReset;
            }
        } else {
            // LOW pulse
            if(DURATION_DIFF(duration, PC_TE_SYNC) < te_d) {
                instance->sync_count++;
            } else if(instance->sync_count >= PC_SYNC_MIN &&
                      DURATION_DIFF(duration, PC_TE_GAP) < te_d) {
                // GAP LOW — next should be GAP HIGH
                instance->decoder.parser_step = PCDecoderStepGapHigh;
            } else {
                instance->decoder.parser_step = PCDecoderStepReset;
            }
        }
        break;

    case PCDecoderStepGapHigh:
        // Absorb the GAP HIGH
        if(level && DURATION_DIFF(duration, PC_TE_GAP) < te_d) {
            instance->raw_data            = 0;
            instance->bit_count           = 0;
            instance->decoder.parser_step = PCDecoderStepData;
        } else {
            instance->decoder.parser_step = PCDecoderStepReset;
        }
        break;

    case PCDecoderStepGapLow:
        // Absorb the GAP LOW
        if(!level && DURATION_DIFF(duration, PC_TE_GAP) < te_d) {
            instance->raw_data            = 0;
            instance->bit_count           = 0;
            instance->decoder.parser_step = PCDecoderStepData;
        } else {
            instance->decoder.parser_step = PCDecoderStepReset;
        }
        break;

    case PCDecoderStepData:
        // Bits are encoded by the HIGH pulse length following the LOW:
        //   LOW ≈ SHORT -> HIGH ≈ LONG -> bit 0
        //   LOW ≈ LONG  -> HIGH ≈ SHORT -> bit 1
        // We key off the HIGH pulse to decide; the preceding LOW was te_last.
        if(level) {
            bool bit;
            if(DURATION_DIFF(instance->decoder.te_last, te_s) < te_d &&
               DURATION_DIFF(duration, te_l) < te_d) {
                bit = false; // bit 0
            } else if(
                DURATION_DIFF(instance->decoder.te_last, te_l) < te_d &&
                DURATION_DIFF(duration, te_s) < te_d) {
                bit = true; // bit 1
            } else {
                instance->decoder.parser_step = PCDecoderStepReset;
                break;
            }
            instance->raw_data = (instance->raw_data << 1) | (bit ? 1u : 0u);
            instance->bit_count++;

            if(instance->bit_count == 64) {
                // Full packet received — decode
                uint8_t pkt[8];
                uint64_t raw = instance->raw_data;
                for(int i = 7; i >= 0; i--) {
                    pkt[i] = raw & 0xFF;
                    raw >>= 8;
                }

                instance->generic.data           = instance->raw_data;
                instance->generic.data_count_bit = 64;
                instance->generic.serial         = ((uint32_t)pkt[1] << 16) |
                                                   ((uint32_t)pkt[2] << 8)  |
                                                   pkt[3];
                instance->generic.btn            = pkt[0] >> 4;

                // Attempt partial counter recovery: brute-force cnt_lo 0..255.
                // We know serial/btn; try each cnt_lo and compare pkt[4..7].
                instance->generic.cnt = 0;
                uint8_t try_pkt[8];
                for(uint16_t try_cnt = 1; try_cnt <= 256; try_cnt++) {
                    uint8_t ft = pkt[0] & 0x07;
                    // cipher increments internally, so pass try_cnt - 1
                    porsche_cayenne_compute_frame(
                        instance->generic.serial,
                        instance->generic.btn,
                        (uint16_t)(try_cnt - 1),
                        ft,
                        try_pkt);
                    if(try_pkt[4] == pkt[4] && try_pkt[5] == pkt[5] &&
                       try_pkt[6] == pkt[6] && try_pkt[7] == pkt[7]) {
                        instance->generic.cnt = try_cnt; // the incremented value
                        break;
                    }
                }

                if(instance->base.callback) {
                    instance->base.callback(&instance->base, instance->base.context);
                }
                instance->decoder.parser_step = PCDecoderStepReset;
            }
        } else {
            // Save LOW duration for use when the following HIGH arrives
            instance->decoder.te_last = duration;
        }
        break;
    }
}

// --- Decoder helpers ---

uint8_t subghz_protocol_decoder_porsche_cayenne_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderPorscheCayenne* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder,
        (instance->generic.data_count_bit / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_porsche_cayenne_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderPorscheCayenne* instance = context;

    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);

    if(ret == SubGhzProtocolStatusOk) {
        uint32_t temp = instance->generic.serial & 0xFFFFFF;
        flipper_format_write_uint32(flipper_format, "Serial", &temp, 1);

        temp = instance->generic.btn;
        flipper_format_write_uint32(flipper_format, "Btn", &temp, 1);

        temp = instance->generic.cnt;
        flipper_format_write_uint32(flipper_format, "Counter", &temp, 1);
    }

    return ret;
}

SubGhzProtocolStatus subghz_protocol_decoder_porsche_cayenne_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderPorscheCayenne* instance = context;

    SubGhzProtocolStatus ret = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        subghz_protocol_porsche_cayenne_const.min_count_bit_for_found);

    if(ret == SubGhzProtocolStatusOk) {
        flipper_format_rewind(flipper_format);
        if(!flipper_format_read_uint32(flipper_format, "Serial", &instance->generic.serial, 1)) {
            FURI_LOG_E(TAG, "Missing Serial field");
            ret = SubGhzProtocolStatusErrorParserKey;
        }
        flipper_format_rewind(flipper_format);
        uint32_t tmp = 0;
        if(!flipper_format_read_uint32(flipper_format, "Btn", &tmp, 1)) {
            FURI_LOG_E(TAG, "Missing Btn field");
            ret = SubGhzProtocolStatusErrorParserKey;
        } else {
            instance->generic.btn = (uint8_t)tmp;
        }
        flipper_format_rewind(flipper_format);
        if(!flipper_format_read_uint32(flipper_format, "Counter", &instance->generic.cnt, 1)) {
            FURI_LOG_E(TAG, "Missing Counter field");
            ret = SubGhzProtocolStatusErrorParserKey;
        }
    }

    return ret;
}

void subghz_protocol_decoder_porsche_cayenne_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderPorscheCayenne* instance = context;

    if(subghz_custom_btn_get_original() == 0) {
        subghz_custom_btn_set_original(instance->generic.btn);
    }
    subghz_custom_btn_set_max(4);

    uint8_t frame_type = (uint8_t)(instance->generic.data >> 56) & 0x07;
    const char* ft_name = "??";
    if(frame_type == 0b010) ft_name = "First";
    else if(frame_type == 0b001) ft_name = "Cont";
    else if(frame_type == 0b100) ft_name = "Final";

    furi_string_cat_printf(
        output,
        "%s 64bit\r\n"
        "Sn:%06lX Btn:%X\r\n"
        "Cnt:%04lX FT:%s\r\n"
        "Raw:%08lX%08lX\r\n",
        instance->generic.protocol_name,
        (unsigned long)(instance->generic.serial & 0xFFFFFF),
        (unsigned int)instance->generic.btn,
        (unsigned long)instance->generic.cnt,
        ft_name,
        (unsigned long)(instance->generic.data >> 32),
        (unsigned long)(instance->generic.data & 0xFFFFFFFF));
}

// =============================================================================
// ENCODER
// =============================================================================

static uint8_t porsche_cayenne_get_btn_code(void) {
    uint8_t custom_btn  = subghz_custom_btn_get();
    uint8_t original_btn = subghz_custom_btn_get_original();
    if(custom_btn == SUBGHZ_CUSTOM_BTN_OK) return original_btn;
    // Map d-pad buttons to common VAG key codes
    if(custom_btn == SUBGHZ_CUSTOM_BTN_UP)    return 0x01; // Lock
    if(custom_btn == SUBGHZ_CUSTOM_BTN_DOWN)  return 0x02; // Unlock
    if(custom_btn == SUBGHZ_CUSTOM_BTN_LEFT)  return 0x04; // Trunk
    if(custom_btn == SUBGHZ_CUSTOM_BTN_RIGHT) return 0x08; // Open
    return original_btn;
}

static void porsche_cayenne_build_upload(SubGhzProtocolEncoderPorscheCayenne* instance) {
    //   Frame 0: frame_type=0b010, counter+1
    //   Frame 1: frame_type=0b001, counter+2
    //   Frame 2: frame_type=0b100, counter+3
    //   Frame 3: frame_type=0b100, counter+4
    // Each frame = 73 SYNC pairs + GAP pair + 64 data bits (MSB first)

    static const uint8_t frame_types[4] = {0b010, 0b001, 0b100, 0b100};

    const uint32_t te_s = subghz_protocol_porsche_cayenne_const.te_short;
    const uint32_t te_l = subghz_protocol_porsche_cayenne_const.te_long;

    uint32_t serial = instance->generic.serial & 0xFFFFFF;
    uint8_t  btn    = porsche_cayenne_get_btn_code();
    uint16_t cnt    = (uint16_t)instance->generic.cnt;

    size_t idx = 0;
    LevelDuration* up = instance->encoder.upload;

    for(int f = 0; f < 4; f++) {
        uint8_t pkt[8];
        // Pass cnt+f; cipher adds 1 internally -> actual cipher counter = cnt+f+1
        porsche_cayenne_compute_frame(serial, btn, (uint16_t)(cnt + (uint16_t)f), frame_types[f], pkt);

        // Preamble: 73 × (LOW LONG + HIGH LONG)
        for(int s = 0; s < PC_SYNC_COUNT; s++) {
            up[idx++] = level_duration_make(false, te_l);
            up[idx++] = level_duration_make(true,  te_l);
        }

        // Gap: LOW GAP + HIGH GAP
        up[idx++] = level_duration_make(false, PC_TE_GAP);
        up[idx++] = level_duration_make(true,  PC_TE_GAP);

        // 64 data bits, MSB first, byte order pkt[0]->pkt[7]
        for(int byte = 0; byte < 8; byte++) {
            for(int bit = 7; bit >= 0; bit--) {
                bool b = (pkt[byte] >> bit) & 1;
                if(b) {
                    // bit 1: LONG LOW + SHORT HIGH
                    up[idx++] = level_duration_make(false, te_l);
                    up[idx++] = level_duration_make(true,  te_s);
                } else {
                    // bit 0: SHORT LOW + LONG HIGH
                    up[idx++] = level_duration_make(false, te_s);
                    up[idx++] = level_duration_make(true,  te_l);
                }
            }
        }
    }

    instance->encoder.size_upload = idx;
    instance->encoder.front       = 0;

    // Advance stored counter by 4 (one per frame)
    instance->generic.cnt = (uint16_t)(cnt + 4);
}

void* subghz_protocol_encoder_porsche_cayenne_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderPorscheCayenne* instance =
        malloc(sizeof(SubGhzProtocolEncoderPorscheCayenne));

    instance->base.protocol          = &subghz_protocol_porsche_cayenne;
    instance->generic.protocol_name  = instance->base.protocol->name;
    instance->encoder.repeat         = 1; // 4-frame burst is sent once per trigger
    instance->encoder.size_upload    = PC_UPLOAD_SIZE;
    instance->encoder.upload         = malloc(PC_UPLOAD_SIZE * sizeof(LevelDuration));
    instance->encoder.is_running     = false;
    instance->encoder.front          = 0;

    return instance;
}

void subghz_protocol_encoder_porsche_cayenne_free(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderPorscheCayenne* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

SubGhzProtocolStatus subghz_protocol_encoder_porsche_cayenne_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolEncoderPorscheCayenne* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    instance->encoder.is_running = false;
    instance->encoder.front      = 0;
    instance->encoder.repeat     = 1;

    do {
        flipper_format_rewind(flipper_format);
        if(!flipper_format_read_uint32(
               flipper_format, "Serial", &instance->generic.serial, 1)) {
            FURI_LOG_E(TAG, "Missing Serial");
            break;
        }

        flipper_format_rewind(flipper_format);
        uint32_t tmp = 0;
        if(!flipper_format_read_uint32(flipper_format, "Btn", &tmp, 1)) {
            FURI_LOG_E(TAG, "Missing Btn");
            break;
        }
        instance->generic.btn = (uint8_t)tmp;

        flipper_format_rewind(flipper_format);
        if(!flipper_format_read_uint32(flipper_format, "Counter", &instance->generic.cnt, 1)) {
            FURI_LOG_E(TAG, "Missing Counter");
            break;
        }

        // Apply custom button if set
        if(subghz_custom_btn_get_original() == 0) {
            subghz_custom_btn_set_original(instance->generic.btn);
        }
        subghz_custom_btn_set_max(4);

        // Build the 4-frame upload array (also advances counter by 4)
        porsche_cayenne_build_upload(instance);

        // Write updated counter back into the file so it persists
        flipper_format_rewind(flipper_format);
        uint32_t new_cnt = instance->generic.cnt;
        flipper_format_insert_or_update_uint32(flipper_format, "Counter", &new_cnt, 1);

        instance->encoder.is_running = true;
        ret = SubGhzProtocolStatusOk;
    } while(false);

    return ret;
}

void subghz_protocol_encoder_porsche_cayenne_stop(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderPorscheCayenne* instance = context;
    instance->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_porsche_cayenne_yield(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderPorscheCayenne* instance = context;

    if(!instance->encoder.is_running || instance->encoder.repeat == 0) {
        instance->encoder.is_running = false;
        return level_duration_reset();
    }

    LevelDuration ret = instance->encoder.upload[instance->encoder.front];

    if(++instance->encoder.front == instance->encoder.size_upload) {
        instance->encoder.repeat--;
        instance->encoder.front = 0;
    }

    return ret;
}

// =============================================================================
// PROTOCOL DESCRIPTOR TABLES
// =============================================================================

const SubGhzProtocolDecoder subghz_protocol_porsche_cayenne_decoder = {
    .alloc         = subghz_protocol_decoder_porsche_cayenne_alloc,
    .free          = subghz_protocol_decoder_porsche_cayenne_free,
    .feed          = subghz_protocol_decoder_porsche_cayenne_feed,
    .reset         = subghz_protocol_decoder_porsche_cayenne_reset,
    .get_hash_data = subghz_protocol_decoder_porsche_cayenne_get_hash_data,
    .serialize     = subghz_protocol_decoder_porsche_cayenne_serialize,
    .deserialize   = subghz_protocol_decoder_porsche_cayenne_deserialize,
    .get_string    = subghz_protocol_decoder_porsche_cayenne_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_porsche_cayenne_encoder = {
    .alloc       = subghz_protocol_encoder_porsche_cayenne_alloc,
    .free        = subghz_protocol_encoder_porsche_cayenne_free,
    .deserialize = subghz_protocol_encoder_porsche_cayenne_deserialize,
    .stop        = subghz_protocol_encoder_porsche_cayenne_stop,
    .yield       = subghz_protocol_encoder_porsche_cayenne_yield,
};

const SubGhzProtocol subghz_protocol_porsche_cayenne = {
    .name = SUBGHZ_PROTOCOL_PORSCHE_CAYENNE_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_868 |
            SubGhzProtocolFlag_AM  | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send | SubGhzProtocolFlag_Automotive,
    .decoder = &subghz_protocol_porsche_cayenne_decoder,
    .encoder = &subghz_protocol_porsche_cayenne_encoder,
};
