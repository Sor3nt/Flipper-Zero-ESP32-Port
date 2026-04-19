#include "sheriff_cfm.h"
#include "keeloq_common.h"
#include "../blocks/custom_btn_i.h"
#include "../blocks/generic.h"

#include <string.h>

#define TAG "SubGhzProtocolSheriffCfm"

static const uint8_t cfm_pi_bytes[] = {
    0xA4, 0x58, 0xFE, 0xA3, 0xF4, 0x93, 0x3D, 0x7E,
    0x0D, 0x95, 0x74, 0x8F, 0x72, 0x8E, 0xB6, 0x58,
};

static const uint8_t cfm_zx750_encoded[8] = {
    0x32, 0x4D, 0xCB, 0x84, 0x5F, 0xE9, 0x27, 0xCB,
};

static const uint8_t cfm_zx930_encoded[8] = {
    0x94, 0x3B, 0x63, 0xA5, 0xE8, 0xF3, 0xAB, 0x60,
};

static void cfm_pi_decode(uint8_t* out, const uint8_t* encoded, size_t pi_offset, size_t len) {
    for(size_t i = 0; i < len; i++) {
        out[i] = encoded[i] ^ cfm_pi_bytes[pi_offset + i];
    }
}

static uint8_t cfm_rlf(uint8_t in) {
    return (uint8_t)((in << 1) | (in >> 7));
}

static uint8_t cfm_rrf(uint8_t in) {
    return (uint8_t)((in >> 1) | (in << 7));
}

static uint8_t cfm_swap(uint8_t in) {
    return (uint8_t)((in << 4) | (in >> 4));
}

typedef enum {
    SheriffCfmModelZX750 = 0,
    SheriffCfmModelZX930 = 1,
    SheriffCfmModelCount = 2,
} SheriffCfmModel;

static const char* cfm_model_name(SheriffCfmModel model) {
    switch(model) {
    case SheriffCfmModelZX750: return "ZX-750";
    case SheriffCfmModelZX930: return "ZX-930";
    default: return "?";
    }
}

static void cfm_decrypt_transform(uint8_t* hop, SheriffCfmModel model) {
    uint8_t temp;
    switch(model) {
    case SheriffCfmModelZX750:
        hop[0] = cfm_swap(hop[0]);
        hop[2] = cfm_swap(hop[2]);
        break;

    case SheriffCfmModelZX930:
        hop[0] = ~hop[0];
        temp = hop[1];
        hop[1] = hop[2];
        hop[2] = temp;
        hop[0] = cfm_rrf(hop[0]);
        hop[1] = cfm_swap(hop[1]);
        hop[1] = cfm_rlf(hop[1]);
        hop[1] = cfm_rlf(hop[1]);
        break;

    default:
        break;
    }
}

static void cfm_encrypt_transform(uint8_t* hop, SheriffCfmModel model) {
    uint8_t temp;
    switch(model) {
    case SheriffCfmModelZX750:
        hop[0] = cfm_swap(hop[0]);
        hop[2] = cfm_swap(hop[2]);
        break;

    case SheriffCfmModelZX930:
        hop[1] = cfm_rrf(hop[1]);
        hop[1] = cfm_rrf(hop[1]);
        hop[1] = cfm_swap(hop[1]);
        hop[0] = cfm_rlf(hop[0]);
        hop[0] = ~hop[0];
        temp = hop[1];
        hop[1] = hop[2];
        hop[2] = temp;
        break;

    default:
        break;
    }
}

static const char* cfm_btn_name(uint8_t btn) {
    switch(btn) {
    case 0x10: return "Lock";
    case 0x20: return "Unlock";
    case 0x40: return "Trunk";
    case 0x80: return "Panic";
    default: return "?";
    }
}

static uint8_t cfm_btn_to_custom(uint8_t btn) {
    switch(btn) {
    case 0x10: return SUBGHZ_CUSTOM_BTN_UP;
    case 0x20: return SUBGHZ_CUSTOM_BTN_DOWN;
    case 0x40: return SUBGHZ_CUSTOM_BTN_LEFT;
    case 0x80: return SUBGHZ_CUSTOM_BTN_RIGHT;
    default: return SUBGHZ_CUSTOM_BTN_OK;
    }
}

static uint8_t cfm_custom_to_btn(uint8_t custom, uint8_t original_btn) {
    if(custom == SUBGHZ_CUSTOM_BTN_OK) return original_btn;
    if(custom == SUBGHZ_CUSTOM_BTN_UP) return 0x10;
    if(custom == SUBGHZ_CUSTOM_BTN_DOWN) return 0x20;
    if(custom == SUBGHZ_CUSTOM_BTN_LEFT) return 0x40;
    if(custom == SUBGHZ_CUSTOM_BTN_RIGHT) return 0x80;
    return original_btn;
}

