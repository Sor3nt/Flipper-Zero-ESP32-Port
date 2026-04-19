#include "vag.h"
#include "aut64.h"

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"
#include "../blocks/custom_btn_i.h"
#include <lib/toolbox/manchester_decoder.h>
#include <string.h>

#define TAG "VAGProtocol"

static const SubGhzBlockConst subghz_protocol_vag_const = {
    .te_short = 500,
    .te_long = 1000,
    .te_delta = 80,
    .min_count_bit_for_found = 80,
};

#define VAG_T12_TE_SHORT     300u
#define VAG_T12_TE_LONG      600u
#define VAG_T12_TE_DELTA     100u
#define VAG_T12_GAP_DELTA    200u
#define VAG_T12_PREAMBLE_MIN 151u

#define VAG_T34_TE_SHORT     500u
#define VAG_T34_TE_LONG      1000u
#define VAG_T34_TE_DELTA     100u
#define VAG_T34_LONG_DELTA   200u
#define VAG_T34_SYNC         750u
#define VAG_T34_SYNC_DELTA   150u
#define VAG_T34_PREAMBLE_MIN 31u
#define VAG_T34_SYNC_PAIRS   3u

#define VAG_DATA_GAP_MIN     4001u
#define VAG_TOTAL_BITS       80u
#define VAG_KEY1_BITS        64u
#define VAG_PREFIX_BITS      15u
#define VAG_BIT_LIMIT        96u
#define VAG_FRAME_PREFIX_T1  0x2F3Fu
#define VAG_FRAME_PREFIX_T2  0x2F1Cu

#define VAG_KEYS_COUNT 3

static const uint8_t vag_keys_packed[VAG_KEYS_COUNT][AUT64_KEY_STRUCT_PACKED_SIZE] = {
    {0x01, 0x37, 0x6C, 0x86, 0xAD, 0xAB, 0xCC, 0x43, 0x07, 0x4D, 0xE8, 0x59, 0xC1, 0x2F, 0x36, 0xAB},
    {0x02, 0x37, 0x7C, 0x65, 0xCE, 0xDC, 0x42, 0xEA, 0xA4, 0x53, 0xE8, 0x61, 0xD9, 0xB7, 0x20, 0xFC},
    {0x03, 0x8A, 0xA3, 0x7B, 0x1E, 0x56, 0x1F, 0x83, 0x84, 0xB6, 0x19, 0xC5, 0x2E, 0x0A, 0x3F, 0xD7}
};

static struct aut64_key protocol_vag_keys[VAG_KEYS_COUNT];
static int8_t protocol_vag_keys_loaded = -1;

static void protocol_vag_load_keys(void) {
    if(protocol_vag_keys_loaded >= 0) {
        return;
    }

    for(uint8_t i = 0; i < VAG_KEYS_COUNT; i++) {
        aut64_unpack(&protocol_vag_keys[i], vag_keys_packed[i]);
    }

    protocol_vag_keys_loaded = 0;
}

static struct aut64_key* protocol_vag_get_key(uint8_t index) {
    for(uint8_t i = 0; i < VAG_KEYS_COUNT; i++) {
        if(protocol_vag_keys[i].index == index) {
            return &protocol_vag_keys[i];
        }
    }

    return NULL;
}

#define VAG_TEA_DELTA  0x9E3779B9U
#define VAG_TEA_ROUNDS 32

static const uint32_t vag_tea_key_schedule[] = {0x0B46502D, 0x5E253718, 0x2BF93A19, 0x622C1206};

static const char* vag_button_name(uint8_t btn) {
    switch(btn) {
    case 0x10:
        return "Unlock";
    case 0x20:
        return "Lock";
    case 0x40:
        return "Trunk";
    case 0x80:
        return "Panic";
    default:
        return "??";
    }
}

static uint8_t vag_custom_to_btn(uint8_t custom, uint8_t original_btn) {
    switch(custom) {
        case 1: return 0x20;  
        case 2: return 0x10;  
        case 3: return 0x40;  
        case 4: return 0x80;  
        default: return original_btn;  
    }
}

static uint8_t vag_btn_to_custom(uint8_t btn) {
    switch(btn) {
        case 0x10: return 2;  
        case 0x20: return 1;  
        case 0x40: return 3;  
        case 0x80: return 4;  
        default: return 1;
    }
}

typedef struct SubGhzProtocolDecoderVAG {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    uint32_t data_low;
    uint32_t data_high;
    uint8_t bit_count;
    uint32_t key1_low;
    uint32_t key1_high;
    uint32_t key2_low;
    uint32_t key2_high;
    uint16_t data_count_bit;
    uint8_t vag_type;
    uint16_t header_count;
    uint8_t mid_count;
    ManchesterState manchester_state;

    uint32_t serial;
    uint32_t cnt;
    uint8_t btn;
    uint8_t check_byte;
    uint8_t key_idx;
    bool decrypted;
    
    
    uint32_t last_valid_serial;
    uint32_t last_valid_cnt;
} SubGhzProtocolDecoderVAG;

typedef enum {
    VAGDecoderStepReset = 0,
    VAGDecoderStepPreamble1 = 1,
    VAGDecoderStepData1 = 2,
    VAGDecoderStepPreamble2 = 3,
    VAGDecoderStepSync2A = 4,
    VAGDecoderStepSync2B = 5,
    VAGDecoderStepSync2C = 6,
    VAGDecoderStepData2 = 7,
} VAGDecoderStep;

static void vag_tea_decrypt(uint32_t* v0, uint32_t* v1, const uint32_t* key_schedule) {
    uint32_t sum = VAG_TEA_DELTA * VAG_TEA_ROUNDS;
    for(int i = 0; i < VAG_TEA_ROUNDS; i++) {
        *v1 -= (((*v0 << 4) ^ (*v0 >> 5)) + *v0) ^ (sum + key_schedule[(sum >> 11) & 3]);
        sum -= VAG_TEA_DELTA;
        *v0 -= (((*v1 << 4) ^ (*v1 >> 5)) + *v1) ^ (sum + key_schedule[sum & 3]);
    }
}

static void vag_tea_encrypt(uint32_t* v0, uint32_t* v1, const uint32_t* key_schedule) {
    uint32_t sum = 0;
    for(int i = 0; i < VAG_TEA_ROUNDS; i++) {
        *v0 += (((*v1 << 4) ^ (*v1 >> 5)) + *v1) ^ (sum + key_schedule[sum & 3]);
        sum += VAG_TEA_DELTA;
        *v1 += (((*v0 << 4) ^ (*v0 >> 5)) + *v0) ^ (sum + key_schedule[(sum >> 11) & 3]);
    }
}

static bool vag_dispatch_type_1_2(uint8_t dispatch) {
    return (dispatch == 0x2A || dispatch == 0x1C || dispatch == 0x46 || dispatch == 0x88);
}

