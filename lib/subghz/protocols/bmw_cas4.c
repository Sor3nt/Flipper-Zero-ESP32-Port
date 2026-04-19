#include "bmw_cas4.h"

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "BmwCas4"

static const SubGhzBlockConst subghz_protocol_bmw_cas4_const = {
    .te_short = 500,
    .te_long = 1000,
    .te_delta = 150,
    .min_count_bit_for_found = 64,
};

#define BMW_CAS4_PREAMBLE_PULSE_MIN 300u
#define BMW_CAS4_PREAMBLE_PULSE_MAX 700u
#define BMW_CAS4_PREAMBLE_MIN       10u
#define BMW_CAS4_DATA_BITS           64u
#define BMW_CAS4_GAP_MIN             1800u
#define BMW_CAS4_BYTE0_MARKER        0x30u
#define BMW_CAS4_BYTE6_MARKER        0xC5u

struct SubGhzProtocolDecoderBmwCas4 {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
    ManchesterState manchester_state;
    uint8_t decoder_state;
    uint16_t preamble_count;
    uint8_t raw_data[8];
    uint8_t bit_count;
    uint32_t te_last;
};

struct SubGhzProtocolEncoderBmwCas4 {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
};

typedef enum {
    BmwCas4DecoderStepReset = 0,
    BmwCas4DecoderStepPreamble,
    BmwCas4DecoderStepData,
} BmwCas4DecoderStep;

const SubGhzProtocolDecoder subghz_protocol_bmw_cas4_decoder = {
    .alloc = subghz_protocol_decoder_bmw_cas4_alloc,
    .free = subghz_protocol_decoder_bmw_cas4_free,
    .feed = subghz_protocol_decoder_bmw_cas4_feed,
    .reset = subghz_protocol_decoder_bmw_cas4_reset,
    .get_hash_data = subghz_protocol_decoder_bmw_cas4_get_hash_data,
    .serialize = subghz_protocol_decoder_bmw_cas4_serialize,
    .deserialize = subghz_protocol_decoder_bmw_cas4_deserialize,
    .get_string = subghz_protocol_decoder_bmw_cas4_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_bmw_cas4_encoder = {
    .alloc = subghz_protocol_encoder_bmw_cas4_alloc,
    .free = subghz_protocol_encoder_bmw_cas4_free,
    .deserialize = subghz_protocol_encoder_bmw_cas4_deserialize,
    .stop = subghz_protocol_encoder_bmw_cas4_stop,
    .yield = subghz_protocol_encoder_bmw_cas4_yield,
};

const SubGhzProtocol subghz_protocol_bmw_cas4 = {
    .name = BMW_CAS4_PROTOCOL_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send |
            SubGhzProtocolFlag_Automotive,
    .decoder = &subghz_protocol_bmw_cas4_decoder,
    .encoder = &subghz_protocol_bmw_cas4_encoder,
};

// Encoder stubs

void* subghz_protocol_encoder_bmw_cas4_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderBmwCas4* instance = calloc(1, sizeof(SubGhzProtocolEncoderBmwCas4));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_bmw_cas4;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.is_running = false;
    instance->encoder.size_upload = 1;
    instance->encoder.upload = malloc(sizeof(LevelDuration));
    furi_check(instance->encoder.upload);
    return instance;
}