static uint8_t cfm_get_btn_code(uint8_t original_btn) {
    return cfm_custom_to_btn(subghz_custom_btn_get(), original_btn);
}

static const SubGhzBlockConst subghz_protocol_sheriff_cfm_const = {
    .te_short = 400,
    .te_long = 800,
    .te_delta = 140,
    .min_count_bit_for_found = 64,
};

struct SubGhzProtocolDecoderSheriffCfm {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
    uint16_t header_count;
    SheriffCfmModel model;
};

struct SubGhzProtocolEncoderSheriffCfm {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
    SheriffCfmModel model;
};

typedef enum {
    SheriffCfmDecoderStepReset = 0,
    SheriffCfmDecoderStepCheckPreambula,
    SheriffCfmDecoderStepSaveDuration,
    SheriffCfmDecoderStepCheckDuration,
} SheriffCfmDecoderStep;

const SubGhzProtocolDecoder subghz_protocol_sheriff_cfm_decoder = {
    .alloc = subghz_protocol_decoder_sheriff_cfm_alloc,
    .free = subghz_protocol_decoder_sheriff_cfm_free,
    .feed = subghz_protocol_decoder_sheriff_cfm_feed,
    .reset = subghz_protocol_decoder_sheriff_cfm_reset,
    .get_hash_data = subghz_protocol_decoder_sheriff_cfm_get_hash_data,
    .serialize = subghz_protocol_decoder_sheriff_cfm_serialize,
    .deserialize = subghz_protocol_decoder_sheriff_cfm_deserialize,
    .get_string = subghz_protocol_decoder_sheriff_cfm_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_sheriff_cfm_encoder = {
    .alloc = subghz_protocol_encoder_sheriff_cfm_alloc,
    .free = subghz_protocol_encoder_sheriff_cfm_free,
    .deserialize = subghz_protocol_encoder_sheriff_cfm_deserialize,
    .stop = subghz_protocol_encoder_sheriff_cfm_stop,
    .yield = subghz_protocol_encoder_sheriff_cfm_yield,
};

const SubGhzProtocol subghz_protocol_sheriff_cfm = {
    .name = SUBGHZ_PROTOCOL_SHERIFF_CFM_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send | SubGhzProtocolFlag_Automotive,
    .decoder = &subghz_protocol_sheriff_cfm_decoder,
    .encoder = &subghz_protocol_sheriff_cfm_encoder,
};

static uint64_t cfm_get_mfkey(SheriffCfmModel model) {
    uint8_t dkey[8];
    switch(model) {
    case SheriffCfmModelZX750:
        cfm_pi_decode(dkey, cfm_zx750_encoded, 0, 8);
        break;
    case SheriffCfmModelZX930:
        cfm_pi_decode(dkey, cfm_zx930_encoded, 8, 8);
        break;
    default:
        return 0;
    }
    uint64_t key = 0;
    for(int i = 7; i >= 0; i--) {
        key = (key << 8) | dkey[i];
    }
    return key;
}

static bool cfm_try_decrypt(
    uint64_t data,
    SheriffCfmModel* out_model,
    uint8_t* out_btn,
    uint32_t* out_serial,
    uint16_t* out_cnt) {

    uint32_t hop_encrypted = (uint32_t)(data & 0xFFFFFFFF);
    uint32_t fix = (uint32_t)(data >> 32);

    for(SheriffCfmModel model = 0; model < SheriffCfmModelCount; model++) {
        uint8_t hop_bytes[4];
        hop_bytes[0] = (uint8_t)(hop_encrypted & 0xFF);
        hop_bytes[1] = (uint8_t)((hop_encrypted >> 8) & 0xFF);
        hop_bytes[2] = (uint8_t)((hop_encrypted >> 16) & 0xFF);
        hop_bytes[3] = (uint8_t)((hop_encrypted >> 24) & 0xFF);

        cfm_decrypt_transform(hop_bytes, model);

        uint32_t hop_transformed =
            (uint32_t)hop_bytes[0] |
            ((uint32_t)hop_bytes[1] << 8) |
            ((uint32_t)hop_bytes[2] << 16) |
            ((uint32_t)hop_bytes[3] << 24);

        uint64_t mfkey = cfm_get_mfkey(model);
        uint32_t decrypted = subghz_protocol_keeloq_common_decrypt(hop_transformed, mfkey);

        uint16_t dec_serial_lo = (uint16_t)((decrypted >> 16) & 0x3FF);
        uint16_t fix_serial_lo = (uint16_t)(fix & 0x3FF);

        uint8_t btn_byte = (uint8_t)((decrypted >> 24) & 0xFF);
        bool valid_btn = (btn_byte == 0x10 || btn_byte == 0x20 ||
                          btn_byte == 0x40 || btn_byte == 0x80);

        if(valid_btn && (dec_serial_lo == fix_serial_lo)) {
            *out_model = model;
            *out_btn = btn_byte;
            *out_serial = fix;
            *out_cnt = (uint16_t)(decrypted & 0xFFFF);
            return true;
        }
    }
    return false;
}

void* subghz_protocol_encoder_sheriff_cfm_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderSheriffCfm* instance = malloc(sizeof(SubGhzProtocolEncoderSheriffCfm));
    instance->base.protocol = &subghz_protocol_sheriff_cfm;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 3;
    instance->encoder.size_upload = 256;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    instance->encoder.front = 0;
    instance->model = SheriffCfmModelZX750;
    return instance;
}

