#include "tpms_pmv107j.h"

#define TAG "TPMSPmv107j"

// Port of rtl_433/src/devices/tpms_pmv107j.c (Christian W. Zuckschwerdt 2017,
// based on work by Werner Johansson, GPL-2+).
//
// FSK 315 MHz, 8-byte Differential Manchester encoded TPMS data with CRC-8.
// Pacific PMV-107J sensors used by Toyota.
//
// Wire format (66 bits effective, realigned with 6 bits of 0-padding to 9
// bytes including CRC):
//
//   II II II I F* PP NN TT CC
//
//   I  = 28-bit ID
//   F* = 6 bit flags (battery_low, repeat_counter[2], unknown, rapid_change, failed)
//   P  = pressure raw
//   N  = inverted pressure (b[6] == ~b[5]) — used as integrity check
//   T  = temperature, °C + 40
//   C  = CRC-8 (poly 0x13, init 0x00) over bytes 0..7
//
// Preamble (decoded): 6-bit `0b111110` = 0xF8.
//
// We implement Differential Manchester decoding directly from level/duration
// pulses (the Flipper port has no helper for it). State machine:
//   - duration ≈ te_long  → bit had no mid-bit transition → emit "1"
//   - duration ≈ te_short → half-bit traversal. First half: just set "saw
//     start transition"; second half: emit "0".
//
// State starts UNCLOCKED; the first te_long pulse establishes synchronisation
// (the preamble starts with five "1" bits = five te_long durations).

#define PMV107J_PREAMBLE 0xF8U
#define PMV107J_PREAMBLE_BITS 6
#define PMV107J_DATA_BITS 66

static const SubGhzBlockConst tpms_protocol_pmv107j_const = {
    .te_short = 100,
    .te_long = 200,
    .te_delta = 35,
    .min_count_bit_for_found = PMV107J_DATA_BITS,
};

typedef enum {
    Pmv107jDmUnclocked = 0, // not yet seen any te_long → no sync
    Pmv107jDmAtBoundary, // about to start a new bit
    Pmv107jDmAtMid, // saw mid-bit transition, waiting for the next-bit boundary
} Pmv107jDmState;

typedef enum {
    Pmv107jStepReset = 0,
    Pmv107jStepFindPreamble,
    Pmv107jStepData,
} Pmv107jStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;

    Pmv107jDmState dm_state;
    uint8_t preamble_shift;
    uint64_t head_64_bit;
} TPMSProtocolDecoderPmv107j;

