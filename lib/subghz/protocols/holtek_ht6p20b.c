#include "holtek_ht6p20b.h"

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

#define TAG "SubGhzProtocolHoltekHt6p20b"

static const SubGhzBlockConst subghz_protocol_holtek_ht6p20b_const = {
    .te_short = 450,
    .te_long = 900,
    .te_delta = 270,
    .min_count_bit_for_found = 28,
};

struct SubGhzProtocolDecoderHoltek_HT6P20B {
    SubGhzProtocolDecoderBase base;

    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    uint32_t te;
    uint32_t last_data;
};

struct SubGhzProtocolEncoderHoltek_HT6P20B {
    SubGhzProtocolEncoderBase base;

    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;

    uint32_t te;
};

typedef enum {
    Holtek_HT6P20BDecoderStepReset = 0,
    Holtek_HT6P20BDecoderStepFoundStartBit,
    Holtek_HT6P20BDecoderStepSaveDuration,
    Holtek_HT6P20BDecoderStepCheckDuration,
} Holtek_HT6P20BDecoderStep;

const SubGhzProtocolDecoder subghz_protocol_holtek_ht6p20b_decoder = {
    .alloc = subghz_protocol_decoder_holtek_ht6p20b_alloc,
    .free = subghz_protocol_decoder_holtek_ht6p20b_free,

    .feed = subghz_protocol_decoder_holtek_ht6p20b_feed,
    .reset = subghz_protocol_decoder_holtek_ht6p20b_reset,

    .get_hash_data = subghz_protocol_decoder_holtek_ht6p20b_get_hash_data,
    .serialize = subghz_protocol_decoder_holtek_ht6p20b_serialize,
    .deserialize = subghz_protocol_decoder_holtek_ht6p20b_deserialize,
    .get_string = subghz_protocol_decoder_holtek_ht6p20b_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_holtek_ht6p20b_encoder = {
    .alloc = subghz_protocol_encoder_holtek_ht6p20b_alloc,
    .free = subghz_protocol_encoder_holtek_ht6p20b_free,

    .deserialize = subghz_protocol_encoder_holtek_ht6p20b_deserialize,
    .stop = subghz_protocol_encoder_holtek_ht6p20b_stop,
    .yield = subghz_protocol_encoder_holtek_ht6p20b_yield,
};

const SubGhzProtocol subghz_protocol_holtek_ht6p20b = {
    .name = SUBGHZ_PROTOCOL_HOLTEK_HT6P20B_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_holtek_ht6p20b_decoder,
    .encoder = &subghz_protocol_holtek_ht6p20b_encoder,
};

void* subghz_protocol_encoder_holtek_ht6p20b_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderHoltek_HT6P20B* instance =
        malloc(sizeof(SubGhzProtocolEncoderHoltek_HT6P20B));

    instance->base.protocol = &subghz_protocol_holtek_ht6p20b;
    instance->generic.protocol_name = instance->base.protocol->name;

    instance->encoder.repeat = 3;
    instance->encoder.size_upload = 58;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void subghz_protocol_encoder_holtek_ht6p20b_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderHoltek_HT6P20B* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

/**
 * Generating an upload from data.
 * @param instance Pointer to a SubGhzProtocolEncoderHoltek_HT6P20B instance
 * @return true On success
 */
static bool
    subghz_protocol_encoder_holtek_ht6p20b_get_upload(SubGhzProtocolEncoderHoltek_HT6P20B* instance) {
    furi_assert(instance);

    size_t index = 0;
    size_t size_upload = (instance->generic.data_count_bit * 2) + 2;
    if(size_upload > instance->encoder.size_upload) {
        FURI_LOG_E(TAG, "Size upload exceeds allocated encoder buffer.");
        return false;
    } else {
        instance->encoder.size_upload = size_upload;
    }

    //Send header
    instance->encoder.upload[index++] = level_duration_make(false, (uint32_t)instance->te * 23);
    //Send start bit
    instance->encoder.upload[index++] = level_duration_make(true, (uint32_t)instance->te);
    //Send key data
    for(uint8_t i = instance->generic.data_count_bit; i > 0; i--) {
        if(bit_read(instance->generic.data, i - 1)) {
            //send bit 1
            instance->encoder.upload[index++] =
                level_duration_make(false, (uint32_t)instance->te * 2);
            instance->encoder.upload[index++] = level_duration_make(true, (uint32_t)instance->te);
        } else {
            //send bit 0
            instance->encoder.upload[index++] = level_duration_make(false, (uint32_t)instance->te);
            instance->encoder.upload[index++] =
                level_duration_make(true, (uint32_t)instance->te * 2);
        }
    }
    return true;
}

SubGhzProtocolStatus
    subghz_protocol_encoder_holtek_ht6p20b_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolEncoderHoltek_HT6P20B* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;
    do {
        ret = subghz_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            subghz_protocol_holtek_ht6p20b_const.min_count_bit_for_found);
        if(ret != SubGhzProtocolStatusOk) {
            break;
        }
        if(!flipper_format_rewind(flipper_format)) {
            FURI_LOG_E(TAG, "Rewind error");
            ret = SubGhzProtocolStatusErrorParserOthers;
            break;
        }
        if(!flipper_format_read_uint32(flipper_format, "TE", (uint32_t*)&instance->te, 1)) {
            FURI_LOG_E(TAG, "Missing TE");
            ret = SubGhzProtocolStatusErrorParserTe;
            break;
        }
        // Optional value
        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        if(!subghz_protocol_encoder_holtek_ht6p20b_get_upload(instance)) {
            ret = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }
        instance->encoder.is_running = true;
    } while(false);

    return ret;
}

