#include "kia_v0.h"

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"
#include "../blocks/custom_btn_i.h"

#define TAG "SubGhzProtocolKiaV0"

static const SubGhzBlockConst subghz_protocol_kia_const = {
    .te_short = 250,
    .te_long = 500,
    .te_delta = 100,
    .min_count_bit_for_found = 61,
};

struct SubGhzProtocolDecoderKIA {
    SubGhzProtocolDecoderBase base;

    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    uint16_t header_count;
};

struct SubGhzProtocolEncoderKIA {
    SubGhzProtocolEncoderBase base;

    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
};

typedef enum {
    KIADecoderStepReset = 0,
    KIADecoderStepCheckPreambula,
    KIADecoderStepSaveDuration,
    KIADecoderStepCheckDuration,
} KIADecoderStep;

const SubGhzProtocolDecoder subghz_protocol_kia_decoder = {
    .alloc = subghz_protocol_decoder_kia_alloc,
    .free = subghz_protocol_decoder_kia_free,

    .feed = subghz_protocol_decoder_kia_feed,
    .reset = subghz_protocol_decoder_kia_reset,

    .get_hash_data = subghz_protocol_decoder_kia_get_hash_data,
    .serialize = subghz_protocol_decoder_kia_serialize,
    .deserialize = subghz_protocol_decoder_kia_deserialize,
    .get_string = subghz_protocol_decoder_kia_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_kia_encoder = {
    .alloc = subghz_protocol_encoder_kia_alloc,
    .free = subghz_protocol_encoder_kia_free,

    .deserialize = subghz_protocol_encoder_kia_deserialize,
    .stop = subghz_protocol_encoder_kia_stop,
    .yield = subghz_protocol_encoder_kia_yield,
};

const SubGhzProtocol subghz_protocol_kia_v0 = {
    .name = SUBGHZ_PROTOCOL_KIA_V0_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send | SubGhzProtocolFlag_Automotive,

    .decoder = &subghz_protocol_kia_decoder,
    .encoder = &subghz_protocol_kia_encoder,
};

/**
 * CRC8 calculation for Kia protocol
 * Polynomial: 0x7F
 * Initial value: 0x00
 * MSB-first processing
 */
static uint8_t kia_crc8(uint8_t* data, size_t len) {
    uint8_t crc = 0x00;
    for(size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for(size_t j = 0; j < 8; j++) {
            if((crc & 0x80) != 0)
                crc = (uint8_t)((crc << 1) ^ 0x7F);
            else
                crc <<= 1;
        }
    }
    return crc;
}

/**
 * Calculate CRC for the Kia data packet
 * CRC is calculated over bits 8-55 (6 bytes)
 */
static uint8_t kia_calculate_crc(uint64_t data) {
    uint8_t crc_data[6];
    crc_data[0] = (data >> 48) & 0xFF;
    crc_data[1] = (data >> 40) & 0xFF;
    crc_data[2] = (data >> 32) & 0xFF;
    crc_data[3] = (data >> 24) & 0xFF;
    crc_data[4] = (data >> 16) & 0xFF;
    crc_data[5] = (data >> 8) & 0xFF;
    
    return kia_crc8(crc_data, 6);
}

/**
 * Verify CRC of received data
 */
static bool kia_verify_crc(uint64_t data) {
    uint8_t received_crc = data & 0xFF;
    uint8_t calculated_crc = kia_calculate_crc(data);
    return (received_crc == calculated_crc);
}

// ============================================================================
// ENCODER IMPLEMENTATION
// ============================================================================

void* subghz_protocol_encoder_kia_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderKIA* instance = malloc(sizeof(SubGhzProtocolEncoderKIA));
    instance->base.protocol = &subghz_protocol_kia_v0;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.size_upload = 848;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.repeat = 1;
    instance->encoder.is_running = false;

    return instance;
}

