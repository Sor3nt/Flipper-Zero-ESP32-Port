#include "tpms_renault.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "TPMSRenault"

// Port of rtl_433/src/devices/tpms_renault.c (Christian W. Zuckschwerdt 2017, GPL-2+)
//
// FSK 433 MHz, Manchester-encoded, 9 byte payload, CRC-8 poly 0x07 init 0x00.
// Seen on Renault Clio/Captur/Zoe and Dacia Sandero.
//
// Wire format (after Manchester-decoding):
//   F F/PP PP TT II II II ?? ?? CC   (9 bytes total)
//   b[0]&0xFC = flags
//   pressure_raw 10-bit = (b[0]&0x03) << 8 | b[1]   → kPa = raw * 0.75
//   b[2]      = temperature in °C + 30
//   b[3..5]   = 24-bit ID, little-endian (b[5] is MSB)
//   b[6..7]   = unknown (usually 0xFFFF)
//   b[8]      = CRC-8 (poly 0x07, init 0x00) over b[0..7]
//
// Preamble raw 0x55 0x55 0x55 0x56 → we hunt for the trailing 16-bit pattern 0x5556.

#define RENAULT_PREAMBLE 0x5556U
#define RENAULT_DATA_BITS 72

static const SubGhzBlockConst tpms_protocol_renault_const = {
    .te_short = 52,
    .te_long = 104,
    .te_delta = 20,
    .min_count_bit_for_found = RENAULT_DATA_BITS,
};

typedef enum {
    RenaultDecoderStepReset = 0,
    RenaultDecoderStepFindPreamble,
    RenaultDecoderStepData,
} RenaultDecoderStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    ManchesterState manchester_saved_state;
    uint16_t header_shift;
    uint64_t head_64_bit; // upper bits when frame > 64 bit (see subghz_protocol_blocks_add_to_128_bit)
} TPMSProtocolDecoderRenault;

const SubGhzProtocolDecoder tpms_protocol_renault_decoder = {
    .alloc = tpms_protocol_decoder_renault_alloc,
    .free = tpms_protocol_decoder_renault_free,
    .feed = tpms_protocol_decoder_renault_feed,
    .reset = tpms_protocol_decoder_renault_reset,
    .get_hash_data = tpms_protocol_decoder_renault_get_hash_data,
    .serialize = tpms_protocol_decoder_renault_serialize,
    .deserialize = tpms_protocol_decoder_renault_deserialize,
    .get_string = tpms_protocol_decoder_renault_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_renault_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol tpms_protocol_renault = {
    .name = TPMS_PROTOCOL_RENAULT_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Sensors | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load,
    .decoder = &tpms_protocol_renault_decoder,
    .encoder = &tpms_protocol_renault_encoder,
};

void* tpms_protocol_decoder_renault_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderRenault* instance = malloc(sizeof(TPMSProtocolDecoderRenault));
    instance->base.protocol = &tpms_protocol_renault;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_renault_free(void* context) {
    furi_assert(context);
    free(context);
}

void tpms_protocol_decoder_renault_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderRenault* instance = context;
    instance->decoder.parser_step = RenaultDecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->head_64_bit = 0;
    instance->header_shift = 0;
}

static ManchesterEvent renault_evt(bool level, uint32_t duration) {
    bool is_long = false;
    if(DURATION_DIFF(duration, tpms_protocol_renault_const.te_long) <
       tpms_protocol_renault_const.te_delta * 2) {
        is_long = true;
    } else if(
        DURATION_DIFF(duration, tpms_protocol_renault_const.te_short) <
        tpms_protocol_renault_const.te_delta) {
        is_long = false;
    } else {
        return ManchesterEventReset;
    }
    if(level)
        return is_long ? ManchesterEventLongHigh : ManchesterEventShortHigh;
    else
        return is_long ? ManchesterEventLongLow : ManchesterEventShortLow;
}

static void renault_get_bytes(TPMSProtocolDecoderRenault* instance, uint8_t b[9]) {
    // 72 bits total. add_to_128_bit shifts the upper 8 bits into head_64_bit
    // (low byte = b[0]); the remaining 64 bits are in decode_data (= b[1..8]).
    b[0] = (uint8_t)(instance->head_64_bit & 0xFF);
    for(int i = 0; i < 8; i++) {
        b[i + 1] = (instance->decoder.decode_data >> (8 * (7 - i))) & 0xFF;
    }
}

static bool renault_check_crc(TPMSProtocolDecoderRenault* instance) {
    uint8_t b[9];
    renault_get_bytes(instance, b);
    return subghz_protocol_blocks_crc8(b, 8, 0x07, 0x00) == b[8];
}

static void renault_analyze(TPMSProtocolDecoderRenault* instance) {
    uint8_t b[9];
    renault_get_bytes(instance, b);
    uint16_t p_raw = ((uint16_t)(b[0] & 0x03) << 8) | b[1];
    instance->generic.pressure = (float)p_raw * 0.75f * 0.01f; // kPa → bar
    instance->generic.temperature = (float)((int)b[2] - 30);
    instance->generic.id = ((uint32_t)b[5] << 16) | ((uint32_t)b[4] << 8) | b[3];
    instance->generic.battery_low = TPMS_NO_BATT;
}

void tpms_protocol_decoder_renault_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderRenault* instance = context;

    ManchesterEvent event = renault_evt(level, duration);
    if(event == ManchesterEventReset) {
        instance->decoder.parser_step = RenaultDecoderStepReset;
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
    case RenaultDecoderStepReset:
        instance->header_shift = 0;
        instance->manchester_saved_state = ManchesterStateStart1;
        instance->decoder.parser_step = RenaultDecoderStepFindPreamble;
        /* fallthrough */
    case RenaultDecoderStepFindPreamble:
        instance->header_shift = (uint16_t)((instance->header_shift << 1) | (bit ? 1 : 0));
        if(instance->header_shift == RENAULT_PREAMBLE) {
            instance->decoder.parser_step = RenaultDecoderStepData;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->head_64_bit = 0;
        }
        break;

    case RenaultDecoderStepData:
        subghz_protocol_blocks_add_to_128_bit(
            &instance->decoder, bit ? 1 : 0, &instance->head_64_bit);
        if(instance->decoder.decode_count_bit ==
           tpms_protocol_renault_const.min_count_bit_for_found) {
            if(renault_check_crc(instance)) {
                instance->generic.data = instance->decoder.decode_data;
                instance->generic.data_count_bit = instance->decoder.decode_count_bit;
                renault_analyze(instance);
                if(instance->base.callback)
                    instance->base.callback(&instance->base, instance->base.context);
            }
            instance->decoder.parser_step = RenaultDecoderStepReset;
        }
        break;
    }
}

uint8_t tpms_protocol_decoder_renault_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderRenault* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_renault_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderRenault* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_renault_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderRenault* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, tpms_protocol_renault_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_renault_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderRenault* instance = context;
    furi_string_printf(
        output,
        "%s\r\n"
        "Id:0x%06lX\r\n"
        "Temp:%2.0f C Bar:%2.1f",
        instance->generic.protocol_name,
        instance->generic.id,
        (double)instance->generic.temperature,
        (double)instance->generic.pressure);
}
