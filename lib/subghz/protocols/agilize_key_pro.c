#include "agilize_key_pro.h"

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

#define TAG "SubGhzProtocolAgilizeKeyPro"

static const SubGhzBlockConst subghz_protocol_agilize_key_pro_const = {
    .te_short = 200,
    .te_delta = 150,
    .min_count_bit_for_found = 44,
};

struct SubGhzProtocolDecoderAgilize_Key_Pro {
    SubGhzProtocolDecoderBase base;

    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    uint32_t te;
    uint32_t number;
    uint32_t command;
    uint32_t last_data;
};

struct SubGhzProtocolEncoderAgilize_Key_Pro {
    SubGhzProtocolEncoderBase base;

    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;

    uint32_t te;
};

typedef enum {
    Agilize_Key_ProDecoderStepReset = 0,
    Agilize_Key_ProDecoderStepFoundStartBit,
    Agilize_Key_ProDecoderStepSaveDuration,
    Agilize_Key_ProDecoderStepCheckDuration,
} Agilize_Key_ProDecoderStep;

const SubGhzProtocolDecoder subghz_protocol_agilize_key_pro_decoder = {
    .alloc = subghz_protocol_decoder_agilize_key_pro_alloc,
    .free = subghz_protocol_decoder_agilize_key_pro_free,

    .feed = subghz_protocol_decoder_agilize_key_pro_feed,
    .reset = subghz_protocol_decoder_agilize_key_pro_reset,

    .get_hash_data = subghz_protocol_decoder_agilize_key_pro_get_hash_data,
    .serialize = subghz_protocol_decoder_agilize_key_pro_serialize,
    .deserialize = subghz_protocol_decoder_agilize_key_pro_deserialize,
    .get_string = subghz_protocol_decoder_agilize_key_pro_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_agilize_key_pro_encoder = {
    .alloc = subghz_protocol_encoder_agilize_key_pro_alloc,
    .free = subghz_protocol_encoder_agilize_key_pro_free,

    .deserialize = subghz_protocol_encoder_agilize_key_pro_deserialize,
    .stop = subghz_protocol_encoder_agilize_key_pro_stop,
    .yield = subghz_protocol_encoder_agilize_key_pro_yield,
};

const SubGhzProtocol subghz_protocol_agilize_key_pro = {
    .name = SUBGHZ_PROTOCOL_AGILIZE_KEY_PRO_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_agilize_key_pro_decoder,
    .encoder = &subghz_protocol_agilize_key_pro_encoder,
};

void* subghz_protocol_encoder_agilize_key_pro_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderAgilize_Key_Pro* instance =
        malloc(sizeof(SubGhzProtocolEncoderAgilize_Key_Pro));

    instance->base.protocol = &subghz_protocol_agilize_key_pro;
    instance->generic.protocol_name = instance->base.protocol->name;

    instance->encoder.repeat = 3;
    instance->encoder.size_upload = 91;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void subghz_protocol_encoder_agilize_key_pro_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderAgilize_Key_Pro* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

/**
 * Generating an upload from data.
 * @param instance Pointer to a SubGhzProtocolEncoderAgilize_Key_Pro instance
 * @return true On success
 */
static bool
    subghz_protocol_encoder_agilize_key_pro_get_upload(SubGhzProtocolEncoderAgilize_Key_Pro* instance) {
    furi_assert(instance);

    size_t index = 0;
    size_t size_upload = (instance->generic.data_count_bit * 2) + 3;
    if(size_upload > instance->encoder.size_upload) {
        FURI_LOG_E(TAG, "Size upload exceeds allocated encoder buffer.");
        return false;
    } else {
        instance->encoder.size_upload = size_upload;
    }

    //Send header
    instance->encoder.upload[index++] = level_duration_make(true, (uint32_t)instance->te * 26);
    instance->encoder.upload[index++] = level_duration_make(false, (uint32_t)instance->te * 26);
    //Send start bit
    instance->encoder.upload[index++] = level_duration_make(true, (uint32_t)instance->te * 2);
    //Send key data
    for(uint8_t i = instance->generic.data_count_bit; i > 0; i--) {
        if(bit_read(instance->generic.data, i - 1)) {
            //send bit 1
            instance->encoder.upload[index++] =
                level_duration_make(false, (uint32_t)instance->te * 5);
            instance->encoder.upload[index++] = level_duration_make(true, (uint32_t)instance->te * 2);
        } else {
            //send bit 0
            instance->encoder.upload[index++] = level_duration_make(false, (uint32_t)instance->te * 3);
            instance->encoder.upload[index++] =
                level_duration_make(true, (uint32_t)instance->te * 4);
        }
    }
    return true;
}

SubGhzProtocolStatus
    subghz_protocol_encoder_agilize_key_pro_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolEncoderAgilize_Key_Pro* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;
    do {
        ret = subghz_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            subghz_protocol_agilize_key_pro_const.min_count_bit_for_found);
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

        if(!subghz_protocol_encoder_agilize_key_pro_get_upload(instance)) {
            ret = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }
        instance->encoder.is_running = true;
    } while(false);

    return ret;
}

