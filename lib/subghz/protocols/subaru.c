#include "subaru.h"

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"
#include "../blocks/custom_btn_i.h"

#define TAG "SubGhzProtocolSubaru"

static const SubGhzBlockConst subghz_protocol_subaru_const = {
    .te_short = 800,
    .te_long = 1600,
    .te_delta = 250,
    .min_count_bit_for_found = 64,
};

struct SubGhzProtocolDecoderSubaru {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
    
    uint16_t header_count;
    uint16_t bit_count;
    uint8_t data[8];
    
    uint64_t key;
    uint32_t serial;
    uint8_t button;
    uint16_t count;
};

struct SubGhzProtocolEncoderSubaru {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
    
    uint64_t key;
    uint32_t serial;
    uint8_t button;
    uint16_t count;
};

typedef enum {
    SubaruDecoderStepReset = 0,
    SubaruDecoderStepCheckPreamble,
    SubaruDecoderStepFoundGap,
    SubaruDecoderStepFoundSync,
    SubaruDecoderStepSaveDuration,
    SubaruDecoderStepCheckDuration,
} SubaruDecoderStep;

static void subaru_decode_count(const uint8_t* KB, uint16_t* count);
static void subaru_encode_count(uint8_t* KB, uint16_t count);
static void subaru_add_bit(SubGhzProtocolDecoderSubaru* instance, bool bit);
static bool subaru_process_data(SubGhzProtocolDecoderSubaru* instance);
static void subghz_protocol_encoder_subaru_get_upload(SubGhzProtocolEncoderSubaru* instance);