static bool vag_dispatch_type_3_4(uint8_t dispatch) {
    return (dispatch == 0x2B || dispatch == 0x1D || dispatch == 0x47 || dispatch == 0x89);
}

static bool vag_button_valid(const uint8_t* dec) {
    uint8_t dec_byte = dec[7];
    uint8_t dec_btn = (dec_byte >> 4) & 0xF;

    if(dec_btn == 1 || dec_btn == 2 || dec_btn == 4 || dec_btn == 8) {
        return true;
    }
    if(dec_byte == 0) {
        return true;
    }
    return false;
}

static bool vag_button_matches(const uint8_t* dec, uint8_t dispatch_byte) {
    uint8_t expected_btn = (dispatch_byte >> 4) & 0xF;
    uint8_t dec_btn = (dec[7] >> 4) & 0xF;

    if(dec_btn == expected_btn) {
        return true;
    }
    if(dec[7] == 0 && expected_btn == 2) {
        return true;
    }
    return false;
}

static void vag_fill_from_decrypted(
    SubGhzProtocolDecoderVAG* instance,
    const uint8_t* dec,
    uint8_t dispatch_byte) {
    uint32_t serial_raw = (uint32_t)dec[0] | ((uint32_t)dec[1] << 8) | ((uint32_t)dec[2] << 16) |
                          ((uint32_t)dec[3] << 24);
    instance->serial = (serial_raw << 24) | ((serial_raw & 0xFF00) << 8) |
                       ((serial_raw >> 8) & 0xFF00) | (serial_raw >> 24);

    instance->cnt = (uint32_t)dec[4] | ((uint32_t)dec[5] << 8) | ((uint32_t)dec[6] << 16);

    
    uint8_t btn_nibble = (dec[7] >> 4) & 0xF;
    if(btn_nibble == 1) instance->btn = 0x10;
    else if(btn_nibble == 2) instance->btn = 0x20;
    else if(btn_nibble == 4) instance->btn = 0x40;
    else if(btn_nibble == 8) instance->btn = 0x80;
    else instance->btn = dec[7];  
    
    instance->check_byte = dispatch_byte;
    instance->decrypted = true;
}

static bool vag_aut64_decrypt(uint8_t* block, int key_index) {
    struct aut64_key* key = protocol_vag_get_key(key_index + 1);
    if(!key) {
        return false;
    }
    aut64_decrypt(*key, block);
    return true;
}

static void vag_parse_data(SubGhzProtocolDecoderVAG* instance) {
    furi_assert(instance);

    instance->decrypted = false;
    instance->serial = 0;
    instance->cnt = 0;
    instance->btn = 0;

    uint8_t dispatch_byte = (uint8_t)(instance->key2_low & 0xFF);
    uint8_t key2_high = (uint8_t)((instance->key2_low >> 8) & 0xFF);

    uint8_t key1_bytes[8];
    uint32_t key1_low = instance->key1_low;
    uint32_t key1_high = instance->key1_high;

    key1_bytes[0] = (uint8_t)(key1_high >> 24);
    key1_bytes[1] = (uint8_t)(key1_high >> 16);
    key1_bytes[2] = (uint8_t)(key1_high >> 8);
    key1_bytes[3] = (uint8_t)(key1_high);
    key1_bytes[4] = (uint8_t)(key1_low >> 24);
    key1_bytes[5] = (uint8_t)(key1_low >> 16);
    key1_bytes[6] = (uint8_t)(key1_low >> 8);
    key1_bytes[7] = (uint8_t)(key1_low);

    uint8_t block[8];
    block[0] = key1_bytes[1];
    block[1] = key1_bytes[2];
    block[2] = key1_bytes[3];
    block[3] = key1_bytes[4];
    block[4] = key1_bytes[5];
    block[5] = key1_bytes[6];
    block[6] = key1_bytes[7];
    block[7] = key2_high;

    switch(instance->vag_type) {
    case 1:
        if(!vag_dispatch_type_1_2(dispatch_byte)) {
            break;
        }
        {
            uint8_t block_copy[8];

            for(int key_idx = 0; key_idx < 3; key_idx++) {
                memcpy(block_copy, block, 8);
                if(!vag_aut64_decrypt(block_copy, key_idx)) {
                    continue;
                }

                if(vag_button_valid(block_copy)) {
                    instance->serial = ((uint32_t)block_copy[0] << 24) |
                                       ((uint32_t)block_copy[1] << 16) |
                                       ((uint32_t)block_copy[2] << 8) | (uint32_t)block_copy[3];
                    instance->cnt = (uint32_t)block_copy[4] | ((uint32_t)block_copy[5] << 8) |
                                    ((uint32_t)block_copy[6] << 16);
                    
                    
                    uint8_t btn_nibble = (block_copy[7] >> 4) & 0xF;
                    if(btn_nibble == 1) instance->btn = 0x10;
                    else if(btn_nibble == 2) instance->btn = 0x20;
                    else if(btn_nibble == 4) instance->btn = 0x40;
                    else if(btn_nibble == 8) instance->btn = 0x80;
                    else instance->btn = block_copy[7];
                    
                    instance->check_byte = dispatch_byte;
                    instance->key_idx = key_idx;
                    instance->decrypted = true;
                    return;
                }
            }
        }
        break;

    case 2:
        if(!vag_dispatch_type_1_2(dispatch_byte)) {
            break;
        }
        {
            uint32_t v0_orig = ((uint32_t)block[0] << 24) | ((uint32_t)block[1] << 16) |
                               ((uint32_t)block[2] << 8) | (uint32_t)block[3];
            uint32_t v1_orig = ((uint32_t)block[4] << 24) | ((uint32_t)block[5] << 16) |
                               ((uint32_t)block[6] << 8) | (uint32_t)block[7];

            {
                uint32_t v0 = v0_orig;
                uint32_t v1 = v1_orig;

                vag_tea_decrypt(&v0, &v1, vag_tea_key_schedule);

                uint8_t tea_dec[8];
                tea_dec[0] = (uint8_t)(v0 >> 24);
                tea_dec[1] = (uint8_t)(v0 >> 16);
                tea_dec[2] = (uint8_t)(v0 >> 8);
                tea_dec[3] = (uint8_t)(v0);
                tea_dec[4] = (uint8_t)(v1 >> 24);
                tea_dec[5] = (uint8_t)(v1 >> 16);
                tea_dec[6] = (uint8_t)(v1 >> 8);
                tea_dec[7] = (uint8_t)(v1);

                if(!vag_button_matches(tea_dec, dispatch_byte)) {
                    break;
                }

                vag_fill_from_decrypted(instance, tea_dec, dispatch_byte);
                instance->key_idx = 0xFF;

                return;
            }
        }
        break;

    case 3: {
        uint8_t block_copy[8];

        memcpy(block_copy, block, 8);
        if(vag_aut64_decrypt(block_copy, 2) && vag_button_valid(block_copy)) {
            instance->vag_type = 4;
            instance->key_idx = 2;
            vag_fill_from_decrypted(instance, block_copy, dispatch_byte);
            return;
        }

        memcpy(block_copy, block, 8);
        if(vag_aut64_decrypt(block_copy, 1) && vag_button_valid(block_copy)) {
            instance->key_idx = 1;
            vag_fill_from_decrypted(instance, block_copy, dispatch_byte);
            return;
        }

        memcpy(block_copy, block, 8);
        if(vag_aut64_decrypt(block_copy, 0) && vag_button_valid(block_copy)) {
            instance->key_idx = 0;
            vag_fill_from_decrypted(instance, block_copy, dispatch_byte);
            return;
        }

    } break;

    case 4:
        if(!vag_dispatch_type_3_4(dispatch_byte)) {
            break;
        }
        {
            uint8_t block_copy[8];
            memcpy(block_copy, block, 8);

            if(!vag_aut64_decrypt(block_copy, 2)) {
                break;
            }
            if(!vag_button_matches(block_copy, dispatch_byte)) {
                break;
            }
            instance->key_idx = 2;
            vag_fill_from_decrypted(instance, block_copy, dispatch_byte);
        }
        return;

    default:
        break;
    }

    instance->decrypted = false;
    instance->serial = 0;
    instance->cnt = 0;
    instance->btn = 0;
    instance->check_byte = 0;
}

