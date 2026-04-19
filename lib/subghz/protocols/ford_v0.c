#include "ford_v0.h"

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"
#include "../blocks/custom_btn_i.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "SubGhzProtocolFordV0"

static const SubGhzBlockConst subghz_protocol_ford_v0_const = {
    .te_short = 250,
    .te_long = 500,
    .te_delta = 100,
    .min_count_bit_for_found = 64,
};

typedef struct SubGhzProtocolDecoderFordV0 {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
    
    ManchesterState manchester_state;
    
    uint64_t data_low;
    uint64_t data_high;
    uint8_t bit_count;
    
    uint16_t header_count;
    
    uint64_t key1;
    uint16_t key2;
    uint32_t serial;
    uint8_t button;
    uint32_t count;
    bool crc_valid;
} SubGhzProtocolDecoderFordV0;

typedef struct SubGhzProtocolEncoderFordV0 {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
    uint64_t key1;
    uint16_t key2;
    uint32_t serial;
    uint8_t button;
    uint32_t count;
} SubGhzProtocolEncoderFordV0;

typedef enum {
    FordV0DecoderStepReset = 0,
    FordV0DecoderStepPreamble,
    FordV0DecoderStepPreambleCheck,
    FordV0DecoderStepGap,
    FordV0DecoderStepData,
} FordV0DecoderStep;

static void ford_v0_add_bit(SubGhzProtocolDecoderFordV0* instance, bool bit);
static void decode_ford_v0(uint64_t key1, uint16_t key2, uint32_t* serial, uint8_t* button, uint32_t* count);
static void encode_ford_v0(uint64_t original_key1, uint32_t serial, uint8_t button, uint32_t count, uint8_t* chk_out, uint64_t* key1_out);
static bool ford_v0_process_data(SubGhzProtocolDecoderFordV0* instance);
static uint8_t ford_v0_calculate_chk_from_buf(uint8_t* buf);