void subghz_protocol_encoder_agilize_key_pro_stop(void* context) {
    SubGhzProtocolEncoderAgilize_Key_Pro* instance = context;
    instance->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_agilize_key_pro_yield(void* context) {
    SubGhzProtocolEncoderAgilize_Key_Pro* instance = context;

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

void* subghz_protocol_decoder_agilize_key_pro_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderAgilize_Key_Pro* instance =
        malloc(sizeof(SubGhzProtocolDecoderAgilize_Key_Pro));
    instance->base.protocol = &subghz_protocol_agilize_key_pro;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_agilize_key_pro_free(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderAgilize_Key_Pro* instance = context;
    free(instance);
}

void subghz_protocol_decoder_agilize_key_pro_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderAgilize_Key_Pro* instance = context;
    instance->decoder.parser_step = Agilize_Key_ProDecoderStepReset;
}

void subghz_protocol_decoder_agilize_key_pro_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderAgilize_Key_Pro* instance = context;

    switch(instance->decoder.parser_step) {
    case Agilize_Key_ProDecoderStepReset:
        if((!level) && (DURATION_DIFF(duration, subghz_protocol_agilize_key_pro_const.te_short * 26) <
                        subghz_protocol_agilize_key_pro_const.te_delta * 26)) {
            //Found Preambula
            instance->decoder.parser_step = Agilize_Key_ProDecoderStepFoundStartBit;
        }
        break;
    case Agilize_Key_ProDecoderStepFoundStartBit:
        if((level) && (DURATION_DIFF(duration, subghz_protocol_agilize_key_pro_const.te_short * 2) <
                       subghz_protocol_agilize_key_pro_const.te_delta * 2)) {
            //Found StartBit
            instance->decoder.parser_step = Agilize_Key_ProDecoderStepSaveDuration;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->te = duration / 2;
        } else {
            instance->decoder.parser_step = Agilize_Key_ProDecoderStepReset;
        }
        break;
    case Agilize_Key_ProDecoderStepSaveDuration:
        //save duration
        if(!level) {
            if(duration >= ((uint32_t)subghz_protocol_agilize_key_pro_const.te_short * 30 +
                            subghz_protocol_agilize_key_pro_const.te_delta)) {
                if(instance->decoder.decode_count_bit ==
                   subghz_protocol_agilize_key_pro_const.min_count_bit_for_found) {
                    
                    //instance->te /= (instance->decoder.decode_count_bit * 3 + 1);

                    instance->generic.data = instance->decoder.decode_data;
                    instance->generic.data_count_bit = instance->decoder.decode_count_bit;

                    if(instance->base.callback)
                        instance->base.callback(&instance->base, instance->base.context);
                    
                }

                instance->decoder.decode_data = 0;
                instance->decoder.decode_count_bit = 0;
                instance->te = 0;
                instance->decoder.parser_step = Agilize_Key_ProDecoderStepReset;
                break;
            } else {
                instance->decoder.te_last = duration;
                //instance->te += duration;
                instance->decoder.parser_step = Agilize_Key_ProDecoderStepCheckDuration;
            }
        } else {
            instance->decoder.parser_step = Agilize_Key_ProDecoderStepReset;
        }
        break;
    case Agilize_Key_ProDecoderStepCheckDuration:
        if(level) {
            //instance->te += duration;
            if((DURATION_DIFF(
                    instance->decoder.te_last, subghz_protocol_agilize_key_pro_const.te_short * 5) <
                subghz_protocol_agilize_key_pro_const.te_delta * 5) &&
               (DURATION_DIFF(duration, subghz_protocol_agilize_key_pro_const.te_short * 2) <
                subghz_protocol_agilize_key_pro_const.te_delta * 2)) {
                subghz_protocol_blocks_add_bit(&instance->decoder, 1);
                instance->decoder.parser_step = Agilize_Key_ProDecoderStepSaveDuration;
            } else if(
                (DURATION_DIFF(
                     instance->decoder.te_last, subghz_protocol_agilize_key_pro_const.te_short * 3) <
                 subghz_protocol_agilize_key_pro_const.te_delta * 3) &&
                (DURATION_DIFF(duration, subghz_protocol_agilize_key_pro_const.te_short * 4) <
                 subghz_protocol_agilize_key_pro_const.te_delta * 4)) {
                subghz_protocol_blocks_add_bit(&instance->decoder, 0);
                instance->decoder.parser_step = Agilize_Key_ProDecoderStepSaveDuration;
            } else {
                instance->decoder.parser_step = Agilize_Key_ProDecoderStepReset;
            }
        } else {
            instance->decoder.parser_step = Agilize_Key_ProDecoderStepReset;
        }
        break;
    }
}

/** 
 * Analysis of received data
 * @param instance Pointer to a SubGhzBlockGeneric* instance
 */
static void subghz_protocol_agilize_key_pro_check_remote_controller(SubGhzBlockGeneric* instance) {
    instance->serial = (instance->data >> 28) & 0xFFFF;
    instance->seed = (instance->data >> 16) & 0xFFF;
    instance->cnt = ((instance->data >> 12) & 0xF) +
                    (10 * ((instance->data >> 8) & 0xF)) +
                    (100 * ((instance->data >> 4) & 0xF)) +
                    (1000 * (instance->data & 0xF));
}

uint8_t subghz_protocol_decoder_agilize_key_pro_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderAgilize_Key_Pro* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_agilize_key_pro_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderAgilize_Key_Pro* instance = context;
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
    subghz_protocol_decoder_agilize_key_pro_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderAgilize_Key_Pro* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;
    do {
        ret = subghz_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            subghz_protocol_agilize_key_pro_const.min_count_bit_for_found);
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

void subghz_protocol_decoder_agilize_key_pro_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderAgilize_Key_Pro* instance = context;
    subghz_protocol_agilize_key_pro_check_remote_controller(&instance->generic);

    // push protocol data to global variable
    subghz_block_generic_global.btn_is_available = false;
    //

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Key:0x%011llX\r\n",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        instance->generic.data);
    furi_string_cat_printf(
        output,
        "Sn:0x%04lX       Cmd:0x%03lX\r\n"
        "Te:%luus         No:%lu\r\n",
        instance->generic.serial,
        instance->generic.seed,
        instance->te,
        instance->generic.cnt);
}