void subghz_protocol_encoder_holtek_ht6p20b_stop(void* context) {
    SubGhzProtocolEncoderHoltek_HT6P20B* instance = context;
    instance->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_holtek_ht6p20b_yield(void* context) {
    SubGhzProtocolEncoderHoltek_HT6P20B* instance = context;

    if(instance->encoder.repeat == 0 || !instance->encoder.is_running) {
        instance->encoder.is_running = false;
        return level_duration_reset();
    }

    LevelDuration ret = instance->encoder.upload[instance->encoder.front];

    if(++instance->encoder.front == instance->encoder.size_upload) {
        if(!subghz_block_generic_global.endless_tx) instance->encoder.repeat--;
        instance->encoder.front = 0;
    }

    return ret;
}

void* subghz_protocol_decoder_holtek_ht6p20b_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderHoltek_HT6P20B* instance =
        malloc(sizeof(SubGhzProtocolDecoderHoltek_HT6P20B));
    instance->base.protocol = &subghz_protocol_holtek_ht6p20b;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_holtek_ht6p20b_free(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderHoltek_HT6P20B* instance = context;
    free(instance);
}

void subghz_protocol_decoder_holtek_ht6p20b_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderHoltek_HT6P20B* instance = context;
    instance->decoder.parser_step = Holtek_HT6P20BDecoderStepReset;
}