const SubGhzProtocolDecoder subghz_protocol_ford_v0_decoder = {
    .alloc = subghz_protocol_decoder_ford_v0_alloc,
    .free = subghz_protocol_decoder_ford_v0_free,
    .feed = subghz_protocol_decoder_ford_v0_feed,
    .reset = subghz_protocol_decoder_ford_v0_reset,
    .get_hash_data = subghz_protocol_decoder_ford_v0_get_hash_data,
    .serialize = subghz_protocol_decoder_ford_v0_serialize,
    .deserialize = subghz_protocol_decoder_ford_v0_deserialize,
    .get_string = subghz_protocol_decoder_ford_v0_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_ford_v0_encoder = {
    .alloc = subghz_protocol_encoder_ford_v0_alloc,
    .free = subghz_protocol_encoder_ford_v0_free,
    .deserialize = subghz_protocol_encoder_ford_v0_deserialize,
    .stop = subghz_protocol_encoder_ford_v0_stop,
    .yield = subghz_protocol_encoder_ford_v0_yield,
};

const SubGhzProtocol subghz_protocol_ford_v0 = {
    .name = SUBGHZ_PROTOCOL_FORD_V0_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send | SubGhzProtocolFlag_Automotive,
    .decoder = &subghz_protocol_ford_v0_decoder,
    .encoder = &subghz_protocol_ford_v0_encoder,
};

static const uint8_t ford_v0_crc_matrix[64] = {
    0xDA, 0xB5, 0x55, 0x6A, 0xAA, 0xAA, 0xAA, 0xD5,
    0xB6, 0x6C, 0xCC, 0xD9, 0x99, 0x99, 0x99, 0xB3,
    0x71, 0xE3, 0xC3, 0xC7, 0x87, 0x87, 0x87, 0x8F,
    0x0F, 0xE0, 0x3F, 0xC0, 0x7F, 0x80, 0x7F, 0x80,
    0x00, 0x1F, 0xFF, 0xC0, 0x00, 0x7F, 0xFF, 0x80,
    0x00, 0x00, 0x00, 0x3F, 0xFF, 0xFF, 0xFF, 0x80,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7F,
    0x23, 0x12, 0x94, 0x84, 0x35, 0xF4, 0x55, 0x84,
};

static uint8_t ford_v0_popcount8(uint8_t x) {
    uint8_t count = 0;
    while(x) {
        count += x & 1;
        x >>= 1;
    }
    return count;
}

static uint8_t ford_v0_calculate_crc(uint8_t* buf) {
    uint8_t crc = 0;
    for(int row = 0; row < 8; row++) {
        uint8_t xor_sum = 0;
        for(int col = 0; col < 8; col++) {
            xor_sum ^= (ford_v0_crc_matrix[row * 8 + col] & buf[col + 1]);
        }
        uint8_t parity = ford_v0_popcount8(xor_sum) & 1;
        if(parity) {
            crc |= (1 << row);
        }
    }
    return crc;
}

static uint8_t ford_v0_calculate_crc_for_tx(uint64_t key1, uint8_t chk) {
    uint8_t buf[16] = {0};
    for(int i = 0; i < 8; ++i) {
        buf[i] = (uint8_t)(key1 >> (56 - i * 8));
    }
    buf[8] = chk;
    uint8_t crc = ford_v0_calculate_crc(buf);
    return crc ^ 0x80;
}

static bool ford_v0_verify_crc(uint64_t key1, uint16_t key2) {
    uint8_t buf[16] = {0};
    for(int i = 0; i < 8; ++i) {
        buf[i] = (uint8_t)(key1 >> (56 - i * 8));
    }
    buf[8] = (uint8_t)(key2 >> 8);
    uint8_t calculated_crc = ford_v0_calculate_crc(buf);
    uint8_t received_crc = (uint8_t)(key2 & 0xFF) ^ 0x80;
    
    return (calculated_crc == received_crc);
}

static uint8_t ford_v0_calculate_chk_from_buf(uint8_t* buf) {
    uint8_t checksum = 0;
    for(int i = 1; i <= 7; i++)
        checksum += buf[i];
    return checksum;
}

static uint8_t ford_v0_get_button_code(uint8_t custom_btn) {
    switch(custom_btn) {
        case 1: return 0x01;
        case 2: return 0x02;
        case 3: return 0x04;
        default: return 0x01;
    }
}

static uint8_t ford_v0_btn_to_custom(uint8_t btn_code) {
    switch(btn_code) {
        case 0x01: return 1;
        case 0x02: return 2;
        case 0x04: return 3;
        default: return 1;
    }
}

static void ford_v0_add_bit(SubGhzProtocolDecoderFordV0* instance, bool bit) {
    uint32_t low = (uint32_t)instance->data_low;
    instance->data_low = (instance->data_low << 1) | (bit ? 1 : 0);
    instance->data_high = (instance->data_high << 1) | ((low >> 31) & 1);
    instance->bit_count++;
}

static void decode_ford_v0(uint64_t key1, uint16_t key2, uint32_t* serial, uint8_t* button, uint32_t* count) {
    uint8_t buf[13] = {0};
    
    for(int i = 0; i < 8; ++i) {
        buf[i] = (uint8_t)(key1 >> (56 - i * 8));
    }
    
    buf[8] = (uint8_t)(key2 >> 8);
    buf[9] = (uint8_t)(key2 & 0xFF);
    
    uint8_t tmp = buf[8];
    uint8_t parity = 0;
    uint8_t parity_any = (tmp != 0);
    while(tmp) {
        parity ^= (tmp & 1);
        tmp >>= 1;
    }
    buf[11] = parity_any ? parity : 0;
    
    uint8_t xor_byte;
    uint8_t limit;
    if(buf[11]) {
        xor_byte = buf[7];
        limit = 7;
    } else {
        xor_byte = buf[6];
        limit = 6;
    }
    
    for(int idx = 1; idx < limit; ++idx) {
        buf[idx] ^= xor_byte;
    }
    
    if(buf[11] == 0) {
        buf[7] ^= xor_byte;
    }
    
    uint8_t orig_b7 = buf[7];
    
    buf[7] = (orig_b7 & 0xAA) | (buf[6] & 0x55);
    uint8_t mixed = (buf[6] & 0xAA) | (orig_b7 & 0x55);
    buf[12] = mixed;
    buf[6] = mixed;
    
    uint32_t serial_le = ((uint32_t)buf[1]) |
                         ((uint32_t)buf[2] << 8) |
                         ((uint32_t)buf[3] << 16) |
                         ((uint32_t)buf[4] << 24);
    
    *serial = ((serial_le & 0xFF) << 24) |
              (((serial_le >> 8) & 0xFF) << 16) |
              (((serial_le >> 16) & 0xFF) << 8) |
              ((serial_le >> 24) & 0xFF);

    *button = (buf[5] >> 4) & 0x0F;

    *count = ((buf[5] & 0x0F) << 16) |
             (buf[6] << 8) |
             buf[7];
}

static bool ford_v0_process_data(SubGhzProtocolDecoderFordV0* instance) {
    if(instance->bit_count == 64) {
        uint64_t combined = ((uint64_t)instance->data_high << 32) | instance->data_low;
        instance->key1 = ~combined;
        instance->data_low = 0;
        instance->data_high = 0;
        
        instance->decoder.decode_data = instance->key1;
        instance->decoder.decode_count_bit = 64;
        
        return false;
    }
    
    if(instance->bit_count == 80) {
        uint16_t key2_raw = (uint16_t)(instance->data_low & 0xFFFF);
        uint16_t key2 = ~key2_raw;
        
        decode_ford_v0(instance->key1, instance->key2, &instance->serial, &instance->button, &instance->count);
        instance->key2 = key2;
        
        instance->crc_valid = ford_v0_verify_crc(instance->key1, instance->key2);
        
        return true;
    }
    
    return false;
}

static void encode_ford_v0(uint64_t original_key1, uint32_t serial, uint8_t button, uint32_t count, uint8_t* chk_out, uint64_t* key1_out) {
    uint8_t buf[13] = {0};
    
    buf[0] = (uint8_t)(original_key1 >> 56);
    
    buf[1] = (serial >> 24) & 0xFF;
    buf[2] = (serial >> 16) & 0xFF;
    buf[3] = (serial >> 8) & 0xFF;
    buf[4] = serial & 0xFF;
    buf[5] = (button << 4) | ((count >> 16) & 0x0F);
    buf[6] = (count >> 8) & 0xFF;
    buf[7] = count & 0xFF;
    
    uint8_t checksum = ford_v0_calculate_chk_from_buf(buf);
    
    uint8_t orig_b7 = buf[7];
    uint8_t orig_b6 = buf[6];
    buf[7] = (orig_b7 & 0xAA) | (orig_b6 & 0x55);
    buf[6] = (orig_b6 & 0xAA) | (orig_b7 & 0x55);
    
    if(chk_out) *chk_out = checksum;
    buf[8] = checksum;
    uint8_t tmp = buf[8];
    uint8_t parity = 0;
    uint8_t parity_any = (tmp != 0);
    while(tmp) {
        parity ^= (tmp & 1);
        tmp >>= 1;
    }
    uint8_t buf_11 = parity_any ? parity : 0;
    
    uint8_t xor_byte;
    uint8_t limit;
    if(buf_11) {
        xor_byte = buf[7];
        limit = 7;
    } else {
        xor_byte = buf[6];
        limit = 6;
    }
    
    if(buf_11 == 0) {
        buf[7] ^= xor_byte;
    }
    
    for(int idx = 1; idx < limit; idx++) {
        buf[idx] ^= xor_byte;
    }
    
    *key1_out = 0;
    for(int i = 0; i < 8; i++) {
        *key1_out = (*key1_out << 8) | buf[i];
    }
}

void* subghz_protocol_encoder_ford_v0_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderFordV0* instance = malloc(sizeof(SubGhzProtocolEncoderFordV0));
    
    instance->base.protocol = &subghz_protocol_ford_v0;
    instance->generic.protocol_name = instance->base.protocol->name;
    
    instance->encoder.repeat = 3;
    instance->encoder.size_upload = 1024;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    instance->encoder.front = 0;
    
    return instance;
}