void subghz_protocol_encoder_kia_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderKIA* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void subghz_protocol_encoder_kia_stop(void* context) {
    SubGhzProtocolEncoderKIA* instance = context;
    instance->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_kia_yield(void* context) {
    SubGhzProtocolEncoderKIA* instance = context;

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

/** 
 * Analysis of received data
 * @param instance Pointer to a SubGhzBlockGeneric* instance
 */
static void subghz_protocol_kia_check_remote_controller(SubGhzBlockGeneric* instance);

/**
 * Generating an upload from data.
 * @param instance Pointer to a SubGhzProtocolEncoderKIA instance
 * @return true On success
 */
static bool subghz_protocol_encoder_kia_get_upload(SubGhzProtocolEncoderKIA* instance) {
    furi_assert(instance);

    // Save original button
    if(subghz_custom_btn_get_original() == 0) {
        subghz_custom_btn_set_original(instance->generic.btn);
    }
    subghz_custom_btn_set_max(4);

    size_t index = 0;
    size_t size_upload = (instance->generic.data_count_bit * 2 + 32) * 2 + 540;
    if(size_upload > instance->encoder.size_upload) {
        FURI_LOG_E(
            TAG,
            "Size upload exceeds allocated encoder buffer. %i",
            instance->generic.data_count_bit);
        return false;
    } else {
        instance->encoder.size_upload = size_upload;
    }

    // Counter increment logic
    if(instance->generic.cnt < 0xFFFF) {
        if((instance->generic.cnt + furi_hal_subghz_get_rolling_counter_mult()) > 0xFFFF) {
            instance->generic.cnt = 0;
        } else {
            instance->generic.cnt += furi_hal_subghz_get_rolling_counter_mult();
        }
    } else if(instance->generic.cnt >= 0xFFFF) {
        instance->generic.cnt = 0;
    }

    // Get button (custom or original)
    // This allows button changing with directional keys in SubGhz app
    uint8_t btn = subghz_custom_btn_get() == SUBGHZ_CUSTOM_BTN_OK ?
                      subghz_custom_btn_get_original() :
                      subghz_custom_btn_get();
    
    // Update the generic button value for potential button changes
    instance->generic.btn = btn;

    // Build data packet
    uint64_t data = 0;
    
    // Bits 56-59: Fixed preamble (0x0F)
    data |= ((uint64_t)(0x0F) << 56);
    
    // Bits 40-55: Counter (16 bits)
    data |= ((uint64_t)(instance->generic.cnt & 0xFFFF) << 40);
    
    // Bits 12-39: Serial (28 bits)
    data |= ((uint64_t)(instance->generic.serial & 0x0FFFFFFF) << 12);
    
    // Bits 8-11: Button (4 bits)
    data |= ((uint64_t)(btn & 0x0F) << 8);
    
    // Bits 0-7: CRC
    uint8_t crc = kia_calculate_crc(data);
    data |= crc;
    
    instance->generic.data = data;

    // Send header (270 pulses of te_short)
    for(uint16_t i = 270; i > 0; i--) {
        instance->encoder.upload[index++] =
            level_duration_make(true, (uint32_t)subghz_protocol_kia_const.te_short);
        instance->encoder.upload[index++] =
            level_duration_make(false, (uint32_t)subghz_protocol_kia_const.te_short);
    }

    // Send 2 data bursts
    for(uint8_t h = 2; h > 0; h--) {
        // Send sync bits (15 pulses of te_short)
        for(uint8_t i = 15; i > 0; i--) {
            instance->encoder.upload[index++] =
                level_duration_make(true, (uint32_t)subghz_protocol_kia_const.te_short);
            instance->encoder.upload[index++] =
                level_duration_make(false, (uint32_t)subghz_protocol_kia_const.te_short);
        }

        // Send data bits (PWM encoding)
        for(uint8_t i = instance->generic.data_count_bit; i > 0; i--) {
            if(bit_read(instance->generic.data, i - 1)) {
                // Send bit 1: long pulse
                instance->encoder.upload[index++] =
                    level_duration_make(true, (uint32_t)subghz_protocol_kia_const.te_long);
                instance->encoder.upload[index++] =
                    level_duration_make(false, (uint32_t)subghz_protocol_kia_const.te_long);
            } else {
                // Send bit 0: short pulse
                instance->encoder.upload[index++] =
                    level_duration_make(true, (uint32_t)subghz_protocol_kia_const.te_short);
                instance->encoder.upload[index++] =
                    level_duration_make(false, (uint32_t)subghz_protocol_kia_const.te_short);
            }
        }

        // Send stop bit (3x te_long)
        instance->encoder.upload[index++] =
            level_duration_make(true, (uint32_t)subghz_protocol_kia_const.te_long * 3);
        instance->encoder.upload[index++] =
            level_duration_make(false, (uint32_t)subghz_protocol_kia_const.te_long * 3);
    }

    return true;
}

SubGhzProtocolStatus subghz_protocol_encoder_kia_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolEncoderKIA* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;
    
    do {
        ret = subghz_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            subghz_protocol_kia_const.min_count_bit_for_found);

        if(ret != SubGhzProtocolStatusOk) {
            break;
        }

        // Extract serial, button, counter from data
        subghz_protocol_kia_check_remote_controller(&instance->generic);

        // Verify CRC
        if(!kia_verify_crc(instance->generic.data)) {
            FURI_LOG_W(TAG, "CRC mismatch in loaded file");
            ret = SubGhzProtocolStatusErrorParserOthers;
            break;
        }

        if(!subghz_protocol_encoder_kia_get_upload(instance)) {
            ret = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }

        // Update the Key in the file with the new counter/button/CRC
        if(!flipper_format_rewind(flipper_format)) {
            FURI_LOG_E(TAG, "Rewind error");
            ret = SubGhzProtocolStatusErrorParserOthers;
            break;
        }
        uint8_t key_data[sizeof(uint64_t)] = {0};
        for(size_t i = 0; i < sizeof(uint64_t); i++) {
            key_data[sizeof(uint64_t) - i - 1] = (instance->generic.data >> i * 8) & 0xFF;
        }
        if(!flipper_format_update_hex(flipper_format, "Key", key_data, sizeof(uint64_t))) {
            FURI_LOG_E(TAG, "Unable to update Key");
            ret = SubGhzProtocolStatusErrorParserKey;
            break;
        }

        instance->encoder.is_running = true;
    } while(false);

    return ret;
}

