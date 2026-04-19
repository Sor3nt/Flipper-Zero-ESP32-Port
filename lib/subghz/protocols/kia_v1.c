#include "kia_v1.h"

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"
#include "../blocks/custom_btn_i.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "SubGhzProtocolKiaV1"

// Costanti esattamente come ProtoP irate
#define KIA_V1_TOTAL_BURSTS 3
#define KIA_V1_INTER_BURST_GAP_US 25000
#define KIA_V1_HEADER_PULSES 90

static const SubGhzBlockConst subghz_protocol_kia_v1_const = {
    .te_short = 800,
    .te_long = 1600,
    .te_delta = 200,
    .min_count_bit_for_found = 57,  // Come ProtoP irate
};

struct SubGhzProtocolDecoderKiaV1 {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
    uint16_t header_count;
    ManchesterState manchester_saved_state;  // Come ProtoP irate
    uint8_t crc;
    bool crc_check;
};

struct SubGhzProtocolEncoderKiaV1 {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
};

typedef enum {
    KiaV1DecoderStepReset = 0,
    KiaV1DecoderStepCheckPreamble,
    KiaV1DecoderStepDecodeData,  // Come ProtoP irate
} KiaV1DecoderStep;

const SubGhzProtocolDecoder subghz_protocol_kia_v1_decoder = {
    .alloc = subghz_protocol_decoder_kia_v1_alloc,
    .free = subghz_protocol_decoder_kia_v1_free,
    .feed = subghz_protocol_decoder_kia_v1_feed,
    .reset = subghz_protocol_decoder_kia_v1_reset,
    .get_hash_data = subghz_protocol_decoder_kia_v1_get_hash_data,
    .serialize = subghz_protocol_decoder_kia_v1_serialize,
    .deserialize = subghz_protocol_decoder_kia_v1_deserialize,
    .get_string = subghz_protocol_decoder_kia_v1_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_kia_v1_encoder = {
    .alloc = subghz_protocol_encoder_kia_v1_alloc,
    .free = subghz_protocol_encoder_kia_v1_free,
    .deserialize = subghz_protocol_encoder_kia_v1_deserialize,
    .stop = subghz_protocol_encoder_kia_v1_stop,
    .yield = subghz_protocol_encoder_kia_v1_yield,
};

const SubGhzProtocol subghz_protocol_kia_v1 = {
    .name = SUBGHZ_PROTOCOL_KIA_V1_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | 
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save |
            SubGhzProtocolFlag_Send | SubGhzProtocolFlag_Automotive,
    .decoder = &subghz_protocol_kia_v1_decoder,
    .encoder = &subghz_protocol_kia_v1_encoder,
};


static uint8_t kia_v1_crc4(const uint8_t* bytes, int count, uint8_t offset) {
    uint8_t crc = 0;
    for(int i = 0; i < count; i++) {
        uint8_t b = bytes[i];
        crc ^= ((b & 0x0F) ^ (b >> 4));
    }
    crc = (crc + offset) & 0x0F;
    return crc;
}



static void subghz_protocol_kia_v1_check_remote_controller(SubGhzProtocolDecoderKiaV1* instance) {
    // Estrazione campi esattamente come ProtoP irate
    instance->generic.serial = instance->generic.data >> 24;
    instance->generic.btn = (instance->generic.data >> 16) & 0xFF;
    instance->generic.cnt = ((instance->generic.data >> 4) & 0xF) << 8 | 
                            ((instance->generic.data >> 8) & 0xFF);

    uint8_t cnt_high = (instance->generic.cnt >> 8) & 0xF;
    uint8_t char_data[7];
    char_data[0] = (instance->generic.serial >> 24) & 0xFF;
    char_data[1] = (instance->generic.serial >> 16) & 0xFF;
    char_data[2] = (instance->generic.serial >> 8) & 0xFF;
    char_data[3] = instance->generic.serial & 0xFF;
    char_data[4] = instance->generic.btn;
    char_data[5] = instance->generic.cnt & 0xFF;
    
    uint8_t crc;
    if(cnt_high == 0) {
        uint8_t offset = (instance->generic.cnt >= 0x098) ? instance->generic.btn : 1;
        crc = kia_v1_crc4(char_data, 6, offset);
    } else if(cnt_high >= 0x6) {
        char_data[6] = cnt_high;
        crc = kia_v1_crc4(char_data, 7, 1);
    } else {
        crc = kia_v1_crc4(char_data, 6, 1);
    }

    instance->crc = cnt_high << 4 | crc;
    instance->crc_check = (crc == (instance->generic.data & 0xF));
    
    // Imposta bottoni custom
    if(subghz_custom_btn_get_original() == 0) {
        subghz_custom_btn_set_original(instance->generic.btn);
    }
    subghz_custom_btn_set_max(4);
}

static const char* subghz_protocol_kia_v1_get_name_button(uint8_t btn) {
    switch(btn) {
    case 0x1: return "Lock";
    case 0x2: return "Unlock";
    case 0x3: return "Trunk";
    case 0x4: return "Panic";
    default: return "Unknown";
    }
}



void* subghz_protocol_encoder_kia_v1_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderKiaV1* instance = malloc(sizeof(SubGhzProtocolEncoderKiaV1));
    
    instance->base.protocol = &subghz_protocol_kia_v1;
    instance->generic.protocol_name = instance->base.protocol->name;
    
    instance->encoder.repeat = 10;
    instance->encoder.size_upload = 1200;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    instance->encoder.front = 0;
    
    return instance;
}