const SubGhzProtocolDecoder subghz_protocol_vag_decoder = {
    .alloc = subghz_protocol_decoder_vag_alloc,
    .free = subghz_protocol_decoder_vag_free,
    .feed = subghz_protocol_decoder_vag_feed,
    .reset = subghz_protocol_decoder_vag_reset,
    .get_hash_data = subghz_protocol_decoder_vag_get_hash_data,
    .serialize = subghz_protocol_decoder_vag_serialize,
    .deserialize = subghz_protocol_decoder_vag_deserialize,
    .get_string = subghz_protocol_decoder_vag_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_vag_encoder = {
    .alloc = subghz_protocol_encoder_vag_alloc,
    .free = subghz_protocol_encoder_vag_free,
    .deserialize = subghz_protocol_encoder_vag_deserialize,
    .stop = subghz_protocol_encoder_vag_stop,
    .yield = subghz_protocol_encoder_vag_yield,
};

const SubGhzProtocol subghz_protocol_vag = {
    .name = VAG_PROTOCOL_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send | SubGhzProtocolFlag_Automotive,
    .decoder = &subghz_protocol_vag_decoder,
    .encoder = &subghz_protocol_vag_encoder,
};

void* subghz_protocol_decoder_vag_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderVAG* instance = malloc(sizeof(SubGhzProtocolDecoderVAG));
    instance->base.protocol = &subghz_protocol_vag;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->decrypted = false;
    instance->serial = 0;
    instance->cnt = 0;
    instance->btn = 0;
    instance->check_byte = 0;
    instance->key_idx = 0xFF;
    instance->last_valid_serial = 0;
    instance->last_valid_cnt = 0;

    protocol_vag_load_keys();

    return instance;
}

void subghz_protocol_decoder_vag_free(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderVAG* instance = context;
    free(instance);
}

void subghz_protocol_decoder_vag_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderVAG* instance = context;
    instance->decoder.parser_step = VAGDecoderStepReset;
    instance->decrypted = false;
    instance->serial = 0;
    instance->cnt = 0;
    instance->btn = 0;
    instance->check_byte = 0;
    instance->key_idx = 0xFF;
}