// ============================================================================
// ENCODER HELPER FUNCTIONS
// ============================================================================

void subghz_protocol_encoder_kia_set_button(void* context, uint8_t button) {
    furi_assert(context);
    SubGhzProtocolEncoderKIA* instance = context;
    instance->generic.btn = button & 0x0F;
}

void subghz_protocol_encoder_kia_set_counter(void* context, uint16_t counter) {
    furi_assert(context);
    SubGhzProtocolEncoderKIA* instance = context;
    instance->generic.cnt = counter;
}

void subghz_protocol_encoder_kia_increment_counter(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderKIA* instance = context;
    if(instance->generic.cnt < 0xFFFF) {
        instance->generic.cnt++;
    } else {
        instance->generic.cnt = 0;
    }
}

uint16_t subghz_protocol_encoder_kia_get_counter(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderKIA* instance = context;
    return instance->generic.cnt;
}

uint8_t subghz_protocol_encoder_kia_get_button(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderKIA* instance = context;
    return instance->generic.btn;
}

// ============================================================================
// DECODER IMPLEMENTATION
// ============================================================================

void* subghz_protocol_decoder_kia_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderKIA* instance = malloc(sizeof(SubGhzProtocolDecoderKIA));
    instance->base.protocol = &subghz_protocol_kia_v0;
    instance->generic.protocol_name = instance->base.protocol->name;

    return instance;
}

void subghz_protocol_decoder_kia_free(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderKIA* instance = context;
    free(instance);
}

void subghz_protocol_decoder_kia_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderKIA* instance = context;
    instance->decoder.parser_step = KIADecoderStepReset;
}

void subghz_protocol_decoder_kia_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderKIA* instance = context;

    switch(instance->decoder.parser_step) {
    case KIADecoderStepReset:
        if((level) && (DURATION_DIFF(duration, subghz_protocol_kia_const.te_short) <
                       subghz_protocol_kia_const.te_delta)) {
            instance->decoder.parser_step = KIADecoderStepCheckPreambula;
            instance->decoder.te_last = duration;
            instance->header_count = 0;
        }
        break;
        
    case KIADecoderStepCheckPreambula:
        if(level) {
            if((DURATION_DIFF(duration, subghz_protocol_kia_const.te_short) <
                subghz_protocol_kia_const.te_delta) ||
               (DURATION_DIFF(duration, subghz_protocol_kia_const.te_long) <
                subghz_protocol_kia_const.te_delta)) {
                instance->decoder.te_last = duration;
            } else {
                instance->decoder.parser_step = KIADecoderStepReset;
            }
        } else if(
            (DURATION_DIFF(duration, subghz_protocol_kia_const.te_short) <
             subghz_protocol_kia_const.te_delta) &&
            (DURATION_DIFF(instance->decoder.te_last, subghz_protocol_kia_const.te_short) <
             subghz_protocol_kia_const.te_delta)) {
            // Found header
            instance->header_count++;
            break;
        } else if(
            (DURATION_DIFF(duration, subghz_protocol_kia_const.te_long) <
             subghz_protocol_kia_const.te_delta) &&
            (DURATION_DIFF(instance->decoder.te_last, subghz_protocol_kia_const.te_long) <
             subghz_protocol_kia_const.te_delta)) {
            // Found start bit
            if(instance->header_count > 15) {
                instance->decoder.parser_step = KIADecoderStepSaveDuration;
                instance->decoder.decode_data = 0;
                instance->decoder.decode_count_bit = 1;
                subghz_protocol_blocks_add_bit(&instance->decoder, 1);
            } else {
                instance->decoder.parser_step = KIADecoderStepReset;
            }
        } else {
            instance->decoder.parser_step = KIADecoderStepReset;
        }
        break;
        
    case KIADecoderStepSaveDuration:
        if(level) {
            if(duration >=
               (subghz_protocol_kia_const.te_long + subghz_protocol_kia_const.te_delta * 2UL)) {
                // Found stop bit
                instance->decoder.parser_step = KIADecoderStepReset;
                if(instance->decoder.decode_count_bit ==
                   subghz_protocol_kia_const.min_count_bit_for_found) {
                    instance->generic.data = instance->decoder.decode_data;
                    instance->generic.data_count_bit = instance->decoder.decode_count_bit;
                    
                    // Verify CRC before accepting the packet
                    if(kia_verify_crc(instance->generic.data)) {
                        if(instance->base.callback)
                            instance->base.callback(&instance->base, instance->base.context);
                    } else {
                        FURI_LOG_W(TAG, "CRC verification failed, packet rejected");
                    }
                }
                instance->decoder.decode_data = 0;
                instance->decoder.decode_count_bit = 0;
                break;
            } else {
                instance->decoder.te_last = duration;
                instance->decoder.parser_step = KIADecoderStepCheckDuration;
            }

        } else {
            instance->decoder.parser_step = KIADecoderStepReset;
        }
        break;
        
    case KIADecoderStepCheckDuration:
        if(!level) {
            if((DURATION_DIFF(instance->decoder.te_last, subghz_protocol_kia_const.te_short) <
                subghz_protocol_kia_const.te_delta) &&
               (DURATION_DIFF(duration, subghz_protocol_kia_const.te_short) <
                subghz_protocol_kia_const.te_delta)) {
                subghz_protocol_blocks_add_bit(&instance->decoder, 0);
                instance->decoder.parser_step = KIADecoderStepSaveDuration;
            } else if(
                (DURATION_DIFF(instance->decoder.te_last, subghz_protocol_kia_const.te_long) <
                 subghz_protocol_kia_const.te_delta) &&
                (DURATION_DIFF(duration, subghz_protocol_kia_const.te_long) <
                 subghz_protocol_kia_const.te_delta)) {
                subghz_protocol_blocks_add_bit(&instance->decoder, 1);
                instance->decoder.parser_step = KIADecoderStepSaveDuration;
            } else {
                instance->decoder.parser_step = KIADecoderStepReset;
            }
        } else {
            instance->decoder.parser_step = KIADecoderStepReset;
        }
        break;
    }
}