void subghz_protocol_encoder_kia_v1_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderKiaV1* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void subghz_protocol_encoder_kia_v1_stop(void* context) {
    SubGhzProtocolEncoderKiaV1* instance = context;
    instance->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_kia_v1_yield(void* context) {
    SubGhzProtocolEncoderKiaV1* instance = context;

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

// ENCODER GET_UPLOAD
static void subghz_protocol_encoder_kia_v1_get_upload(SubGhzProtocolEncoderKiaV1* instance) {
    furi_assert(instance);
    size_t index = 0;

    // Calcolo CRC come ProtoP irate
    uint8_t cnt_high = (instance->generic.cnt >> 8) & 0xF;
    uint8_t char_data[7];
    char_data[0] = (instance->generic.serial >> 24) & 0xFF;
    char_data[1] = (instance->generic.serial >> 16) & 0xFF;
    char_data[2] = (instance->generic.serial >> 8) & 0xFF;
    char_data[3] = instance->generic.serial & 0xFF;
    char_data[4] = instance->generic.btn;
    char_data[5] = instance->generic.cnt & 0xFF;
    
    uint8_t crc;
    if(cnt_high == 0) {
        uint8_t offset = (instance->generic.cnt >= 0x098) ? instance->generic.btn : 1;
        crc = kia_v1_crc4(char_data, 6, offset);
    } else if(cnt_high >= 0x6) {
        char_data[6] = cnt_high;
        crc = kia_v1_crc4(char_data, 7, 1);
    } else {
        crc = kia_v1_crc4(char_data, 6, 1);
    }

    // Costruisci data esattamente come ProtoP irate
    instance->generic.data = (uint64_t)instance->generic.serial << 24 |
                             (uint64_t)instance->generic.btn << 16 | 
                             (uint64_t)(instance->generic.cnt & 0xFF) << 8 |
                             (uint64_t)((instance->generic.cnt >> 8) & 0xF) << 4 | 
                             crc;

    // 3 burst come ProtoP irate
    for(uint8_t burst = 0; burst < KIA_V1_TOTAL_BURSTS; burst++) {
        // Gap tra burst
        if(burst > 0) {
            instance->encoder.upload[index++] =
                level_duration_make(false, KIA_V1_INTER_BURST_GAP_US);
        }

        // 90 header pulses: LOW-HIGH con te_long
        for(int i = 0; i < KIA_V1_HEADER_PULSES; i++) {
            instance->encoder.upload[index++] =
                level_duration_make(false, (uint32_t)subghz_protocol_kia_v1_const.te_long);
            instance->encoder.upload[index++] =
                level_duration_make(true, (uint32_t)subghz_protocol_kia_v1_const.te_long);
        }

        // SHORT_LOW prima dei dati
        instance->encoder.upload[index++] =
            level_duration_make(false, (uint32_t)subghz_protocol_kia_v1_const.te_short);

        // Manchester encoding dei dati
        for(uint8_t i = instance->generic.data_count_bit; i > 1; i--) {
            if(bit_read(instance->generic.data, i - 2)) {
                // Bit 1: HIGH-LOW
                instance->encoder.upload[index++] =
                    level_duration_make(true, (uint32_t)subghz_protocol_kia_v1_const.te_short);
                instance->encoder.upload[index++] =
                    level_duration_make(false, (uint32_t)subghz_protocol_kia_v1_const.te_short);
            } else {
                // Bit 0: LOW-HIGH
                instance->encoder.upload[index++] =
                    level_duration_make(false, (uint32_t)subghz_protocol_kia_v1_const.te_short);
                instance->encoder.upload[index++] =
                    level_duration_make(true, (uint32_t)subghz_protocol_kia_v1_const.te_short);
            }
        }
    }

    instance->encoder.size_upload = index;
    instance->encoder.front = 0;
    
    FURI_LOG_I(TAG, "Upload built: size=%zu, data=0x%014llX", index, instance->generic.data);
}

SubGhzProtocolStatus subghz_protocol_encoder_kia_v1_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolEncoderKiaV1* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;
    
    do {
        ret = subghz_block_generic_deserialize(&instance->generic, flipper_format);
        if(ret != SubGhzProtocolStatusOk) {
            FURI_LOG_E(TAG, "Deserialize failed");
            break;
        }
        
        // Imposta data_count_bit
        instance->generic.data_count_bit = subghz_protocol_kia_v1_const.min_count_bit_for_found;
        
        // Estrai serial, btn, cnt dalla data (come ProtoP irate)
        instance->generic.serial = instance->generic.data >> 24;
        instance->generic.btn = (instance->generic.data >> 16) & 0xFF;
        instance->generic.cnt = ((instance->generic.data >> 4) & 0xF) << 8 | 
                                ((instance->generic.data >> 8) & 0xFF);
        
        FURI_LOG_I(TAG, "Deserialized: data=%014llX, serial=%08lX, btn=%02X, cnt=%03lX",
                   instance->generic.data, instance->generic.serial, 
                   instance->generic.btn, instance->generic.cnt);
        
        // Imposta bottone originale per custom buttons
        if(subghz_custom_btn_get_original() == 0) {
            subghz_custom_btn_set_original(instance->generic.btn);
        }
        subghz_custom_btn_set_max(4);
        
        // Incrementa counter
        if(instance->generic.cnt < 0xFFF) {
            instance->generic.cnt += furi_hal_subghz_get_rolling_counter_mult();
            if(instance->generic.cnt > 0xFFF) {
                instance->generic.cnt = 0;
            }
        } else {
            instance->generic.cnt = 0;
        }
        
        // Gestione bottoni custom
        uint8_t btn = subghz_custom_btn_get();
        if(btn != SUBGHZ_CUSTOM_BTN_OK) {
            instance->generic.btn = btn;
        }
        
        // Costruisci upload
        subghz_protocol_encoder_kia_v1_get_upload(instance);
        
        // Aggiorna file con nuova key
        if(!flipper_format_rewind(flipper_format)) {
            FURI_LOG_E(TAG, "Rewind error");
            ret = SubGhzProtocolStatusErrorParserOthers;
            break;
        }
        
        uint8_t key_data[sizeof(uint64_t)] = {0};
        for(size_t i = 0; i < sizeof(uint64_t); i++) {
            key_data[sizeof(uint64_t) - i - 1] = (instance->generic.data >> (i * 8)) & 0xFF;
        }
        if(!flipper_format_update_hex(flipper_format, "Key", key_data, sizeof(uint64_t))) {
            FURI_LOG_E(TAG, "Unable to update Key");
            ret = SubGhzProtocolStatusErrorParserKey;
            break;
        }
        
        instance->encoder.is_running = true;
        ret = SubGhzProtocolStatusOk;
        
    } while(false);
    
    return ret;
}



void* subghz_protocol_decoder_kia_v1_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderKiaV1* instance = malloc(sizeof(SubGhzProtocolDecoderKiaV1));
    instance->base.protocol = &subghz_protocol_kia_v1;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_kia_v1_free(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderKiaV1* instance = context;
    free(instance);
}

void subghz_protocol_decoder_kia_v1_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderKiaV1* instance = context;
    instance->decoder.parser_step = KiaV1DecoderStepReset;
}