void subghz_protocol_encoder_ford_v0_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderFordV0* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void subghz_protocol_encoder_ford_v0_stop(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderFordV0* instance = context;
    instance->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_ford_v0_yield(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderFordV0* instance = context;
    
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

static void subghz_protocol_encoder_ford_v0_get_upload(SubGhzProtocolEncoderFordV0* instance) {
    furi_assert(instance);
    size_t index = 0;
    
    uint32_t te_short = subghz_protocol_ford_v0_const.te_short;
    uint32_t te_long = subghz_protocol_ford_v0_const.te_long;
    uint32_t gap_duration = 3500;
    
    for(int i = 0; i < 20; i++) {
        instance->encoder.upload[index++] = level_duration_make(true, te_short);
        instance->encoder.upload[index++] = level_duration_make(false, te_long);
        instance->encoder.upload[index++] = level_duration_make(true, te_long);
    }
    
    instance->encoder.upload[index++] = level_duration_make(true, te_short);
    
    instance->encoder.upload[index++] = level_duration_make(false, gap_duration);
    
    uint64_t key1_inv = ~instance->key1;
    uint16_t key2_inv = ~instance->key2;
    
    for(int i = 63; i >= 0; i--) {
        bool bit = (key1_inv >> i) & 1;
        if(bit) {
            instance->encoder.upload[index++] = level_duration_make(true, te_short);
            instance->encoder.upload[index++] = level_duration_make(false, te_short);
        } else {
            instance->encoder.upload[index++] = level_duration_make(false, te_short);
            instance->encoder.upload[index++] = level_duration_make(true, te_short);
        }
    }
    
    for(int i = 15; i >= 0; i--) {
        bool bit = (key2_inv >> i) & 1;
        if(bit) {
            instance->encoder.upload[index++] = level_duration_make(true, te_short);
            instance->encoder.upload[index++] = level_duration_make(false, te_short);
        } else {
            instance->encoder.upload[index++] = level_duration_make(false, te_short);
            instance->encoder.upload[index++] = level_duration_make(true, te_short);
        }
    }
    
    instance->encoder.size_upload = index;
    instance->encoder.front = 0;
}

SubGhzProtocolStatus subghz_protocol_encoder_ford_v0_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolEncoderFordV0* instance = context;
    
    SubGhzProtocolStatus ret = subghz_block_generic_deserialize(&instance->generic, flipper_format);
    if(ret != SubGhzProtocolStatusOk) return ret;

    uint32_t chk_old = 0, crc_old = 0;
    flipper_format_read_uint32(flipper_format, "CheckSum", &chk_old, 1);
    flipper_format_read_uint32(flipper_format, "CRC", &crc_old, 1);
    uint16_t key2_old = ((chk_old & 0xFF) << 8) | (crc_old & 0xFF);

    decode_ford_v0(instance->generic.data, key2_old, &instance->serial, &instance->button, &instance->count);

    uint8_t custom_btn = ford_v0_btn_to_custom(instance->button);
    if(subghz_custom_btn_get_original() == 0) {
        subghz_custom_btn_set_original(custom_btn);
    }
    subghz_custom_btn_set_max(3);
    
    instance->count = (instance->count + furi_hal_subghz_get_rolling_counter_mult()) & 0xFFFFF;
    
    uint8_t btn = subghz_custom_btn_get() == SUBGHZ_CUSTOM_BTN_OK ?
                      subghz_custom_btn_get_original() :
                      subghz_custom_btn_get();
    instance->button = ford_v0_get_button_code(btn);

    uint8_t new_chk = 0;
    encode_ford_v0(instance->generic.data, instance->serial, instance->button, instance->count, &new_chk, &instance->key1);
    
    uint8_t new_crc = ford_v0_calculate_crc_for_tx(instance->key1, new_chk);
    instance->key2 = ((uint16_t)new_chk << 8) | new_crc;

    subghz_protocol_encoder_ford_v0_get_upload(instance);
    
    flipper_format_rewind(flipper_format);
    uint8_t key_data[8];
    for(int i = 0; i < 8; i++) key_data[7-i] = (instance->key1 >> (i * 8)) & 0xFF;
    flipper_format_update_hex(flipper_format, "Key", key_data, 8);
    flipper_format_update_uint32(flipper_format, "CheckSum", (uint32_t[]){new_chk}, 1);
    flipper_format_update_uint32(flipper_format, "CRC", (uint32_t[]){new_crc}, 1);

    instance->encoder.is_running = true;
    return SubGhzProtocolStatusOk;
}

void* subghz_protocol_decoder_ford_v0_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderFordV0* instance = malloc(sizeof(SubGhzProtocolDecoderFordV0));
    instance->base.protocol = &subghz_protocol_ford_v0;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->crc_valid = false;
    return instance;
}