void subghz_protocol_decoder_vag_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderVAG* instance = context;

    switch(instance->decoder.parser_step) {
    case VAGDecoderStepReset:
        if(!level) break;
        if(DURATION_DIFF(duration, VAG_T12_TE_SHORT) < VAG_T12_TE_DELTA) {
            instance->decoder.parser_step = VAGDecoderStepPreamble1;
        } else if(DURATION_DIFF(duration, VAG_T34_TE_SHORT) < VAG_T34_TE_DELTA) {
            instance->decoder.parser_step = VAGDecoderStepPreamble2;
        } else {
            break;
        }
        instance->data_low = 0;
        instance->data_high = 0;
        instance->header_count = 0;
        instance->mid_count = 0;
        instance->bit_count = 0;
        instance->vag_type = 0;
        instance->decoder.te_last = duration;
        manchester_advance(
            instance->manchester_state, ManchesterEventReset, &instance->manchester_state, NULL);
        break;

    case VAGDecoderStepPreamble1:
        if(level) break;
        if(DURATION_DIFF(duration, VAG_T12_TE_SHORT) < VAG_T12_TE_DELTA) {
            if(DURATION_DIFF(instance->decoder.te_last, VAG_T12_TE_SHORT) < VAG_T12_TE_DELTA) {
                instance->decoder.te_last = duration;
                instance->header_count++;
            } else {
                instance->decoder.parser_step = VAGDecoderStepReset;
            }
            break;
        }
        instance->decoder.parser_step = VAGDecoderStepReset;
        if(instance->header_count < VAG_T12_PREAMBLE_MIN) break;
        if(DURATION_DIFF(duration, VAG_T12_TE_LONG) >= VAG_T12_GAP_DELTA) break;
        if(DURATION_DIFF(instance->decoder.te_last, VAG_T12_TE_SHORT) >= VAG_T12_TE_DELTA) break;
        instance->decoder.parser_step = VAGDecoderStepData1;
        break;

    case VAGDecoderStepData1: {
        if(instance->bit_count >= VAG_BIT_LIMIT) {
            instance->decoder.parser_step = VAGDecoderStepReset;
            break;
        }

        bool bit_value = false;
        ManchesterEvent event = ManchesterEventReset;
        bool got_pulse = false;

        if(DURATION_DIFF(duration, VAG_T12_TE_SHORT) < VAG_T12_TE_DELTA) {
            event = level ? ManchesterEventShortLow : ManchesterEventShortHigh;
            got_pulse = true;
        } else if(
            duration > VAG_T12_TE_SHORT + VAG_T12_TE_DELTA &&
            DURATION_DIFF(duration, VAG_T12_TE_LONG) < VAG_T12_GAP_DELTA) {
            event = level ? ManchesterEventLongLow : ManchesterEventLongHigh;
            got_pulse = true;
        }

        if(got_pulse) {
            if(manchester_advance(
                   instance->manchester_state, event, &instance->manchester_state, &bit_value)) {
                uint32_t carry = (instance->data_low >> 31) & 1;
                instance->data_low = (instance->data_low << 1) | (bit_value ? 1 : 0);
                instance->data_high = (instance->data_high << 1) | carry;
                instance->bit_count++;

                if(instance->bit_count == VAG_PREFIX_BITS) {
                    if(instance->data_low == VAG_FRAME_PREFIX_T1 && instance->data_high == 0) {
                        instance->data_low = 0;
                        instance->data_high = 0;
                        instance->bit_count = 0;
                        instance->vag_type = 1;
                    } else if(instance->data_low == VAG_FRAME_PREFIX_T2 && instance->data_high == 0) {
                        instance->data_low = 0;
                        instance->data_high = 0;
                        instance->bit_count = 0;
                        instance->vag_type = 2;
                    }
                } else if(instance->bit_count == VAG_KEY1_BITS) {
                    instance->key1_low = ~instance->data_low;
                    instance->key1_high = ~instance->data_high;
                    instance->data_low = 0;
                    instance->data_high = 0;
                }
            }
            break;
        }

        if(level) break;
        if(duration < VAG_DATA_GAP_MIN) break;
        if(instance->bit_count == VAG_TOTAL_BITS) {
            instance->key2_low = (~instance->data_low) & 0xFFFF;
            instance->key2_high = 0;
            instance->data_count_bit = VAG_TOTAL_BITS;

            vag_parse_data(instance);

            if(instance->base.callback && instance->decrypted) {
                bool is_valid = true;
                if(instance->last_valid_serial != 0) {
                    if(instance->serial == instance->last_valid_serial) {
                        uint32_t cnt_diff = instance->cnt > instance->last_valid_cnt ?
                            instance->cnt - instance->last_valid_cnt :
                            instance->last_valid_cnt - instance->cnt;
                        if(cnt_diff > 100) {
                            is_valid = false;
                        }
                    }
                }
                if(is_valid) {
                    instance->last_valid_serial = instance->serial;
                    instance->last_valid_cnt = instance->cnt;
                    instance->base.callback(&instance->base, instance->base.context);
                }
            }
        }
        instance->data_low = 0;
        instance->data_high = 0;
        instance->bit_count = 0;
        instance->decoder.parser_step = VAGDecoderStepReset;
        break;
    }

    case VAGDecoderStepPreamble2:
        if(!level) {
            if(DURATION_DIFF(duration, VAG_T34_TE_SHORT) < VAG_T34_TE_DELTA &&
               DURATION_DIFF(instance->decoder.te_last, VAG_T34_TE_SHORT) < VAG_T34_TE_DELTA) {
                instance->decoder.te_last = duration;
                instance->header_count++;
            } else {
                instance->decoder.parser_step = VAGDecoderStepReset;
            }
            break;
        }
        if(instance->header_count < VAG_T34_PREAMBLE_MIN) break;
        if(DURATION_DIFF(duration, VAG_T34_TE_LONG) >= VAG_T34_LONG_DELTA) break;
        if(DURATION_DIFF(instance->decoder.te_last, VAG_T34_TE_SHORT) >= VAG_T34_TE_DELTA) break;
        instance->decoder.te_last = duration;
        instance->decoder.parser_step = VAGDecoderStepSync2A;
        break;

    case VAGDecoderStepSync2A:
        if(!level &&
           DURATION_DIFF(duration, VAG_T34_TE_SHORT) < VAG_T34_TE_DELTA &&
           DURATION_DIFF(instance->decoder.te_last, VAG_T34_TE_LONG) < VAG_T34_LONG_DELTA) {
            instance->decoder.te_last = duration;
            instance->decoder.parser_step = VAGDecoderStepSync2B;
        } else {
            instance->decoder.parser_step = VAGDecoderStepReset;
        }
        break;

    case VAGDecoderStepSync2B:
        if(level && DURATION_DIFF(duration, VAG_T34_SYNC) < VAG_T34_SYNC_DELTA) {
            instance->decoder.te_last = duration;
            instance->decoder.parser_step = VAGDecoderStepSync2C;
        } else {
            instance->decoder.parser_step = VAGDecoderStepReset;
        }
        break;

    case VAGDecoderStepSync2C:
        if(!level &&
           DURATION_DIFF(duration, VAG_T34_SYNC) < VAG_T34_SYNC_DELTA &&
           DURATION_DIFF(instance->decoder.te_last, VAG_T34_SYNC) < VAG_T34_SYNC_DELTA) {
            instance->mid_count++;
            instance->decoder.parser_step = VAGDecoderStepSync2B;
            if(instance->mid_count == VAG_T34_SYNC_PAIRS) {
                instance->data_low = 1;
                instance->data_high = 0;
                instance->bit_count = 1;
                manchester_advance(
                    instance->manchester_state,
                    ManchesterEventReset,
                    &instance->manchester_state,
                    NULL);
                instance->decoder.parser_step = VAGDecoderStepData2;
            }
        } else {
            instance->decoder.parser_step = VAGDecoderStepReset;
        }
        break;

    case VAGDecoderStepData2: {
        bool bit_value = false;
        ManchesterEvent event = ManchesterEventReset;
        bool got_pulse = false;

        if(DURATION_DIFF(duration, VAG_T34_TE_SHORT) < VAG_T34_TE_DELTA) {
            event = level ? ManchesterEventShortLow : ManchesterEventShortHigh;
            got_pulse = true;
        } else if(DURATION_DIFF(duration, VAG_T34_TE_LONG) < VAG_T34_LONG_DELTA) {
            event = level ? ManchesterEventLongLow : ManchesterEventLongHigh;
            got_pulse = true;
        }

        if(got_pulse) {
            if(manchester_advance(
                   instance->manchester_state, event, &instance->manchester_state, &bit_value)) {
                uint32_t carry = (instance->data_low >> 31) & 1;
                instance->data_low = (instance->data_low << 1) | (bit_value ? 1 : 0);
                instance->data_high = (instance->data_high << 1) | carry;
                instance->bit_count++;

                if(instance->bit_count == VAG_KEY1_BITS) {
                    instance->key1_low = instance->data_low;
                    instance->key1_high = instance->data_high;
                    instance->data_low = 0;
                    instance->data_high = 0;
                }
            }
        }

        if(instance->bit_count != VAG_TOTAL_BITS) break;
        instance->key2_low = instance->data_low & 0xFFFF;
        instance->key2_high = 0;
        instance->data_count_bit = VAG_TOTAL_BITS;
        instance->vag_type = 3;
        vag_parse_data(instance);
        if(instance->base.callback && instance->decrypted) {
            bool is_valid = true;
            if(instance->last_valid_serial != 0) {
                if(instance->serial == instance->last_valid_serial) {
                    uint32_t cnt_diff = instance->cnt > instance->last_valid_cnt ?
                        instance->cnt - instance->last_valid_cnt :
                        instance->last_valid_cnt - instance->cnt;
                    if(cnt_diff > 100) {
                        is_valid = false;
                    }
                }
            }
            if(is_valid) {
                instance->last_valid_serial = instance->serial;
                instance->last_valid_cnt = instance->cnt;
                instance->base.callback(&instance->base, instance->base.context);
            }
        }
        instance->data_low = 0;
        instance->data_high = 0;
        instance->bit_count = 0;
        instance->decoder.parser_step = VAGDecoderStepReset;
        break;
    }

    default:
        instance->decoder.parser_step = VAGDecoderStepReset;
        break;
    }
}