const SubGhzProtocolDecoder tpms_protocol_pmv107j_decoder = {
    .alloc = tpms_protocol_decoder_pmv107j_alloc,
    .free = tpms_protocol_decoder_pmv107j_free,
    .feed = tpms_protocol_decoder_pmv107j_feed,
    .reset = tpms_protocol_decoder_pmv107j_reset,
    .get_hash_data = tpms_protocol_decoder_pmv107j_get_hash_data,
    .serialize = tpms_protocol_decoder_pmv107j_serialize,
    .deserialize = tpms_protocol_decoder_pmv107j_deserialize,
    .get_string = tpms_protocol_decoder_pmv107j_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_pmv107j_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol tpms_protocol_pmv107j = {
    .name = TPMS_PROTOCOL_PMV107J_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Sensors | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load,
    .decoder = &tpms_protocol_pmv107j_decoder,
    .encoder = &tpms_protocol_pmv107j_encoder,
};

void* tpms_protocol_decoder_pmv107j_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderPmv107j* instance = malloc(sizeof(TPMSProtocolDecoderPmv107j));
    instance->base.protocol = &tpms_protocol_pmv107j;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_pmv107j_free(void* context) {
    furi_assert(context);
    free(context);
}

static void pmv107j_reset_state(TPMSProtocolDecoderPmv107j* instance) {
    instance->decoder.parser_step = Pmv107jStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->head_64_bit = 0;
    instance->dm_state = Pmv107jDmUnclocked;
    instance->preamble_shift = 0;
}

void tpms_protocol_decoder_pmv107j_reset(void* context) {
    furi_assert(context);
    pmv107j_reset_state(context);
}

// Returns -1 = no bit yet, 0 or 1 = decoded bit, -2 = sync lost / reset.
static int pmv107j_dm_advance(TPMSProtocolDecoderPmv107j* instance, uint32_t duration) {
    bool is_short = DURATION_DIFF(duration, tpms_protocol_pmv107j_const.te_short) <
                    tpms_protocol_pmv107j_const.te_delta;
    bool is_long = DURATION_DIFF(duration, tpms_protocol_pmv107j_const.te_long) <
                   tpms_protocol_pmv107j_const.te_delta;

    if(!is_short && !is_long) {
        instance->dm_state = Pmv107jDmUnclocked;
        return -2;
    }

    if(is_long) {
        // Full bit period without a mid transition → "1".
        instance->dm_state = Pmv107jDmAtBoundary;
        return 1;
    }

    // is_short
    if(instance->dm_state == Pmv107jDmUnclocked) {
        // Can't tell whether this is start- or mid-half until we've seen a
        // te_long anchor; drop until we sync.
        return -1;
    }
    if(instance->dm_state == Pmv107jDmAtBoundary) {
        // First half of a bit with mid-transition.
        instance->dm_state = Pmv107jDmAtMid;
        return -1;
    }
    // At mid → second half done, full bit consumed with mid-transition → "0".
    instance->dm_state = Pmv107jDmAtBoundary;
    return 0;
}

static void pmv107j_get_bytes(TPMSProtocolDecoderPmv107j* instance, uint8_t b[9]) {
    // 66 bits collected: head lower 2 = stream bits 0..1 → b[0] lower 2 bits;
    // decode_data 64 bits = stream bits 2..65 → b[1..8].
    b[0] = (uint8_t)(instance->head_64_bit & 0x03);
    for(int i = 0; i < 8; i++) {
        b[i + 1] = (instance->decoder.decode_data >> (8 * (7 - i))) & 0xFF;
    }
}

static bool pmv107j_check(TPMSProtocolDecoderPmv107j* instance) {
    uint8_t b[9];
    pmv107j_get_bytes(instance, b);
    if(subghz_protocol_blocks_crc8(b, 8, 0x13, 0x00) != b[8]) return false;
    // Pressure plausibility: b[6] should be inverted b[5].
    if((b[5] ^ 0xFF) != b[6]) return false;
    return true;
}

static void pmv107j_analyze(TPMSProtocolDecoderPmv107j* instance) {
    uint8_t b[9];
    pmv107j_get_bytes(instance, b);
    // 28-bit ID = b[0]<<26 | b[1]<<18 | b[2]<<10 | b[3]<<2 | b[4]>>6
    uint32_t id = ((uint32_t)b[0] << 26) | ((uint32_t)b[1] << 18) | ((uint32_t)b[2] << 10) |
                  ((uint32_t)b[3] << 2) | (b[4] >> 6);
    instance->generic.id = id;
    instance->generic.battery_low = (b[4] & 0x20) >> 5;
    // pressure_kpa = (pressure_raw - 40) * 2.48
    float kpa = ((float)b[5] - 40.0f) * 2.48f;
    instance->generic.pressure = kpa * 0.01f;
    instance->generic.temperature = (float)((int)b[7] - 40);
}

void tpms_protocol_decoder_pmv107j_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    UNUSED(level);
    TPMSProtocolDecoderPmv107j* instance = context;

    int bit = pmv107j_dm_advance(instance, duration);
    if(bit == -2) {
        pmv107j_reset_state(instance);
        return;
    }
    if(bit == -1) return;

    switch(instance->decoder.parser_step) {
    case Pmv107jStepReset:
        instance->preamble_shift = 0;
        instance->decoder.parser_step = Pmv107jStepFindPreamble;
        /* fallthrough */
    case Pmv107jStepFindPreamble:
        instance->preamble_shift =
            (uint8_t)(((instance->preamble_shift << 1) | (bit & 0x1)) & 0x3F);
        if(instance->preamble_shift == PMV107J_PREAMBLE) {
            instance->decoder.parser_step = Pmv107jStepData;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->head_64_bit = 0;
        }
        break;

    case Pmv107jStepData:
        subghz_protocol_blocks_add_to_128_bit(
            &instance->decoder, (uint8_t)bit, &instance->head_64_bit);
        if(instance->decoder.decode_count_bit ==
           tpms_protocol_pmv107j_const.min_count_bit_for_found) {
            if(pmv107j_check(instance)) {
                instance->generic.data = instance->decoder.decode_data;
                instance->generic.data_count_bit = instance->decoder.decode_count_bit;
                pmv107j_analyze(instance);
                if(instance->base.callback)
                    instance->base.callback(&instance->base, instance->base.context);
            }
            pmv107j_reset_state(instance);
        }
        break;
    }
}

uint8_t tpms_protocol_decoder_pmv107j_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderPmv107j* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_pmv107j_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderPmv107j* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_pmv107j_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderPmv107j* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, tpms_protocol_pmv107j_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_pmv107j_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderPmv107j* instance = context;
    furi_string_printf(
        output,
        "%s\r\n"
        "Id:0x%08lX\r\n"
        "Bat:%s\r\n"
        "Temp:%2.0f C Bar:%2.1f",
        instance->generic.protocol_name,
        instance->generic.id,
        instance->generic.battery_low ? "low" : "ok",
        (double)instance->generic.temperature,
        (double)instance->generic.pressure);
}