void subghz_protocol_decoder_ford_v0_free(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderFordV0* instance = context;
    free(instance);
}

void subghz_protocol_decoder_ford_v0_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderFordV0* instance = context;
    instance->decoder.parser_step = FordV0DecoderStepReset;
    instance->decoder.te_last = 0;
    instance->manchester_state = ManchesterStateMid1;
    instance->data_low = 0;
    instance->data_high = 0;
    instance->bit_count = 0;
    instance->header_count = 0;
    instance->key1 = 0;
    instance->key2 = 0;
    instance->serial = 0;
    instance->button = 0;
    instance->count = 0;
    instance->crc_valid = false;
}

void subghz_protocol_decoder_ford_v0_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderFordV0* instance = context;
    
    uint32_t te_short = subghz_protocol_ford_v0_const.te_short;
    uint32_t te_long = subghz_protocol_ford_v0_const.te_long;
    uint32_t te_delta = subghz_protocol_ford_v0_const.te_delta;
    uint32_t gap_threshold = 3500;
    
    switch(instance->decoder.parser_step) {
    case FordV0DecoderStepReset:
        if(level && (DURATION_DIFF(duration, te_short) < te_delta)) {
            instance->data_low = 0;
            instance->data_high = 0;
            instance->decoder.parser_step = FordV0DecoderStepPreamble;
            instance->decoder.te_last = duration;
            instance->header_count = 0;
            instance->bit_count = 0;
            manchester_advance(instance->manchester_state, ManchesterEventReset, &instance->manchester_state, NULL);
        }
        break;
        
    case FordV0DecoderStepPreamble:
        if(!level) {
            if(DURATION_DIFF(duration, te_long) < te_delta) {
                instance->decoder.te_last = duration;
                instance->decoder.parser_step = FordV0DecoderStepPreambleCheck;
            } else {
                instance->decoder.parser_step = FordV0DecoderStepReset;
            }
        }
        break;
        
    case FordV0DecoderStepPreambleCheck:
        if(level) {
            if(DURATION_DIFF(duration, te_long) < te_delta) {
                instance->header_count++;
                instance->decoder.te_last = duration;
                instance->decoder.parser_step = FordV0DecoderStepPreamble;
            } else if(DURATION_DIFF(duration, te_short) < te_delta) {
                instance->decoder.parser_step = FordV0DecoderStepGap;
            } else {
                instance->decoder.parser_step = FordV0DecoderStepReset;
            }
        }
        break;
        
    case FordV0DecoderStepGap:
        if(!level && (DURATION_DIFF(duration, gap_threshold) < 250)) {
            instance->data_low = 1;
            instance->data_high = 0;
            instance->bit_count = 1;
            instance->decoder.parser_step = FordV0DecoderStepData;
        } else if(!level && duration > gap_threshold + 250) {
            instance->decoder.parser_step = FordV0DecoderStepReset;
        }
        break;
        
    case FordV0DecoderStepData: {
        ManchesterEvent event;
        
        if(DURATION_DIFF(duration, te_short) < te_delta) {
            event = level ? ManchesterEventShortLow : ManchesterEventShortHigh;
        } else if(DURATION_DIFF(duration, te_long) < te_delta) {
            event = level ? ManchesterEventLongLow : ManchesterEventLongHigh;
        } else {
            instance->decoder.parser_step = FordV0DecoderStepReset;
            break;
        }
        
        bool data_bit;
        if(manchester_advance(instance->manchester_state, event, &instance->manchester_state, &data_bit)) {
            ford_v0_add_bit(instance, data_bit);
            
            if(ford_v0_process_data(instance)) {
                instance->generic.data = instance->key1;
                instance->generic.data_count_bit = 64;
                instance->generic.serial = instance->serial;
                instance->generic.btn = instance->button;
                instance->generic.cnt = instance->count;
                
                uint8_t custom_btn = ford_v0_btn_to_custom(instance->button);
                if(subghz_custom_btn_get_original() == 0) {
                    subghz_custom_btn_set_original(custom_btn);
                }
                subghz_custom_btn_set_max(3);
                
                if(instance->base.callback) {
                    instance->base.callback(&instance->base, instance->base.context);
                }
                
                instance->data_low = 0;
                instance->data_high = 0;
                instance->bit_count = 0;
                instance->decoder.parser_step = FordV0DecoderStepReset;
            }
        }
        
        instance->decoder.te_last = duration;
        break;
    }
    }
}