uint8_t subghz_protocol_decoder_vag_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderVAG* instance = context;
    uint8_t hash = 0;
    hash ^= (instance->key1_low & 0xFF);
    hash ^= ((instance->key1_low >> 8) & 0xFF);
    hash ^= ((instance->key1_low >> 16) & 0xFF);
    hash ^= ((instance->key1_low >> 24) & 0xFF);
    hash ^= (instance->key1_high & 0xFF);
    hash ^= ((instance->key1_high >> 8) & 0xFF);
    hash ^= ((instance->key1_high >> 16) & 0xFF);
    hash ^= ((instance->key1_high >> 24) & 0xFF);
    return hash;
}

SubGhzProtocolStatus subghz_protocol_decoder_vag_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderVAG* instance = context;

    
    if(!instance->decrypted && instance->data_count_bit >= 80) {
        vag_parse_data(instance);
    }

    
    uint64_t key1 = ((uint64_t)instance->key1_high << 32) | instance->key1_low;
    uint16_t key2_16bit = (uint16_t)(instance->key2_low & 0xFFFF);

    instance->generic.data = key1;
    instance->generic.data_count_bit = instance->data_count_bit;

    
    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);

    if(ret == SubGhzProtocolStatusOk) {
        
        uint8_t key2_bytes[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        key2_bytes[6] = (uint8_t)((key2_16bit >> 8) & 0xFF);
        key2_bytes[7] = (uint8_t)(key2_16bit & 0xFF);
        flipper_format_write_hex(flipper_format, "Key2", key2_bytes, 8);

        
        uint32_t type = instance->vag_type;
        flipper_format_write_uint32(flipper_format, "Type", &type, 1);

        
        if(instance->decrypted && instance->key_idx != 0xFF) {
            uint32_t key_idx_temp = instance->key_idx;
            flipper_format_write_uint32(flipper_format, "KeyIdx", &key_idx_temp, 1);
        }
        if(instance->decrypted) {
            // Write Cnt so Counter BruteForce option appears in saved menu
            uint32_t cnt_tmp = instance->cnt;
            flipper_format_write_uint32(flipper_format, "Cnt", &cnt_tmp, 1);
            uint32_t serial_tmp = instance->serial;
            flipper_format_write_uint32(flipper_format, "Serial", &serial_tmp, 1);
        }
    }

    return ret;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_vag_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderVAG* instance = context;

    SubGhzProtocolStatus ret = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, subghz_protocol_vag_const.min_count_bit_for_found);

    if(ret == SubGhzProtocolStatusOk) {
        uint64_t key1 = instance->generic.data;
        instance->key1_low = (uint32_t)key1;
        instance->key1_high = (uint32_t)(key1 >> 32);

        uint8_t key2_bytes[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        flipper_format_rewind(flipper_format);
        if(flipper_format_read_hex(flipper_format, "Key2", key2_bytes, 8)) {
            uint16_t key2_16bit = ((uint16_t)key2_bytes[6] << 8) | (uint16_t)key2_bytes[7];
            instance->key2_low = (uint32_t)key2_16bit & 0xFFFF;
            instance->key2_high = 0;
        }

        uint32_t type = 0;
        flipper_format_rewind(flipper_format);
        if(flipper_format_read_uint32(flipper_format, "Type", &type, 1)) {
            instance->vag_type = (uint8_t)type;
        }

        instance->data_count_bit = instance->generic.data_count_bit;

        instance->decrypted = false;
        vag_parse_data(instance);
        
        if(subghz_custom_btn_get_original() == 0) {
            subghz_custom_btn_set_original(vag_btn_to_custom(instance->btn));
        }
        subghz_custom_btn_set_max(4);
    }

    return ret;
}

typedef struct SubGhzProtocolEncoderVAG {
    SubGhzProtocolEncoderBase base;
    SubGhzBlockGeneric generic;

    uint32_t key1_low;
    uint32_t key1_high;
    uint32_t key2_low;
    uint32_t key2_high;
    uint32_t serial;
    uint32_t cnt;
    uint8_t vag_type;
    uint8_t btn;
    uint8_t dispatch_byte;
    uint8_t key_idx;

    size_t repeat;
    size_t front;
    size_t size_upload;
    LevelDuration* upload;
    bool is_running;
} SubGhzProtocolEncoderVAG;

static bool vag_aut64_encrypt(uint8_t* block, int key_index) {
    struct aut64_key* key = protocol_vag_get_key(key_index + 1);
    if(!key) {
        return false;
    }
    aut64_encrypt(*key, block);
    return true;
}

static uint8_t vag_get_dispatch_byte(uint8_t btn, uint8_t vag_type) {
    if(vag_type == 1 || vag_type == 2) {
        switch(btn) {
        case 0x20:
        case 2:
            return 0x2A;
        case 0x40:
        case 4:
            return 0x46;
        case 0x10:
        case 1:
            return 0x1C;
        case 0x80:
        case 8:
            return 0x88;  
        default:
            return 0x2A;
        }
    } else {
        switch(btn) {
        case 0x20:
        case 2:
            return 0x2B;
        case 0x40:
        case 4:
            return 0x47;
        case 0x10:
        case 1:
            return 0x1D;
        case 0x80:
        case 8:
            return 0x89;  
        default:
            return 0x2B;
        }
    }
}

static uint8_t vag_btn_to_byte(uint8_t btn, uint8_t vag_type) {
    
    
    
    
    uint8_t nibble;
    if(btn == 0x10) nibble = 1;
    else if(btn == 0x20) nibble = 2;
    else if(btn == 0x40) nibble = 4;
    else if(btn == 0x80) nibble = 8;
    else nibble = 2;  
    
    if(vag_type == 1) {
        return nibble;  
    } else {
        
        return btn;  
    }
}