void subghz_protocol_encoder_sheriff_cfm_free(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderSheriffCfm* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

static bool subghz_protocol_encoder_sheriff_cfm_get_upload(
    SubGhzProtocolEncoderSheriffCfm* instance,
    uint8_t btn) {
    furi_check(instance);

    if((instance->generic.cnt + 1) > 0xFFFF) {
        instance->generic.cnt = 0;
    } else {
        instance->generic.cnt += 1;
    }

    uint32_t fix = (uint32_t)(instance->generic.data >> 32);
    uint16_t serial_lo = (uint16_t)(fix & 0x3FF);
    uint32_t hop_plain =
        ((uint32_t)btn << 24) |
        ((uint32_t)serial_lo << 16) |
        (instance->generic.cnt & 0xFFFF);

    uint64_t mfkey = cfm_get_mfkey(instance->model);
    uint32_t hop_encrypted = subghz_protocol_keeloq_common_encrypt(hop_plain, mfkey);

    uint8_t hop_bytes[4];
    hop_bytes[0] = (uint8_t)(hop_encrypted & 0xFF);
    hop_bytes[1] = (uint8_t)((hop_encrypted >> 8) & 0xFF);
    hop_bytes[2] = (uint8_t)((hop_encrypted >> 16) & 0xFF);
    hop_bytes[3] = (uint8_t)((hop_encrypted >> 24) & 0xFF);
    cfm_encrypt_transform(hop_bytes, instance->model);

    hop_encrypted =
        (uint32_t)hop_bytes[0] |
        ((uint32_t)hop_bytes[1] << 8) |
        ((uint32_t)hop_bytes[2] << 16) |
        ((uint32_t)hop_bytes[3] << 24);

    instance->generic.data = ((uint64_t)fix << 32) | hop_encrypted;
    instance->generic.btn = btn;

    size_t index = 0;

    for(uint8_t i = 11; i > 0; i--) {
        instance->encoder.upload[index++] =
            level_duration_make(true, (uint32_t)subghz_protocol_sheriff_cfm_const.te_short);
        instance->encoder.upload[index++] =
            level_duration_make(false, (uint32_t)subghz_protocol_sheriff_cfm_const.te_short);
    }
    instance->encoder.upload[index++] =
        level_duration_make(true, (uint32_t)subghz_protocol_sheriff_cfm_const.te_short);
    instance->encoder.upload[index++] =
        level_duration_make(false, (uint32_t)subghz_protocol_sheriff_cfm_const.te_short * 10);

    for(uint8_t i = instance->generic.data_count_bit; i > 0; i--) {
        if(bit_read(instance->generic.data, i - 1)) {
            instance->encoder.upload[index++] =
                level_duration_make(true, (uint32_t)subghz_protocol_sheriff_cfm_const.te_short);
            instance->encoder.upload[index++] =
                level_duration_make(false, (uint32_t)subghz_protocol_sheriff_cfm_const.te_long);
        } else {
            instance->encoder.upload[index++] =
                level_duration_make(true, (uint32_t)subghz_protocol_sheriff_cfm_const.te_long);
            instance->encoder.upload[index++] =
                level_duration_make(false, (uint32_t)subghz_protocol_sheriff_cfm_const.te_short);
        }
    }

    instance->encoder.upload[index++] =
        level_duration_make(true, (uint32_t)subghz_protocol_sheriff_cfm_const.te_short);
    instance->encoder.upload[index++] =
        level_duration_make(false, (uint32_t)subghz_protocol_sheriff_cfm_const.te_short * 40);

    instance->encoder.size_upload = index;
    return true;
}

SubGhzProtocolStatus
    subghz_protocol_encoder_sheriff_cfm_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolEncoderSheriffCfm* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    do {
        ret = subghz_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            subghz_protocol_sheriff_cfm_const.min_count_bit_for_found);
        if(ret != SubGhzProtocolStatusOk) break;

        uint32_t model_temp = 0;
        if(flipper_format_read_uint32(flipper_format, "Model", &model_temp, 1)) {
            instance->model = (SheriffCfmModel)model_temp;
        } else {
            SheriffCfmModel detected;
            uint8_t det_btn;
            uint32_t det_serial;
            uint16_t det_cnt;
            if(cfm_try_decrypt(instance->generic.data, &detected, &det_btn, &det_serial, &det_cnt)) {
                instance->model = detected;
                instance->generic.btn = det_btn;
                instance->generic.serial = det_serial;
                instance->generic.cnt = det_cnt;
            }
        }

        if(!flipper_format_read_uint32(
               flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1)) {
            instance->encoder.repeat = 3;
        }

        subghz_custom_btn_set_original(cfm_btn_to_custom(instance->generic.btn));
        subghz_custom_btn_set_max(4);

        uint8_t selected_btn = cfm_get_btn_code(instance->generic.btn);

        if(!subghz_protocol_encoder_sheriff_cfm_get_upload(instance, selected_btn)) {
            break;
        }

        instance->encoder.is_running = true;
        instance->encoder.front = 0;
        ret = SubGhzProtocolStatusOk;
    } while(false);

    return ret;
}