const SubGhzProtocolDecoder subghz_protocol_subaru_decoder = {
    .alloc = subghz_protocol_decoder_subaru_alloc,
    .free = subghz_protocol_decoder_subaru_free,
    .feed = subghz_protocol_decoder_subaru_feed,
    .reset = subghz_protocol_decoder_subaru_reset,
    .get_hash_data = subghz_protocol_decoder_subaru_get_hash_data,
    .serialize = subghz_protocol_decoder_subaru_serialize,
    .deserialize = subghz_protocol_decoder_subaru_deserialize,
    .get_string = subghz_protocol_decoder_subaru_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_subaru_encoder = {
    .alloc = subghz_protocol_encoder_subaru_alloc,
    .free = subghz_protocol_encoder_subaru_free,
    .deserialize = subghz_protocol_encoder_subaru_deserialize,
    .stop = subghz_protocol_encoder_subaru_stop,
    .yield = subghz_protocol_encoder_subaru_yield,
};

const SubGhzProtocol subghz_protocol_subaru = {
    .name = SUBGHZ_PROTOCOL_SUBARU_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | 
            SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Load | 
            SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send | SubGhzProtocolFlag_Automotive,
    .decoder = &subghz_protocol_subaru_decoder,
    .encoder = &subghz_protocol_subaru_encoder,
};

static uint8_t subaru_get_button_code(uint8_t custom_btn) {
    switch(custom_btn) {
        case 1: return 0x01;
        case 2: return 0x02;
        case 3: return 0x03;
        case 4: return 0x04;
        case 5: return 0x08;
        default: return 0x01;
    }
}

static uint8_t subaru_btn_to_custom(uint8_t btn_code) {
    switch(btn_code) {
        case 0x01: return 1;
        case 0x02: return 2;
        case 0x03: return 3;
        case 0x04: return 4;
        case 0x08: return 5;
        default: return 1;
    }
}

static const char* subaru_get_button_name(uint8_t btn) {
    switch(btn) {
        case 0x01: return "Lock";
        case 0x02: return "Unlock";
        case 0x03: return "Trunk";
        case 0x04: return "Panic";
        case 0x08: return "0x08";
        default: return "??";
    }
}

static void subaru_decode_count(const uint8_t* KB, uint16_t* count) {
    uint8_t lo = 0;
    if((KB[4] & 0x40) == 0) lo |= 0x01;
    if((KB[4] & 0x80) == 0) lo |= 0x02;
    if((KB[5] & 0x01) == 0) lo |= 0x04;
    if((KB[5] & 0x02) == 0) lo |= 0x08;
    if((KB[6] & 0x01) == 0) lo |= 0x10;
    if((KB[6] & 0x02) == 0) lo |= 0x20;
    if((KB[5] & 0x40) == 0) lo |= 0x40;
    if((KB[5] & 0x80) == 0) lo |= 0x80;
    
    uint8_t REG_SH1 = (KB[7] << 4) & 0xF0;
    if(KB[5] & 0x04) REG_SH1 |= 0x04;
    if(KB[5] & 0x08) REG_SH1 |= 0x08;
    if(KB[6] & 0x80) REG_SH1 |= 0x02;
    if(KB[6] & 0x40) REG_SH1 |= 0x01;
    
    uint8_t REG_SH2 = ((KB[6] << 2) & 0xF0) | ((KB[7] >> 4) & 0x0F);
    
    uint8_t SER0 = KB[3];
    uint8_t SER1 = KB[1];
    uint8_t SER2 = KB[2];
    
    uint8_t total_rot = 4 + lo;
    for(uint8_t i = 0; i < total_rot; ++i) {
        uint8_t t_bit = (SER0 >> 7) & 1;
        SER0 = ((SER0 << 1) & 0xFE) | ((SER1 >> 7) & 1);
        SER1 = ((SER1 << 1) & 0xFE) | ((SER2 >> 7) & 1);
        SER2 = ((SER2 << 1) & 0xFE) | t_bit;
    }
    
    uint8_t T1 = SER1 ^ REG_SH1;
    uint8_t T2 = SER2 ^ REG_SH2;
    
    uint8_t hi = 0;
    if((T1 & 0x10) == 0) hi |= 0x04;
    if((T1 & 0x20) == 0) hi |= 0x08;
    if((T2 & 0x80) == 0) hi |= 0x02;
    if((T2 & 0x40) == 0) hi |= 0x01;
    if((T1 & 0x01) == 0) hi |= 0x40;
    if((T1 & 0x02) == 0) hi |= 0x80;
    if((T2 & 0x08) == 0) hi |= 0x20;
    if((T2 & 0x04) == 0) hi |= 0x10;
    
    *count = ((hi << 8) | lo) & 0xFFFF;
}

static void subaru_encode_count(uint8_t* KB, uint16_t count) {
    uint8_t lo = count & 0xFF;
    uint8_t hi = (count >> 8) & 0xFF;
    
    KB[4] &= ~0xC0;
    KB[5] &= ~0xC3;
    KB[6] &= ~0x03;
    
    if((lo & 0x01) == 0) KB[4] |= 0x40;
    if((lo & 0x02) == 0) KB[4] |= 0x80;
    if((lo & 0x04) == 0) KB[5] |= 0x01;
    if((lo & 0x08) == 0) KB[5] |= 0x02;
    if((lo & 0x10) == 0) KB[6] |= 0x01;
    if((lo & 0x20) == 0) KB[6] |= 0x02;
    if((lo & 0x40) == 0) KB[5] |= 0x40;
    if((lo & 0x80) == 0) KB[5] |= 0x80;
    
    uint8_t SER0 = KB[3];
    uint8_t SER1 = KB[1];
    uint8_t SER2 = KB[2];
    
    uint8_t total_rot = 4 + lo;
    for(uint8_t i = 0; i < total_rot; ++i) {
        uint8_t t_bit = (SER0 >> 7) & 1;
        SER0 = ((SER0 << 1) & 0xFE) | ((SER1 >> 7) & 1);
        SER1 = ((SER1 << 1) & 0xFE) | ((SER2 >> 7) & 1);
        SER2 = ((SER2 << 1) & 0xFE) | t_bit;
    }
    
    uint8_t T1 = 0xFF;
    uint8_t T2 = 0xFF;
    
    if(hi & 0x04) T1 &= ~0x10;
    if(hi & 0x08) T1 &= ~0x20;
    if(hi & 0x02) T2 &= ~0x80;
    if(hi & 0x01) T2 &= ~0x40;
    if(hi & 0x40) T1 &= ~0x01;
    if(hi & 0x80) T1 &= ~0x02;
    if(hi & 0x20) T2 &= ~0x08;
    if(hi & 0x10) T2 &= ~0x04;
    
    uint8_t new_REG_SH1 = T1 ^ SER1;
    uint8_t new_REG_SH2 = T2 ^ SER2;
    
    KB[5] &= ~0x0C;
    KB[6] &= ~0xC0;
    
    KB[7] = (KB[7] & 0xF0) | ((new_REG_SH1 >> 4) & 0x0F);
    
    if(new_REG_SH1 & 0x04) KB[5] |= 0x04;
    if(new_REG_SH1 & 0x08) KB[5] |= 0x08;
    if(new_REG_SH1 & 0x02) KB[6] |= 0x80;
    if(new_REG_SH1 & 0x01) KB[6] |= 0x40;
    
    KB[6] = (KB[6] & 0xC3) | ((new_REG_SH2 >> 2) & 0x3C);
    
    KB[7] = (KB[7] & 0x0F) | ((new_REG_SH2 << 4) & 0xF0);
}

static void subaru_add_bit(SubGhzProtocolDecoderSubaru* instance, bool bit) {
    if(instance->bit_count < 64) {
        uint8_t byte_idx = instance->bit_count / 8;
        uint8_t bit_idx = 7 - (instance->bit_count % 8);
        if(bit) {
            instance->data[byte_idx] |= (1 << bit_idx);
        } else {
            instance->data[byte_idx] &= ~(1 << bit_idx);
        }
        instance->bit_count++;
    }
}

static bool subaru_process_data(SubGhzProtocolDecoderSubaru* instance) {
    if(instance->bit_count < 64) {
        return false;
    }
    
    uint8_t* b = instance->data;
    
    instance->key = ((uint64_t)b[0] << 56) | ((uint64_t)b[1] << 48) |
                    ((uint64_t)b[2] << 40) | ((uint64_t)b[3] << 32) |
                    ((uint64_t)b[4] << 24) | ((uint64_t)b[5] << 16) |
                    ((uint64_t)b[6] << 8)  | ((uint64_t)b[7]);
    
    instance->serial = ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
    instance->button = b[0] & 0x0F;
    subaru_decode_count(b, &instance->count);
    
    instance->decoder.decode_data = instance->key;
    instance->decoder.decode_count_bit = 64;
    
    return true;
}

void* subghz_protocol_decoder_subaru_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderSubaru* instance = malloc(sizeof(SubGhzProtocolDecoderSubaru));
    instance->base.protocol = &subghz_protocol_subaru;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_subaru_free(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderSubaru* instance = context;
    free(instance);
}

void subghz_protocol_decoder_subaru_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderSubaru* instance = context;
    instance->decoder.parser_step = SubaruDecoderStepReset;
    instance->decoder.te_last = 0;
    instance->header_count = 0;
    instance->bit_count = 0;
    memset(instance->data, 0, sizeof(instance->data));
}