uint8_t subghz_protocol_decoder_ford_v0_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderFordV0* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_ford_v0_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderFordV0* instance = context;
    
    SubGhzProtocolStatus ret = subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    
    if(ret == SubGhzProtocolStatusOk) {
        uint32_t chk = (instance->key2 >> 8) & 0xFF;
        uint32_t crc = instance->key2 & 0xFF;
        if(!flipper_format_write_uint32(flipper_format, "CheckSum", &chk, 1) ||
           !flipper_format_write_uint32(flipper_format, "CRC", &crc, 1)) {
            ret = SubGhzProtocolStatusErrorParserOthers;
        }
    }
    return ret;
}

SubGhzProtocolStatus subghz_protocol_decoder_ford_v0_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderFordV0* instance = context;
    
    SubGhzProtocolStatus ret = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, subghz_protocol_ford_v0_const.min_count_bit_for_found);
    
    if(ret == SubGhzProtocolStatusOk) {
        uint32_t chk_temp = 0, crc_temp = 0;
        
        flipper_format_read_uint32(flipper_format, "CheckSum", &chk_temp, 1);
        flipper_format_read_uint32(flipper_format, "CRC", &crc_temp, 1);
        
        instance->key1 = instance->generic.data;
        instance->key2 = ((chk_temp & 0xFF) << 8) | (crc_temp & 0xFF);
        
        decode_ford_v0(instance->key1, instance->key2, &instance->serial, &instance->button, &instance->count);
        
        instance->generic.serial = instance->serial;
        instance->generic.btn = instance->button;
        instance->generic.cnt = instance->count;
        instance->crc_valid = ford_v0_verify_crc(instance->key1, instance->key2);
        
        uint8_t custom_btn = ford_v0_btn_to_custom(instance->button);
        if(subghz_custom_btn_get_original() == 0) {
            subghz_custom_btn_set_original(custom_btn);
        }
        subghz_custom_btn_set_max(3);
    }
    return ret;
}

static const char* ford_v0_get_button_name(uint8_t btn) {
    switch(btn) {
        case 0x01: return "Lock";
        case 0x02: return "Unlock";
        case 0x04: return "Trunk";
        default: return "??";
    }
}

void subghz_protocol_decoder_ford_v0_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderFordV0* instance = context;
    
    uint32_t code_found_hi = (uint32_t)(instance->key1 >> 32);
    uint32_t code_found_lo = (uint32_t)(instance->key1 & 0xFFFFFFFF);
    
    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Key1:%08lX%08lX\r\n"
        "Key2:%04X Sn:%08lX\r\n"
        "Btn:%02X:[%s] Cnt:%06lX\r\n"
        "CheckSum:%02X CRC:%02X %s",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        code_found_hi,
        code_found_lo,
        instance->key2,
        instance->serial,
        instance->button,
        ford_v0_get_button_name(instance->button),
        instance->count,
        (instance->key2 >> 8) & 0xFF,
        instance->key2 & 0xFF,
        instance->crc_valid ? "(OK)" : "(FAIL)"
    );
}