void subghz_protocol_encoder_sheriff_cfm_stop(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderSheriffCfm* instance = context;
    instance->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_sheriff_cfm_yield(void* context) {
    SubGhzProtocolEncoderSheriffCfm* instance = context;

    if(instance->encoder.repeat == 0 || !instance->encoder.is_running) {
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

void* subghz_protocol_decoder_sheriff_cfm_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderSheriffCfm* instance = malloc(sizeof(SubGhzProtocolDecoderSheriffCfm));
    instance->base.protocol = &subghz_protocol_sheriff_cfm;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_sheriff_cfm_free(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderSheriffCfm* instance = context;
    free(instance);
}

void subghz_protocol_decoder_sheriff_cfm_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderSheriffCfm* instance = context;
    instance->decoder.parser_step = SheriffCfmDecoderStepReset;
    instance->header_count = 0;
}

void subghz_protocol_decoder_sheriff_cfm_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderSheriffCfm* instance = context;

    switch(instance->decoder.parser_step) {
    case SheriffCfmDecoderStepReset:
        if((level) && DURATION_DIFF(duration, subghz_protocol_sheriff_cfm_const.te_short) <
                          subghz_protocol_sheriff_cfm_const.te_delta) {
            instance->decoder.parser_step = SheriffCfmDecoderStepCheckPreambula;
            instance->header_count++;
        }
        break;

    case SheriffCfmDecoderStepCheckPreambula:
        if((!level) && (DURATION_DIFF(duration, subghz_protocol_sheriff_cfm_const.te_short) <
                        subghz_protocol_sheriff_cfm_const.te_delta)) {
            instance->decoder.parser_step = SheriffCfmDecoderStepReset;
            break;
        }
        if((instance->header_count > 2) &&
           (DURATION_DIFF(duration, subghz_protocol_sheriff_cfm_const.te_short * 10) <
            subghz_protocol_sheriff_cfm_const.te_delta * 10)) {
            instance->decoder.parser_step = SheriffCfmDecoderStepSaveDuration;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
        } else {
            instance->decoder.parser_step = SheriffCfmDecoderStepReset;
            instance->header_count = 0;
        }
        break;

    case SheriffCfmDecoderStepSaveDuration:
        if(level) {
            instance->decoder.te_last = duration;
            instance->decoder.parser_step = SheriffCfmDecoderStepCheckDuration;
        }
        break;

    case SheriffCfmDecoderStepCheckDuration:
        if(!level) {
            if(duration >= ((uint32_t)subghz_protocol_sheriff_cfm_const.te_short * 2 +
                            subghz_protocol_sheriff_cfm_const.te_delta)) {
                instance->decoder.parser_step = SheriffCfmDecoderStepReset;
                if((instance->decoder.decode_count_bit >=
                    subghz_protocol_sheriff_cfm_const.min_count_bit_for_found) &&
                   (instance->decoder.decode_count_bit <=
                    subghz_protocol_sheriff_cfm_const.min_count_bit_for_found + 2)) {
                    SheriffCfmModel model;
                    uint8_t btn;
                    uint32_t serial;
                    uint16_t cnt;
                    if(cfm_try_decrypt(
                           instance->decoder.decode_data, &model, &btn, &serial, &cnt)) {
                        instance->generic.data = instance->decoder.decode_data;
                        instance->generic.data_count_bit =
                            subghz_protocol_sheriff_cfm_const.min_count_bit_for_found;
                        instance->model = model;
                        instance->generic.btn = btn;
                        instance->generic.serial = serial;
                        instance->generic.cnt = cnt;
                        if(instance->base.callback)
                            instance->base.callback(&instance->base, instance->base.context);
                    }
                }
                instance->decoder.decode_data = 0;
                instance->decoder.decode_count_bit = 0;
                instance->header_count = 0;
                break;
            } else if(
                (DURATION_DIFF(
                     instance->decoder.te_last, subghz_protocol_sheriff_cfm_const.te_short) <
                 subghz_protocol_sheriff_cfm_const.te_delta) &&
                (DURATION_DIFF(duration, subghz_protocol_sheriff_cfm_const.te_long) <
                 subghz_protocol_sheriff_cfm_const.te_delta * 2)) {
                if(instance->decoder.decode_count_bit <
                   subghz_protocol_sheriff_cfm_const.min_count_bit_for_found) {
                    subghz_protocol_blocks_add_bit(&instance->decoder, 1);
                } else {
                    instance->decoder.decode_count_bit++;
                }
                instance->decoder.parser_step = SheriffCfmDecoderStepSaveDuration;
            } else if(
                (DURATION_DIFF(
                     instance->decoder.te_last, subghz_protocol_sheriff_cfm_const.te_long) <
                 subghz_protocol_sheriff_cfm_const.te_delta * 2) &&
                (DURATION_DIFF(duration, subghz_protocol_sheriff_cfm_const.te_short) <
                 subghz_protocol_sheriff_cfm_const.te_delta)) {
                if(instance->decoder.decode_count_bit <
                   subghz_protocol_sheriff_cfm_const.min_count_bit_for_found) {
                    subghz_protocol_blocks_add_bit(&instance->decoder, 0);
                } else {
                    instance->decoder.decode_count_bit++;
                }
                instance->decoder.parser_step = SheriffCfmDecoderStepSaveDuration;
            } else {
                instance->decoder.parser_step = SheriffCfmDecoderStepReset;
                instance->header_count = 0;
            }
        } else {
            instance->decoder.parser_step = SheriffCfmDecoderStepReset;
            instance->header_count = 0;
        }
        break;
    }
}

uint8_t subghz_protocol_decoder_sheriff_cfm_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderSheriffCfm* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_sheriff_cfm_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderSheriffCfm* instance = context;
    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);

    if(ret == SubGhzProtocolStatusOk) {
        uint32_t model = (uint32_t)instance->model;
        flipper_format_insert_or_update_uint32(flipper_format, "Model", &model, 1);
    }
    return ret;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_sheriff_cfm_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderSheriffCfm* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    do {
        ret = subghz_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            subghz_protocol_sheriff_cfm_const.min_count_bit_for_found);
        if(ret != SubGhzProtocolStatusOk) break;

        uint32_t model_temp = 0;
        if(flipper_format_read_uint32(flipper_format, "Model", &model_temp, 1)) {
            instance->model = (SheriffCfmModel)model_temp;
        }

        SheriffCfmModel detected;
        uint8_t btn;
        uint32_t serial;
        uint16_t cnt;
        if(cfm_try_decrypt(instance->generic.data, &detected, &btn, &serial, &cnt)) {
            instance->model = detected;
            instance->generic.btn = btn;
            instance->generic.serial = serial;
            instance->generic.cnt = cnt;
        }
    } while(false);

    return ret;
}

void subghz_protocol_decoder_sheriff_cfm_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderSheriffCfm* instance = context;

    subghz_custom_btn_set_original(cfm_btn_to_custom(instance->generic.btn));
    subghz_custom_btn_set_max(4);

    uint8_t selected_btn = cfm_get_btn_code(instance->generic.btn);

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Key:0x%lX%08lX\r\n"
        "Sn:%08lX Btn:[%s]\r\n"
        "Cnt:%04lX Model:%s\r\n",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        (uint32_t)(instance->generic.data >> 32),
        (uint32_t)instance->generic.data,
        instance->generic.serial,
        cfm_btn_name(selected_btn),
        (uint32_t)instance->generic.cnt,
        cfm_model_name(instance->model));
}