static void vag_encoder_build_type1(SubGhzProtocolEncoderVAG* instance) {

    size_t index = 0;
    LevelDuration* upload = instance->upload;

    uint8_t btn_byte = vag_btn_to_byte(instance->btn, 1);
    uint8_t dispatch = vag_get_dispatch_byte(btn_byte, 1);
    instance->dispatch_byte = dispatch;

    uint8_t block[8];
    uint8_t type_byte = (uint8_t)(instance->key1_high >> 24);

    block[0] = (uint8_t)(instance->serial >> 24);
    block[1] = (uint8_t)(instance->serial >> 16);
    block[2] = (uint8_t)(instance->serial >> 8);
    block[3] = (uint8_t)(instance->serial);

    block[4] = (uint8_t)(instance->cnt);
    block[5] = (uint8_t)(instance->cnt >> 8);
    block[6] = (uint8_t)(instance->cnt >> 16);

    block[7] = btn_byte;

    int key_idx = (instance->key_idx != 0xFF) ? instance->key_idx : 0;

    if(!vag_aut64_encrypt(block, key_idx)) {
        instance->size_upload = 0;
        return;
    }

    instance->key1_high =
        ((uint32_t)type_byte << 24) | ((uint32_t)block[0] << 16) | ((uint32_t)block[1] << 8) |
        (uint32_t)block[2];
    instance->key1_low = ((uint32_t)block[3] << 24) | ((uint32_t)block[4] << 16) |
                         ((uint32_t)block[5] << 8) | (uint32_t)block[6];
    uint32_t key2_upper = ((uint32_t)(block[7] & 0xFF) << 8);
    uint32_t key2_lower = (uint32_t)(dispatch & 0xFF);
    instance->key2_low = (key2_upper | key2_lower) & 0xFFFF;
    instance->key2_high = 0;

    for(int i = 0; i < 220; i++) {
        upload[index++] = level_duration_make(true, 300);
        upload[index++] = level_duration_make(false, 300);
    }
    upload[index++] = level_duration_make(false, 300);
    upload[index++] = level_duration_make(true, 300);

    uint16_t prefix = 0xAF3F;
    for(int i = 15; i >= 0; i--) {
        bool bit = (prefix >> i) & 1;
        if(bit) {
            upload[index++] = level_duration_make(true, 300);
            upload[index++] = level_duration_make(false, 300);
        } else {
            upload[index++] = level_duration_make(false, 300);
            upload[index++] = level_duration_make(true, 300);
        }
    }

    uint64_t key1 = ((uint64_t)instance->key1_high << 32) | instance->key1_low;
    uint64_t key1_inv = ~key1;

    for(int i = 63; i >= 0; i--) {
        bool bit = (key1_inv >> i) & 1;
        if(bit) {
            upload[index++] = level_duration_make(true, 300);
            upload[index++] = level_duration_make(false, 300);
        } else {
            upload[index++] = level_duration_make(false, 300);
            upload[index++] = level_duration_make(true, 300);
        }
    }

    uint16_t key2 = (uint16_t)(instance->key2_low & 0xFFFF);
    uint16_t key2_inv = ~key2;

    for(int i = 15; i >= 0; i--) {
        bool bit = (key2_inv >> i) & 1;
        if(bit) {
            upload[index++] = level_duration_make(true, 300);
            upload[index++] = level_duration_make(false, 300);
        } else {
            upload[index++] = level_duration_make(false, 300);
            upload[index++] = level_duration_make(true, 300);
        }
    }

    upload[index++] = level_duration_make(false, 15000);

    instance->size_upload = index;
    
    if(index != 635) {
    }
}

static void vag_encoder_build_type2(SubGhzProtocolEncoderVAG* instance) {

    size_t index = 0;
    LevelDuration* upload = instance->upload;

    uint8_t btn_byte = vag_btn_to_byte(instance->btn, 2);
    uint8_t dispatch = vag_get_dispatch_byte(btn_byte, 2);
    instance->dispatch_byte = dispatch;

    uint8_t type_byte = (uint8_t)(instance->key1_high >> 24);

    uint8_t block[8];
    block[0] = (uint8_t)(instance->serial >> 24);
    block[1] = (uint8_t)(instance->serial >> 16);
    block[2] = (uint8_t)(instance->serial >> 8);
    block[3] = (uint8_t)(instance->serial);

    block[4] = (uint8_t)(instance->cnt);
    block[5] = (uint8_t)(instance->cnt >> 8);
    block[6] = (uint8_t)(instance->cnt >> 16);

    block[7] = btn_byte;

    uint32_t v0 = ((uint32_t)block[0] << 24) | ((uint32_t)block[1] << 16) |
                  ((uint32_t)block[2] << 8) | (uint32_t)block[3];
    uint32_t v1 = ((uint32_t)block[4] << 24) | ((uint32_t)block[5] << 16) |
                  ((uint32_t)block[6] << 8) | (uint32_t)block[7];

    vag_tea_encrypt(&v0, &v1, vag_tea_key_schedule);

    block[0] = (uint8_t)(v0 >> 24);
    block[1] = (uint8_t)(v0 >> 16);
    block[2] = (uint8_t)(v0 >> 8);
    block[3] = (uint8_t)(v0);
    block[4] = (uint8_t)(v1 >> 24);
    block[5] = (uint8_t)(v1 >> 16);
    block[6] = (uint8_t)(v1 >> 8);
    block[7] = (uint8_t)(v1);

    instance->key1_high =
        ((uint32_t)type_byte << 24) | ((uint32_t)block[0] << 16) | ((uint32_t)block[1] << 8) |
        (uint32_t)block[2];
    instance->key1_low = ((uint32_t)block[3] << 24) | ((uint32_t)block[4] << 16) |
                         ((uint32_t)block[5] << 8) | (uint32_t)block[6];
    uint32_t key2_upper = ((uint32_t)(block[7] & 0xFF) << 8);
    uint32_t key2_lower = (uint32_t)(dispatch & 0xFF);
    instance->key2_low = (key2_upper | key2_lower) & 0xFFFF;
    instance->key2_high = 0;

    for(int i = 0; i < 220; i++) {
        upload[index++] = level_duration_make(true, 300);
        upload[index++] = level_duration_make(false, 300);
    }
    upload[index++] = level_duration_make(false, 300);
    upload[index++] = level_duration_make(true, 300);

    uint16_t prefix = 0xAF1C;
    for(int i = 15; i >= 0; i--) {
        bool bit = (prefix >> i) & 1;
        if(bit) {
            upload[index++] = level_duration_make(true, 300);
            upload[index++] = level_duration_make(false, 300);
        } else {
            upload[index++] = level_duration_make(false, 300);
            upload[index++] = level_duration_make(true, 300);
        }
    }

    uint64_t key1 = ((uint64_t)instance->key1_high << 32) | instance->key1_low;
    uint64_t key1_inv = ~key1;

    for(int i = 63; i >= 0; i--) {
        bool bit = (key1_inv >> i) & 1;
        if(bit) {
            upload[index++] = level_duration_make(true, 300);
            upload[index++] = level_duration_make(false, 300);
        } else {
            upload[index++] = level_duration_make(false, 300);
            upload[index++] = level_duration_make(true, 300);
        }
    }

    uint16_t key2 = (uint16_t)(instance->key2_low & 0xFFFF);
    uint16_t key2_inv = ~key2;

    for(int i = 15; i >= 0; i--) {
        bool bit = (key2_inv >> i) & 1;
        if(bit) {
            upload[index++] = level_duration_make(true, 300);
            upload[index++] = level_duration_make(false, 300);
        } else {
            upload[index++] = level_duration_make(false, 300);
            upload[index++] = level_duration_make(true, 300);
        }
    }

    upload[index++] = level_duration_make(false, 15000);

    instance->size_upload = index;
}