/** 
 * Analysis of received data
 * @param instance Pointer to a SubGhzBlockGeneric* instance
 */
static void subghz_protocol_kia_check_remote_controller(SubGhzBlockGeneric* instance) {
    /*
    *   0x0F 0112 43B04EC 1 7D
    *   0x0F 0113 43B04EC 1 DF
    *   0x0F 0114 43B04EC 1 30
    *   0x0F 0115 43B04EC 2 13
    *   0x0F 0116 43B04EC 3 F5
    *         CNT  Serial K CRC8 Kia
    */

    instance->serial = (uint32_t)((instance->data >> 12) & 0x0FFFFFFF);
    instance->btn = (instance->data >> 8) & 0x0F;
    instance->cnt = (instance->data >> 40) & 0xFFFF;

    if(subghz_custom_btn_get_original() == 0) {
        subghz_custom_btn_set_original(instance->btn);
    }

    subghz_custom_btn_set_max(4);
}

uint8_t subghz_protocol_decoder_kia_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderKIA* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_kia_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderKIA* instance = context;
    return subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    subghz_protocol_decoder_kia_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderKIA* instance = context;
    
    SubGhzProtocolStatus ret = subghz_block_generic_deserialize(&instance->generic, flipper_format);
    
    if(ret == SubGhzProtocolStatusOk) {
        if(instance->generic.data_count_bit < subghz_protocol_kia_const.min_count_bit_for_found) {
            ret = SubGhzProtocolStatusErrorParserBitCount;
        }
    }
    
    return ret;
}

static const char* subghz_protocol_kia_get_name_button(uint8_t btn) {
    const char* name_btn[5] = {"Unknown", "Lock", "Unlock", "Trunk", "Horn"};
    return name_btn[btn < 5 ? btn : 0];
}

void subghz_protocol_decoder_kia_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderKIA* instance = context;

    subghz_protocol_kia_check_remote_controller(&instance->generic);
    uint32_t code_found_hi = instance->generic.data >> 32;
    uint32_t code_found_lo = instance->generic.data & 0x00000000ffffffff;
    
    uint8_t received_crc = instance->generic.data & 0xFF;
    uint8_t calculated_crc = kia_calculate_crc(instance->generic.data);
    bool crc_valid = (received_crc == calculated_crc);

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Key:%08lX%08lX\r\n"
        "Sn:%07lX  Cnt:%04lX\r\n"
        "Btn:%02X:[%s]\r\n"
        "CRC:%02X %s",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        code_found_hi,
        code_found_lo,
        instance->generic.serial,
        instance->generic.cnt,
        instance->generic.btn,
        subghz_protocol_kia_get_name_button(instance->generic.btn),
        received_crc,
        crc_valid ? "(OK)" : "(FAIL)");
}