// FEED 
void subghz_protocol_decoder_kia_v1_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderKiaV1* instance = context;

    ManchesterEvent event = ManchesterEventReset;

    switch(instance->decoder.parser_step) {
    case KiaV1DecoderStepReset:
        if((level) && (DURATION_DIFF(duration, subghz_protocol_kia_v1_const.te_long) <
                       subghz_protocol_kia_v1_const.te_delta)) {
            instance->decoder.parser_step = KiaV1DecoderStepCheckPreamble;
            instance->decoder.te_last = duration;
            instance->header_count = 0;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            manchester_advance(
                instance->manchester_saved_state,
                ManchesterEventReset,
                &instance->manchester_saved_state,
                NULL);
        }
        break;

    case KiaV1DecoderStepCheckPreamble:
        if(!level) {
            if((DURATION_DIFF(duration, subghz_protocol_kia_v1_const.te_long) <
                subghz_protocol_kia_v1_const.te_delta) &&
               (DURATION_DIFF(instance->decoder.te_last, subghz_protocol_kia_v1_const.te_long) <
                subghz_protocol_kia_v1_const.te_delta)) {
                instance->header_count++;
                instance->decoder.te_last = duration;
            } else {
                instance->decoder.parser_step = KiaV1DecoderStepReset;
            }
        }
        if(instance->header_count > 70) {
            if((!level) &&
               (DURATION_DIFF(duration, subghz_protocol_kia_v1_const.te_short) <
                subghz_protocol_kia_v1_const.te_delta) &&
               (DURATION_DIFF(instance->decoder.te_last, subghz_protocol_kia_v1_const.te_long) <
                subghz_protocol_kia_v1_const.te_delta)) {
                instance->decoder.decode_count_bit = 1;
                subghz_protocol_blocks_add_bit(&instance->decoder, 1);
                instance->header_count = 0;
                instance->decoder.parser_step = KiaV1DecoderStepDecodeData;
            }
        }
        break;

    case KiaV1DecoderStepDecodeData:
        if((DURATION_DIFF(duration, subghz_protocol_kia_v1_const.te_short) <
            subghz_protocol_kia_v1_const.te_delta)) {
            event = level ? ManchesterEventShortLow : ManchesterEventShortHigh;
        } else if((DURATION_DIFF(duration, subghz_protocol_kia_v1_const.te_long) <
                   subghz_protocol_kia_v1_const.te_delta)) {
            event = level ? ManchesterEventLongLow : ManchesterEventLongHigh;
        } else {
            // Durata non valida - reset completo
            instance->decoder.parser_step = KiaV1DecoderStepReset;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            break;
        }

        if(event != ManchesterEventReset) {
            bool data;
            bool data_ok = manchester_advance(
                instance->manchester_saved_state, event, &instance->manchester_saved_state, &data);
            if(data_ok) {
                instance->decoder.decode_data = (instance->decoder.decode_data << 1) | data;
                instance->decoder.decode_count_bit++;
            }
        }

        if(instance->decoder.decode_count_bit ==
           subghz_protocol_kia_v1_const.min_count_bit_for_found) {
            instance->generic.data = instance->decoder.decode_data;
            instance->generic.data_count_bit = instance->decoder.decode_count_bit;
            if(instance->base.callback)
                instance->base.callback(&instance->base, instance->base.context);

            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->decoder.parser_step = KiaV1DecoderStepReset;
        }
        break;
    }
}

uint8_t subghz_protocol_decoder_kia_v1_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderKiaV1* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_kia_v1_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderKiaV1* instance = context;
    return subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus subghz_protocol_decoder_kia_v1_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderKiaV1* instance = context;
    return subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, subghz_protocol_kia_v1_const.min_count_bit_for_found);
}

void subghz_protocol_decoder_kia_v1_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderKiaV1* instance = context;

    subghz_protocol_kia_v1_check_remote_controller(instance);
    
    uint32_t code_found_hi = instance->generic.data >> 32;
    uint32_t code_found_lo = instance->generic.data & 0xFFFFFFFF;

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Key:%06lX%08lX\r\n"
        "Sn:%08lX Cnt:%03lX\r\n"
        "Btn:%02X:[%s]\r\n"
        "CRC:%02X %s\r\n",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        code_found_hi,
        code_found_lo,
        instance->generic.serial,
        instance->generic.cnt,
        instance->generic.btn,
        subghz_protocol_kia_v1_get_name_button(instance->generic.btn),
        instance->crc,
        instance->crc_check ? "(OK)" : "(FAIL)");
}