void subghz_protocol_decoder_holtek_ht6p20b_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderHoltek_HT6P20B* instance = context;

    switch(instance->decoder.parser_step) {
    case Holtek_HT6P20BDecoderStepReset:
        if((!level) && (DURATION_DIFF(duration, subghz_protocol_holtek_ht6p20b_const.te_short * 23) <
                        subghz_protocol_holtek_ht6p20b_const.te_delta * 23)) {
            //Found Preambula
            instance->decoder.parser_step = Holtek_HT6P20BDecoderStepFoundStartBit;
        }
        break;
    case Holtek_HT6P20BDecoderStepFoundStartBit:
        if((level) && (DURATION_DIFF(duration, subghz_protocol_holtek_ht6p20b_const.te_short) <
                       subghz_protocol_holtek_ht6p20b_const.te_delta)) {
            //Found StartBit
            instance->decoder.parser_step = Holtek_HT6P20BDecoderStepSaveDuration;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->te = duration;
        } else {
            instance->decoder.parser_step = Holtek_HT6P20BDecoderStepReset;
        }
        break;
    case Holtek_HT6P20BDecoderStepSaveDuration:
        //save duration
        if(!level) {
            if(duration >= ((uint32_t)subghz_protocol_holtek_ht6p20b_const.te_short * 10 +
                            subghz_protocol_holtek_ht6p20b_const.te_delta)) {
                if(instance->decoder.decode_count_bit ==
                   subghz_protocol_holtek_ht6p20b_const.min_count_bit_for_found) {
                    if((instance->last_data == instance->decoder.decode_data) &&
                       instance->last_data) {
                        instance->te /= (instance->decoder.decode_count_bit * 3 + 1);

                        instance->generic.data = instance->decoder.decode_data;
                        instance->generic.data_count_bit = instance->decoder.decode_count_bit;

                        if(instance->base.callback)
                            instance->base.callback(&instance->base, instance->base.context);
                    }
                    instance->last_data = instance->decoder.decode_data;
                }
                instance->decoder.decode_data = 0;
                instance->decoder.decode_count_bit = 0;
                instance->te = 0;
                instance->decoder.parser_step = Holtek_HT6P20BDecoderStepFoundStartBit;
                break;
            } else {
                instance->decoder.te_last = duration;
                instance->te += duration;
                instance->decoder.parser_step = Holtek_HT6P20BDecoderStepCheckDuration;
            }
        } else {
            instance->decoder.parser_step = Holtek_HT6P20BDecoderStepReset;
        }
        break;
    case Holtek_HT6P20BDecoderStepCheckDuration:
        if(level) {
            instance->te += duration;
            if((DURATION_DIFF(
                    instance->decoder.te_last, subghz_protocol_holtek_ht6p20b_const.te_long) <
                subghz_protocol_holtek_ht6p20b_const.te_delta * 2) &&
               (DURATION_DIFF(duration, subghz_protocol_holtek_ht6p20b_const.te_short) <
                subghz_protocol_holtek_ht6p20b_const.te_delta)) {
                subghz_protocol_blocks_add_bit(&instance->decoder, 1);
                instance->decoder.parser_step = Holtek_HT6P20BDecoderStepSaveDuration;
            } else if(
                (DURATION_DIFF(
                     instance->decoder.te_last, subghz_protocol_holtek_ht6p20b_const.te_short) <
                 subghz_protocol_holtek_ht6p20b_const.te_delta) &&
                (DURATION_DIFF(duration, subghz_protocol_holtek_ht6p20b_const.te_long) <
                 subghz_protocol_holtek_ht6p20b_const.te_delta * 2)) {
                subghz_protocol_blocks_add_bit(&instance->decoder, 0);
                instance->decoder.parser_step = Holtek_HT6P20BDecoderStepSaveDuration;
            } else {
                instance->decoder.parser_step = Holtek_HT6P20BDecoderStepReset;
            }
        } else {
            instance->decoder.parser_step = Holtek_HT6P20BDecoderStepReset;
        }
        break;
    }
}

/** 
 * Analysis of received data
 * @param instance Pointer to a SubGhzBlockGeneric* instance
 */
static void subghz_protocol_holtek_ht6p20b_check_remote_controller(SubGhzBlockGeneric* instance) {
    instance->btn = (instance->data >> 4) & 0xF;
    instance->cnt = instance->data & 0xF;
}

uint8_t subghz_protocol_decoder_holtek_ht6p20b_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderHoltek_HT6P20B* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_holtek_ht6p20b_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderHoltek_HT6P20B* instance = context;
    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    if((ret == SubGhzProtocolStatusOk) &&
       !flipper_format_write_uint32(flipper_format, "TE", &instance->te, 1)) {
        FURI_LOG_E(TAG, "Unable to add TE");
        ret = SubGhzProtocolStatusErrorParserTe;
    }
    return ret;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_holtek_ht6p20b_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderHoltek_HT6P20B* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;
    do {
        ret = subghz_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            subghz_protocol_holtek_ht6p20b_const.min_count_bit_for_found);
        if(ret != SubGhzProtocolStatusOk) {
            break;
        }
        if(!flipper_format_rewind(flipper_format)) {
            FURI_LOG_E(TAG, "Rewind error");
            ret = SubGhzProtocolStatusErrorParserOthers;
            break;
        }
        if(!flipper_format_read_uint32(flipper_format, "TE", (uint32_t*)&instance->te, 1)) {
            FURI_LOG_E(TAG, "Missing TE");
            ret = SubGhzProtocolStatusErrorParserTe;
            break;
        }
    } while(false);
    return ret;
}

void subghz_protocol_decoder_holtek_ht6p20b_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderHoltek_HT6P20B* instance = context;
    subghz_protocol_holtek_ht6p20b_check_remote_controller(&instance->generic);

    // push protocol data to global variable
    subghz_block_generic_global.btn_is_available = false;
    subghz_block_generic_global.current_btn = instance->generic.btn;
    subghz_block_generic_global.btn_length_bit = 4;
    //

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Key:0x%07lX\r\n",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        (uint32_t)(instance->generic.data & 0xFFFFFFF));
    furi_string_cat_printf(
        output,
        "Anti-Code:0x%01lX\r\n" //Anti-Code: not used in any implementation AFAIK and is fixed to 0x5 in most implementations.
        "Te:%luus\r\n",
        instance->generic.cnt,
        instance->te);
}