static void vag_encoder_build_type3_4(SubGhzProtocolEncoderVAG* instance) {

    size_t index = 0;
    LevelDuration* upload = instance->upload;

    uint8_t btn_byte = vag_btn_to_byte(instance->btn, instance->vag_type);
    uint8_t dispatch = vag_get_dispatch_byte(btn_byte, instance->vag_type);
    instance->dispatch_byte = dispatch;

    uint8_t type_byte = (uint8_t)(instance->key1_high >> 24);

    uint8_t block[8];
    block[0] = (uint8_t)(instance->serial >> 24);
    block[1] = (uint8_t)(instance->serial >> 16);
    block[2] = (uint8_t)(instance->serial >> 8);
    block[3] = (uint8_t)(instance->serial);
    block[4] = (uint8_t)(instance->cnt);
    block[5] = (uint8_t)(instance->cnt >> 8);
    block[6] = (uint8_t)(instance->cnt >> 16);
    block[7] = btn_byte;

    int key_idx;
    if(instance->key_idx != 0xFF) {
        key_idx = instance->key_idx;
    } else {
        key_idx = (instance->vag_type == 4) ? 2 : 1;
    }

    if(!vag_aut64_encrypt(block, key_idx)) {
        instance->size_upload = 0;
        return;
    }

    instance->key1_high =
        ((uint32_t)type_byte << 24) | ((uint32_t)block[0] << 16) | ((uint32_t)block[1] << 8) |
        (uint32_t)block[2];
    instance->key1_low = ((uint32_t)block[3] << 24) | ((uint32_t)block[4] << 16) |
                         ((uint32_t)block[5] << 8) | (uint32_t)block[6];
    uint32_t key2_upper = ((uint32_t)(block[7] & 0xFF) << 8);
    uint32_t key2_lower = (uint32_t)(dispatch & 0xFF);
    instance->key2_low = (key2_upper | key2_lower) & 0xFFFF;
    instance->key2_high = 0;

    uint64_t key1 = ((uint64_t)instance->key1_high << 32) | instance->key1_low;
    uint16_t key2 = (uint16_t)(instance->key2_low & 0xFFFF);
    

    for(int repeat = 0; repeat < 2; repeat++) {
        
        for(int i = 0; i < 45; i++) {
            upload[index++] = level_duration_make(true, 500);
            upload[index++] = level_duration_make(false, 500);
        }

        upload[index++] = level_duration_make(true, 1000);
        upload[index++] = level_duration_make(false, 500);

        for(int i = 0; i < 3; i++) {
            upload[index++] = level_duration_make(true, 750);
            upload[index++] = level_duration_make(false, 750);
        }

        uint8_t consecutive_same = 0;
        bool prev_level = true;
        
        for(int i = 63; i >= 0; i--) {
            bool bit = (key1 >> i) & 1;
            bool first_level = bit ? true : false;
            
            if(first_level == prev_level) {
                consecutive_same++;
            }
            
            if(bit) {
                upload[index++] = level_duration_make(true, 500);
                upload[index++] = level_duration_make(false, 500);
                prev_level = false;
            } else {
                upload[index++] = level_duration_make(false, 500);
                upload[index++] = level_duration_make(true, 500);
                prev_level = true;
            }
        }

        bool last_level = false;
        for(int i = 15; i >= 0; i--) {
            bool bit = (key2 >> i) & 1;
            if(bit) {
                upload[index++] = level_duration_make(true, 500);
                upload[index++] = level_duration_make(false, 500);
                last_level = false;
            } else {
                upload[index++] = level_duration_make(false, 500);
                upload[index++] = level_duration_make(true, 500);
                last_level = true;
            }
        }

        if(!last_level) {
            upload[index++] = level_duration_make(false, 10000);
        } else {
            upload[index++] = level_duration_make(false, 10000);
        }
        
    }

    instance->size_upload = index;
    
    if(index != 518) {
    }
}

void subghz_protocol_decoder_vag_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderVAG* instance = context;

    if(!instance->decrypted && instance->data_count_bit >= 80) {
        vag_parse_data(instance);
    }

    uint64_t key1 = ((uint64_t)instance->key1_high << 32) | instance->key1_low;
    uint16_t key2 = (uint16_t)(instance->key2_low & 0xFFFF);

    uint8_t type_byte = (uint8_t)(instance->key1_high >> 24);
    const char* vehicle_name;
    switch(type_byte) {
    case 0x00:
        vehicle_name = "VAG NEW";
        break;
    case 0xC0:
        vehicle_name = "VAG OLD";
        break;
    case 0xC1:
        vehicle_name = "AUDI";
        break;
    case 0xC2:
        vehicle_name = "SEAT";
        break;
    case 0xC3:
        vehicle_name = "SKODA";
        break;
    default:
        vehicle_name = "VAG GEN";
        break;
    }

    if(instance->decrypted) {
        furi_string_cat_printf(
            output,
            "%s %dbit\r\n"
            "Key1:%08lX%08lX\r\n"
            "Key2:%04X KeyIdx:%d\r\n"
            "Ser:%08lX Cnt:%06lX\r\n"
            "Btn:[%s]",
            vehicle_name,
            instance->data_count_bit,
            (unsigned long)(key1 >> 32),
            (unsigned long)(key1 & 0xFFFFFFFF),
            key2,
            instance->key_idx,
            (unsigned long)instance->serial,
            (unsigned long)instance->cnt,
            vag_button_name(instance->btn));
    } else {
        furi_string_cat_printf(
            output,
            "%s %dbit\r\n"
            "Key1:%08lX%08lX\r\n"
            "Key2:%04X (corrupted)\r\n",
            vehicle_name,
            instance->data_count_bit,
            (unsigned long)(key1 >> 32),
            (unsigned long)(key1 & 0xFFFFFFFF),
            key2);
    }
}

#define VAG_ENCODER_UPLOAD_MAX_SIZE 2560

void* subghz_protocol_encoder_vag_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);

    SubGhzProtocolEncoderVAG* instance = malloc(sizeof(SubGhzProtocolEncoderVAG));
    instance->base.protocol = &subghz_protocol_vag;
    instance->generic.protocol_name = instance->base.protocol->name;

    instance->upload = malloc(VAG_ENCODER_UPLOAD_MAX_SIZE * sizeof(LevelDuration));
    instance->size_upload = 0;
    instance->repeat = 1;  
    instance->front = 0;
    instance->is_running = false;

    instance->key1_low = 0;
    instance->key1_high = 0;
    instance->key2_low = 0;
    instance->key2_high = 0;
    instance->serial = 0;
    instance->cnt = 0;
    instance->vag_type = 0;
    instance->btn = 0;
    instance->dispatch_byte = 0;
    instance->key_idx = 0xFF;

    protocol_vag_load_keys();

    return instance;
}