void subghz_protocol_decoder_subaru_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderSubaru* instance = context;
    
    switch(instance->decoder.parser_step) {
    case SubaruDecoderStepReset:
        if(level && DURATION_DIFF(duration, subghz_protocol_subaru_const.te_long) < subghz_protocol_subaru_const.te_delta) {
            instance->decoder.parser_step = SubaruDecoderStepCheckPreamble;
            instance->decoder.te_last = duration;
            instance->header_count = 1;
        }
        break;
        
    case SubaruDecoderStepCheckPreamble:
        if(!level) {
            if(DURATION_DIFF(duration, subghz_protocol_subaru_const.te_long) < subghz_protocol_subaru_const.te_delta) {
                instance->header_count++;
            } else if(duration > 2000 && duration < 3500) {
                if(instance->header_count > 20) {
                    instance->decoder.parser_step = SubaruDecoderStepFoundGap;
                } else {
                    instance->decoder.parser_step = SubaruDecoderStepReset;
                }
            } else {
                instance->decoder.parser_step = SubaruDecoderStepReset;
            }
        } else {
            if(DURATION_DIFF(duration, subghz_protocol_subaru_const.te_long) < subghz_protocol_subaru_const.te_delta) {
                instance->decoder.te_last = duration;
                instance->header_count++;
            } else {
                instance->decoder.parser_step = SubaruDecoderStepReset;
            }
        }
        break;
        
    case SubaruDecoderStepFoundGap:
        if(level && duration > 2000 && duration < 3500) {
            instance->decoder.parser_step = SubaruDecoderStepFoundSync;
        } else {
            instance->decoder.parser_step = SubaruDecoderStepReset;
        }
        break;
        
    case SubaruDecoderStepFoundSync:
        if(!level && DURATION_DIFF(duration, subghz_protocol_subaru_const.te_long) < subghz_protocol_subaru_const.te_delta) {
            instance->decoder.parser_step = SubaruDecoderStepSaveDuration;
            instance->bit_count = 0;
            memset(instance->data, 0, sizeof(instance->data));
        } else {
            instance->decoder.parser_step = SubaruDecoderStepReset;
        }
        break;
        
    case SubaruDecoderStepSaveDuration:
        if(level) {
            if(DURATION_DIFF(duration, subghz_protocol_subaru_const.te_short) < subghz_protocol_subaru_const.te_delta) {
                subaru_add_bit(instance, true);
                instance->decoder.te_last = duration;
                instance->decoder.parser_step = SubaruDecoderStepCheckDuration;
            } else if(DURATION_DIFF(duration, subghz_protocol_subaru_const.te_long) < subghz_protocol_subaru_const.te_delta) {
                subaru_add_bit(instance, false);
                instance->decoder.te_last = duration;
                instance->decoder.parser_step = SubaruDecoderStepCheckDuration;
            } else if(duration > 3000) {
                if(instance->bit_count >= 64) {
                    if(subaru_process_data(instance)) {
                        instance->generic.data = instance->key;
                        instance->generic.data_count_bit = 64;
                        instance->generic.serial = instance->serial;
                        instance->generic.btn = instance->button;
                        instance->generic.cnt = instance->count;
                        
                        uint8_t custom_btn = subaru_btn_to_custom(instance->button);
                        if(subghz_custom_btn_get_original() == 0) {
                            subghz_custom_btn_set_original(custom_btn);
                        }
                        subghz_custom_btn_set_max(5);
                        
                        if(instance->base.callback) {
                            instance->base.callback(&instance->base, instance->base.context);
                        }
                    }
                }
                instance->decoder.parser_step = SubaruDecoderStepReset;
            } else {
                instance->decoder.parser_step = SubaruDecoderStepReset;
            }
        } else {
            instance->decoder.parser_step = SubaruDecoderStepReset;
        }
        break;
        
    case SubaruDecoderStepCheckDuration:
        if(!level) {
            if(DURATION_DIFF(duration, subghz_protocol_subaru_const.te_short) < subghz_protocol_subaru_const.te_delta ||
               DURATION_DIFF(duration, subghz_protocol_subaru_const.te_long) < subghz_protocol_subaru_const.te_delta) {
                instance->decoder.parser_step = SubaruDecoderStepSaveDuration;
            } else if(duration > 3000) {
                if(instance->bit_count >= 64) {
                    if(subaru_process_data(instance)) {
                        instance->generic.data = instance->key;
                        instance->generic.data_count_bit = 64;
                        instance->generic.serial = instance->serial;
                        instance->generic.btn = instance->button;
                        instance->generic.cnt = instance->count;
                        
                        uint8_t custom_btn = subaru_btn_to_custom(instance->button);
                        if(subghz_custom_btn_get_original() == 0) {
                            subghz_custom_btn_set_original(custom_btn);
                        }
                        subghz_custom_btn_set_max(5);
                        
                        if(instance->base.callback) {
                            instance->base.callback(&instance->base, instance->base.context);
                        }
                    }
                }
                instance->decoder.parser_step = SubaruDecoderStepReset;
            } else {
                instance->decoder.parser_step = SubaruDecoderStepReset;
            }
        } else {
            instance->decoder.parser_step = SubaruDecoderStepReset;
        }
        break;
    }
}