void subghz_protocol_encoder_bmw_cas4_free(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderBmwCas4* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

SubGhzProtocolStatus
    subghz_protocol_encoder_bmw_cas4_deserialize(void* context, FlipperFormat* flipper_format) {
    UNUSED(context);
    UNUSED(flipper_format);
    return SubGhzProtocolStatusError;
}

void subghz_protocol_encoder_bmw_cas4_stop(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderBmwCas4* instance = context;
    instance->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_bmw_cas4_yield(void* context) {
    UNUSED(context);
    return level_duration_reset();
}

// Decoder

static void bmw_cas4_rebuild_raw_data(SubGhzProtocolDecoderBmwCas4* instance) {
    memset(instance->raw_data, 0, sizeof(instance->raw_data));
    uint64_t key = instance->generic.data;
    for(int i = 0; i < 8; i++) {
        instance->raw_data[i] = (uint8_t)(key >> (56 - i * 8));
    }
    instance->bit_count = instance->generic.data_count_bit;
}

void* subghz_protocol_decoder_bmw_cas4_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderBmwCas4* instance = calloc(1, sizeof(SubGhzProtocolDecoderBmwCas4));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_bmw_cas4;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_bmw_cas4_free(void* context) {
    furi_check(context);
    free(context);
}

void subghz_protocol_decoder_bmw_cas4_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderBmwCas4* instance = context;
    instance->decoder_state = BmwCas4DecoderStepReset;
    instance->preamble_count = 0;
    instance->bit_count = 0;
    instance->te_last = 0;
    instance->generic.data = 0;
    memset(instance->raw_data, 0, sizeof(instance->raw_data));
    instance->manchester_state = ManchesterStateMid1;
}

void subghz_protocol_decoder_bmw_cas4_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderBmwCas4* instance = context;

    uint32_t te_short = subghz_protocol_bmw_cas4_const.te_short;
    uint32_t te_long = subghz_protocol_bmw_cas4_const.te_long;
    uint32_t te_delta = subghz_protocol_bmw_cas4_const.te_delta;
    uint32_t diff;

    switch(instance->decoder_state) {
    case BmwCas4DecoderStepReset:
        if(level && duration >= BMW_CAS4_PREAMBLE_PULSE_MIN &&
           duration <= BMW_CAS4_PREAMBLE_PULSE_MAX) {
            instance->decoder_state = BmwCas4DecoderStepPreamble;
            instance->preamble_count = 1;
            instance->te_last = duration;
        }
        break;

    case BmwCas4DecoderStepPreamble:
        if(duration >= BMW_CAS4_PREAMBLE_PULSE_MIN &&
           duration <= BMW_CAS4_PREAMBLE_PULSE_MAX) {
            instance->preamble_count++;
            instance->te_last = duration;
        } else if(!level && duration >= BMW_CAS4_GAP_MIN) {
            if(instance->preamble_count >= BMW_CAS4_PREAMBLE_MIN) {
                instance->bit_count = 0;
                instance->generic.data = 0;
                memset(instance->raw_data, 0, sizeof(instance->raw_data));
                manchester_advance(
                    instance->manchester_state,
                    ManchesterEventReset,
                    &instance->manchester_state,
                    NULL);
                instance->decoder_state = BmwCas4DecoderStepData;
            } else {
                instance->decoder_state = BmwCas4DecoderStepReset;
            }
        } else {
            instance->decoder_state = BmwCas4DecoderStepReset;
        }
        break;

    case BmwCas4DecoderStepData: {
        if(instance->bit_count >= BMW_CAS4_DATA_BITS) {
            instance->decoder_state = BmwCas4DecoderStepReset;
            break;
        }

        ManchesterEvent event = ManchesterEventReset;

        diff = (duration > te_short) ? (duration - te_short) : (te_short - duration);
        if(diff < te_delta) {
            event = level ? ManchesterEventShortLow : ManchesterEventShortHigh;
        } else {
            diff = (duration > te_long) ? (duration - te_long) : (te_long - duration);
            if(diff < te_delta) {
                event = level ? ManchesterEventLongLow : ManchesterEventLongHigh;
            }
        }

        if(event != ManchesterEventReset) {
            bool data_bit;
            if(manchester_advance(
                   instance->manchester_state,
                   event,
                   &instance->manchester_state,
                   &data_bit)) {
                uint32_t new_bit = data_bit ? 1 : 0;

                if(instance->bit_count < BMW_CAS4_DATA_BITS) {
                    uint8_t byte_idx = instance->bit_count / 8;
                    uint8_t bit_pos = 7 - (instance->bit_count % 8);
                    if(new_bit) {
                        instance->raw_data[byte_idx] |= (1 << bit_pos);
                    }
                    instance->generic.data = (instance->generic.data << 1) | new_bit;
                }

                instance->bit_count++;

                if(instance->bit_count == BMW_CAS4_DATA_BITS) {
                    if(instance->raw_data[0] == BMW_CAS4_BYTE0_MARKER &&
                       instance->raw_data[6] == BMW_CAS4_BYTE6_MARKER) {
                        instance->generic.data_count_bit = BMW_CAS4_DATA_BITS;
                        if(instance->base.callback) {
                            instance->base.callback(&instance->base, instance->base.context);
                        }
                    }
                    instance->decoder_state = BmwCas4DecoderStepReset;
                }
            }
        } else {
            instance->decoder_state = BmwCas4DecoderStepReset;
        }

        instance->te_last = duration;
        break;
    }
    }
}

uint8_t subghz_protocol_decoder_bmw_cas4_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderBmwCas4* instance = context;
    SubGhzBlockDecoder dec = {
        .decode_data = instance->generic.data,
        .decode_count_bit = instance->generic.data_count_bit,
    };
    return subghz_protocol_blocks_get_hash_data(&dec, (dec.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_bmw_cas4_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderBmwCas4* instance = context;
    return subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    subghz_protocol_decoder_bmw_cas4_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderBmwCas4* instance = context;
    SubGhzProtocolStatus ret =
        subghz_block_generic_deserialize(&instance->generic, flipper_format);
    if(ret == SubGhzProtocolStatusOk) {
        bmw_cas4_rebuild_raw_data(instance);
    }
    return ret;
}

void subghz_protocol_decoder_bmw_cas4_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderBmwCas4* instance = context;

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Raw:%02X %02X%02X%02X%02X%02X %02X %02X\r\n",
        instance->generic.protocol_name,
        (int)instance->generic.data_count_bit,
        instance->raw_data[0],
        instance->raw_data[1], instance->raw_data[2],
        instance->raw_data[3], instance->raw_data[4], instance->raw_data[5],
        instance->raw_data[6],
        instance->raw_data[7]);
}