void subghz_protocol_encoder_vag_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderVAG* instance = context;
    if(instance->upload) {
        free(instance->upload);
    }
    free(instance);
}

void subghz_protocol_encoder_vag_stop(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderVAG* instance = context;
    instance->is_running = false;
}

LevelDuration subghz_protocol_encoder_vag_yield(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderVAG* instance = context;

    if(!instance->is_running || instance->repeat == 0) {
        if(instance->is_running) {
        }
        instance->is_running = false;
        return level_duration_reset();
    }

    LevelDuration ret = instance->upload[instance->front];
    instance->front++;

    if(instance->front >= instance->size_upload) {
        instance->front = 0;
        instance->repeat--;
    }

    return ret;
}

SubGhzProtocolStatus
    subghz_protocol_encoder_vag_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolEncoderVAG* instance = context;

    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    do {
        ret = subghz_block_generic_deserialize(&instance->generic, flipper_format);
        if(ret != SubGhzProtocolStatusOk) {
            break;
        }

        uint64_t key1 = instance->generic.data;
        instance->key1_low = (uint32_t)key1;
        instance->key1_high = (uint32_t)(key1 >> 32);

        uint8_t key2_bytes[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        flipper_format_rewind(flipper_format);
        if(!flipper_format_read_hex(flipper_format, "Key2", key2_bytes, 8)) {
            ret = SubGhzProtocolStatusErrorParserOthers;
            break;
        }
        uint16_t key2_16bit = ((uint16_t)key2_bytes[6] << 8) | (uint16_t)key2_bytes[7];
        instance->key2_low = (uint32_t)key2_16bit & 0xFFFF;
        instance->key2_high = 0;

        uint32_t type = 0;
        flipper_format_rewind(flipper_format);
        if(!flipper_format_read_uint32(flipper_format, "Type", &type, 1)) {
            type = 0;
        }
        instance->vag_type = (uint8_t)type;

        
        uint32_t file_key_idx = 0xFF;
        flipper_format_rewind(flipper_format);
        bool has_key_idx = flipper_format_read_uint32(flipper_format, "KeyIdx", &file_key_idx, 1);
        instance->key_idx = has_key_idx ? (uint8_t)file_key_idx : 0xFF;

        
        SubGhzProtocolDecoderVAG decoder;
        memset(&decoder, 0, sizeof(decoder));
        decoder.key1_low = instance->key1_low;
        decoder.key1_high = instance->key1_high;
        decoder.key2_low = instance->key2_low;
        decoder.key2_high = instance->key2_high;
        decoder.vag_type = instance->vag_type;
        decoder.data_count_bit = 80;
        decoder.key_idx = instance->key_idx;
        vag_parse_data(&decoder);

        if(decoder.decrypted) {
            instance->serial = decoder.serial;
            instance->cnt = decoder.cnt;
            instance->btn = decoder.btn;
            if(instance->key_idx == 0xFF) {
                instance->key_idx = decoder.key_idx;
            }
        } else {
            
            instance->serial = 0;
            instance->cnt = 0;
            instance->btn = 0x20;  
        }
        
        if(subghz_custom_btn_get_original() == 0) {
            subghz_custom_btn_set_original(vag_btn_to_custom(instance->btn));
        }
        subghz_custom_btn_set_max(4);
        
        uint8_t selected_custom;
        if(subghz_custom_btn_get() == SUBGHZ_CUSTOM_BTN_OK) {
            selected_custom = subghz_custom_btn_get_original();
        } else {
            selected_custom = subghz_custom_btn_get();
        }
        
        uint8_t new_btn = vag_custom_to_btn(selected_custom, instance->btn);
        instance->btn = new_btn;
        
        
        
        uint32_t mult = furi_hal_subghz_get_rolling_counter_mult();
        instance->cnt = (instance->cnt + mult) & 0xFFFFFF;

        uint8_t type_byte = (uint8_t)(instance->key1_high >> 24);
        if(instance->vag_type == 1 && type_byte == 0x00) {
            instance->vag_type = 2;
        }

        switch(instance->vag_type) {
        case 1:
            vag_encoder_build_type1(instance);
            break;
        case 2:
            vag_encoder_build_type2(instance);
            break;
        case 3:
        case 4:
            vag_encoder_build_type3_4(instance);
            break;
        default:
            instance->vag_type = 1;
            vag_encoder_build_type1(instance);
            break;
        }

        if(instance->size_upload == 0) {
            ret = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }

        flipper_format_rewind(flipper_format);
        uint8_t key1_bytes[8];
        key1_bytes[0] = (uint8_t)(instance->key1_high >> 24);
        key1_bytes[1] = (uint8_t)(instance->key1_high >> 16);
        key1_bytes[2] = (uint8_t)(instance->key1_high >> 8);
        key1_bytes[3] = (uint8_t)(instance->key1_high);
        key1_bytes[4] = (uint8_t)(instance->key1_low >> 24);
        key1_bytes[5] = (uint8_t)(instance->key1_low >> 16);
        key1_bytes[6] = (uint8_t)(instance->key1_low >> 8);
        key1_bytes[7] = (uint8_t)(instance->key1_low);
        if(!flipper_format_update_hex(flipper_format, "Key", key1_bytes, 8)) {
        }

        flipper_format_rewind(flipper_format);
        instance->key2_high = 0;
        uint16_t key2_write = (uint16_t)(instance->key2_low & 0xFFFF);
        instance->key2_low = (uint32_t)key2_write;
        uint8_t key2_write_bytes[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        key2_write_bytes[6] = (uint8_t)((key2_write >> 8) & 0xFF);
        key2_write_bytes[7] = (uint8_t)(key2_write & 0xFF);
        if(!flipper_format_update_hex(flipper_format, "Key2", key2_write_bytes, 8)) {
        }

        
        if(instance->key_idx != 0xFF) {
            flipper_format_rewind(flipper_format);
            uint32_t key_idx32 = instance->key_idx;
            if(!flipper_format_update_uint32(flipper_format, "KeyIdx", &key_idx32, 1)) {
                flipper_format_rewind(flipper_format);
                flipper_format_insert_or_update_uint32(flipper_format, "KeyIdx", &key_idx32, 1);
            }
        }

        
        flipper_format_rewind(flipper_format);
        uint32_t type32 = instance->vag_type;
        if(!flipper_format_update_uint32(flipper_format, "Type", &type32, 1)) {
            flipper_format_rewind(flipper_format);
            flipper_format_insert_or_update_uint32(flipper_format, "Type", &type32, 1);
        }

        

        instance->repeat = 1;  
        instance->front = 0;
        instance->is_running = true;

        ret = SubGhzProtocolStatusOk;
    } while(false);

    if(ret != SubGhzProtocolStatusOk) {
    }

    return ret;
}