uint8_t subghz_protocol_decoder_subaru_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderSubaru* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_subaru_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderSubaru* instance = context;
    
    return subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus subghz_protocol_decoder_subaru_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderSubaru* instance = context;
    
    SubGhzProtocolStatus ret = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, subghz_protocol_subaru_const.min_count_bit_for_found);
    
    if(ret == SubGhzProtocolStatusOk) {
        instance->key = instance->generic.data;
        
        uint8_t b[8];
        for(int i = 0; i < 8; i++) {
            b[i] = (uint8_t)(instance->key >> (56 - i * 8));
        }
        
        instance->serial = ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
        instance->button = b[0] & 0x0F;
        subaru_decode_count(b, &instance->count);
        
        instance->generic.serial = instance->serial;
        instance->generic.btn = instance->button;
        instance->generic.cnt = instance->count;
        
        uint8_t custom_btn = subaru_btn_to_custom(instance->button);
        if(subghz_custom_btn_get_original() == 0) {
            subghz_custom_btn_set_original(custom_btn);
        }
        subghz_custom_btn_set_max(5);
    }
    
    return ret;
}

void subghz_protocol_decoder_subaru_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderSubaru* instance = context;
    
    uint32_t key_hi = (uint32_t)(instance->key >> 32);
    uint32_t key_lo = (uint32_t)(instance->key & 0xFFFFFFFF);
    
    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Key:%08lX%08lX\r\n"
        "Sn:%06lX Cnt:%04X\r\n"
        "Btn:%X [%s]",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        key_hi,
        key_lo,
        instance->serial,
        instance->count,
        instance->button,
        subaru_get_button_name(instance->button));
}

void* subghz_protocol_encoder_subaru_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderSubaru* instance = malloc(sizeof(SubGhzProtocolEncoderSubaru));
    
    instance->base.protocol = &subghz_protocol_subaru;
    instance->generic.protocol_name = instance->base.protocol->name;
    
    instance->encoder.repeat = 10;
    instance->encoder.size_upload = 2048;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    instance->encoder.front = 0;
    
    return instance;
}

