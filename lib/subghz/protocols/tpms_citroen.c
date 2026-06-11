#include "tpms_citroen.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "TPMSCitroen"

// Port of rtl_433/src/devices/tpms_citroen.c (Christian W. Zuckschwerdt 2017, GPL-2+)
//
// FSK 433 MHz, Manchester-encoded, 10 byte payload.
// Also Peugeot and likely Fiat, Mitsubishi, VDO-types.
//
// Wire format (after Manchester-decoding):
//   UU IIIIIIII FR PP TT BB CC          (10 bytes total)
//   b[0]     = state (not covered by CRC)
//   b[1..4]  = 32-bit ID
//   b[5]     = flags (high nibble) + repeat (low nibble)
//   b[6]     = pressure raw, 1.364 kPa per step
//   b[7]     = temperature in °C + 50
//   b[8]     = battery (or other)
//   b[9]     = XOR(b[1..9]) == 0
//
// Preamble raw 0x55 0x55 0x55 0x56 → trailing 16-bit pattern 0x5556.

#define CITROEN_PREAMBLE 0x5556U
#define CITROEN_DATA_BITS 80

static const SubGhzBlockConst tpms_protocol_citroen_const = {
    .te_short = 52,
    .te_long = 104,
    .te_delta = 20,
    .min_count_bit_for_found = CITROEN_DATA_BITS,
};

typedef enum {
    CitroenDecoderStepReset = 0,
    CitroenDecoderStepFindPreamble,
    CitroenDecoderStepData,
} CitroenDecoderStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    ManchesterState manchester_saved_state;
    uint16_t header_shift;
    uint64_t head_64_bit;
} TPMSProtocolDecoderCitroen;

const SubGhzProtocolDecoder tpms_protocol_citroen_decoder = {
    .alloc = tpms_protocol_decoder_citroen_alloc,
    .free = tpms_protocol_decoder_citroen_free,
    .feed = tpms_protocol_decoder_citroen_feed,
    .reset = tpms_protocol_decoder_citroen_reset,
    .get_hash_data = tpms_protocol_decoder_citroen_get_hash_data,
    .serialize = tpms_protocol_decoder_citroen_serialize,
    .deserialize = tpms_protocol_decoder_citroen_deserialize,
    .get_string = tpms_protocol_decoder_citroen_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_citroen_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol tpms_protocol_citroen = {
    .name = TPMS_PROTOCOL_CITROEN_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Sensors | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load,
    .decoder = &tpms_protocol_citroen_decoder,
    .encoder = &tpms_protocol_citroen_encoder,
};

void* tpms_protocol_decoder_citroen_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderCitroen* instance = malloc(sizeof(TPMSProtocolDecoderCitroen));
    instance->base.protocol = &tpms_protocol_citroen;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_citroen_free(void* context) {
    furi_assert(context);
    free(context);
}

void tpms_protocol_decoder_citroen_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderCitroen* instance = context;
    instance->decoder.parser_step = CitroenDecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->head_64_bit = 0;
    instance->header_shift = 0;
}

static ManchesterEvent citroen_evt(bool level, uint32_t duration) {
    bool is_long = false;
    if(DURATION_DIFF(duration, tpms_protocol_citroen_const.te_long) <
       tpms_protocol_citroen_const.te_delta * 2) {
        is_long = true;
    } else if(
        DURATION_DIFF(duration, tpms_protocol_citroen_const.te_short) <
        tpms_protocol_citroen_const.te_delta) {
        is_long = false;
    } else {
        return ManchesterEventReset;
    }
    if(level)
        return is_long ? ManchesterEventLongHigh : ManchesterEventShortHigh;
    else
        return is_long ? ManchesterEventLongLow : ManchesterEventShortLow;
}

static void citroen_get_bytes(TPMSProtocolDecoderCitroen* instance, uint8_t b[10]) {
    // 80 bits total: head_64_bit lower 16 bits = b[0..1] (MSB-first),
    // decode_data 64 bits = b[2..9].
    b[0] = (uint8_t)((instance->head_64_bit >> 8) & 0xFF);
    b[1] = (uint8_t)(instance->head_64_bit & 0xFF);
    for(int i = 0; i < 8; i++) {
        b[i + 2] = (instance->decoder.decode_data >> (8 * (7 - i))) & 0xFF;
    }
}

static bool citroen_check_crc(TPMSProtocolDecoderCitroen* instance) {
    uint8_t b[10];
    citroen_get_bytes(instance, b);
    uint8_t x = 0;
    for(int i = 1; i <= 9; i++) x ^= b[i];
    return x == 0;
}

static void citroen_analyze(TPMSProtocolDecoderCitroen* instance) {
    uint8_t b[10];
    citroen_get_bytes(instance, b);
    instance->generic.id =
        ((uint32_t)b[1] << 24) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 8) | b[4];
    // Pressure raw → kPa → bar
    instance->generic.pressure = ((float)b[6] * 1.364f) * 0.01f;
    instance->generic.temperature = (float)((int)b[7] - 50);
    // b[8] is suspected battery byte — without a confirmed bit mapping just keep
    // the raw byte in battery_low for now; the view can show it.
    instance->generic.battery_low = b[8];
}

void tpms_protocol_decoder_citroen_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderCitroen* instance = context;

    ManchesterEvent event = citroen_evt(level, duration);
    if(event == ManchesterEventReset) {
        instance->decoder.parser_step = CitroenDecoderStepReset;
        instance->decoder.decode_data = 0;
        instance->decoder.decode_count_bit = 0;
        instance->head_64_bit = 0;
        instance->header_shift = 0;
        instance->manchester_saved_state = ManchesterStateStart1;
        return;
    }

    bool bit = false;
    bool have_bit = manchester_advance(
        instance->manchester_saved_state, event, &instance->manchester_saved_state, &bit);
    if(!have_bit) return;

    switch(instance->decoder.parser_step) {
    case CitroenDecoderStepReset:
        instance->header_shift = 0;
        instance->manchester_saved_state = ManchesterStateStart1;
        instance->decoder.parser_step = CitroenDecoderStepFindPreamble;
        /* fallthrough */
    case CitroenDecoderStepFindPreamble:
        instance->header_shift = (uint16_t)((instance->header_shift << 1) | (bit ? 1 : 0));
        if(instance->header_shift == CITROEN_PREAMBLE) {
            instance->decoder.parser_step = CitroenDecoderStepData;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->head_64_bit = 0;
        }
        break;

    case CitroenDecoderStepData:
        subghz_protocol_blocks_add_to_128_bit(
            &instance->decoder, bit ? 1 : 0, &instance->head_64_bit);
        if(instance->decoder.decode_count_bit ==
           tpms_protocol_citroen_const.min_count_bit_for_found) {
            if(citroen_check_crc(instance)) {
                instance->generic.data = instance->decoder.decode_data;
                instance->generic.data_count_bit = instance->decoder.decode_count_bit;
                citroen_analyze(instance);
                if(instance->base.callback)
                    instance->base.callback(&instance->base, instance->base.context);
            }
            instance->decoder.parser_step = CitroenDecoderStepReset;
        }
        break;
    }
}

uint8_t tpms_protocol_decoder_citroen_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderCitroen* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_citroen_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderCitroen* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_citroen_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderCitroen* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, tpms_protocol_citroen_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_citroen_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderCitroen* instance = context;
    furi_string_printf(
        output,
        "%s\r\n"
        "Id:0x%08lX\r\n"
        "Temp:%2.0f C Bar:%2.1f",
        instance->generic.protocol_name,
        instance->generic.id,
        (double)instance->generic.temperature,
        (double)instance->generic.pressure);
}