void subghz_protocol_encoder_subaru_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderSubaru* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void subghz_protocol_encoder_subaru_stop(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderSubaru* instance = context;
    instance->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_subaru_yield(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderSubaru* instance = context;
    
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

static void subghz_protocol_encoder_subaru_get_upload(SubGhzProtocolEncoderSubaru* instance) {
    furi_assert(instance);
    size_t index = 0;
    
    uint32_t te_short = subghz_protocol_subaru_const.te_short;
    uint32_t te_long = subghz_protocol_subaru_const.te_long;
    uint32_t gap_duration = 2500;
    uint32_t sync_duration = 2500;
    
    for(int i = 0; i < 20; i++) {
        instance->encoder.upload[index++] = level_duration_make(true, te_long);
        instance->encoder.upload[index++] = level_duration_make(false, te_long);
    }
    instance->encoder.upload[index++] = level_duration_make(true, te_long);
    
    instance->encoder.upload[index++] = level_duration_make(false, gap_duration);
    
    instance->encoder.upload[index++] = level_duration_make(true, sync_duration);
    
    instance->encoder.upload[index++] = level_duration_make(false, te_long);
    
    for(int i = 63; i >= 0; i--) {
        bool bit = (instance->key >> i) & 1;
        if(bit) {
            instance->encoder.upload[index++] = level_duration_make(true, te_short);
            instance->encoder.upload[index++] = level_duration_make(false, te_short);
        } else {
            instance->encoder.upload[index++] = level_duration_make(true, te_long);
            instance->encoder.upload[index++] = level_duration_make(false, te_short);
        }
    }
    
    instance->encoder.upload[index++] = level_duration_make(true, te_long);
    instance->encoder.upload[index++] = level_duration_make(false, gap_duration * 2);
    
    instance->encoder.size_upload = index;
    instance->encoder.front = 0;
}

SubGhzProtocolStatus subghz_protocol_encoder_subaru_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolEncoderSubaru* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;
    
    do {
        ret = subghz_block_generic_deserialize(&instance->generic, flipper_format);
        if(ret != SubGhzProtocolStatusOk) {
            break;
        }
        
        uint64_t original_key = instance->generic.data;
        
        uint8_t b[8];
        for(int i = 0; i < 8; i++) {
            b[i] = (uint8_t)(original_key >> (56 - i * 8));
        }
        
        instance->serial = ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
        instance->button = b[0] & 0x0F;
        subaru_decode_count(b, &instance->count);
        
        uint8_t original_custom_btn = subaru_btn_to_custom(instance->button);
        if(subghz_custom_btn_get_original() == 0) {
            subghz_custom_btn_set_original(original_custom_btn);
        }
        subghz_custom_btn_set_max(5);
        
        uint8_t selected_custom_btn;
        if(subghz_custom_btn_get() == SUBGHZ_CUSTOM_BTN_OK) {
            selected_custom_btn = subghz_custom_btn_get_original();
        } else {
            selected_custom_btn = subghz_custom_btn_get();
        }
        
        uint8_t new_button = subaru_get_button_code(selected_custom_btn);
        instance->button = new_button;
        
        uint32_t mult = furi_hal_subghz_get_rolling_counter_mult();
        instance->count = (instance->count + mult) & 0xFFFF;
        
        b[0] = (b[0] & 0xF0) | (instance->button & 0x0F);
        
        subaru_encode_count(b, instance->count);
        
        instance->key = 0;
        for(int i = 0; i < 8; i++) {
            instance->key = (instance->key << 8) | b[i];
        }
        
        instance->generic.data = instance->key;
        instance->generic.serial = instance->serial;
        instance->generic.btn = instance->button;
        instance->generic.cnt = instance->count;
        
        subghz_protocol_encoder_subaru_get_upload(instance);
        
        if(!flipper_format_rewind(flipper_format)) {
            ret = SubGhzProtocolStatusErrorParserOthers;
            break;
        }
        
        uint8_t key_data[sizeof(uint64_t)] = {0};
        for(size_t i = 0; i < sizeof(uint64_t); i++) {
            key_data[sizeof(uint64_t) - i - 1] = (instance->key >> (i * 8)) & 0xFF;
        }
        if(!flipper_format_update_hex(flipper_format, "Key", key_data, sizeof(uint64_t))) {
            ret = SubGhzProtocolStatusErrorParserKey;
            break;
        }
        
        instance->encoder.is_running = true;
        ret = SubGhzProtocolStatusOk;
        
    } while(false);
    
    return ret;
}
