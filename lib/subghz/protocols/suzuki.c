#include "suzuki.h"

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"
#include "../blocks/custom_btn_i.h"

#define TAG "SuzukiProtocol"

static const SubGhzBlockConst subghz_protocol_suzuki_const = {
    .te_short = 250,
    .te_long = 500,
    .te_delta = 99,
    .min_count_bit_for_found = 64,
};

#define SUZUKI_GAP_TIME 2000
#define SUZUKI_GAP_DELTA 399
#define SUZUKI_MIN_PREAMBLE_COUNT 200
#define SUZUKI_ENCODER_PREAMBLE_COUNT 300

typedef struct SubGhzProtocolDecoderSuzuki
{
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
    uint16_t header_count;
} SubGhzProtocolDecoderSuzuki;

typedef struct SubGhzProtocolEncoderSuzuki
{
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
} SubGhzProtocolEncoderSuzuki;

typedef enum
{
    SuzukiDecoderStepReset = 0,
    SuzukiDecoderStepCountPreamble = 1,
    SuzukiDecoderStepDecodeData = 2,
} SuzukiDecoderStep;

static void suzuki_add_bit(SubGhzProtocolDecoderSuzuki* instance, uint8_t bit) {
    instance->decoder.decode_data = (instance->decoder.decode_data << 1) | bit;
    instance->decoder.decode_count_bit++;
}

static uint8_t suzuki_crc8(uint8_t* data, size_t len) {
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

static uint8_t suzuki_calculate_crc(uint64_t data) {
    uint8_t crc_data[6];
    crc_data[0] = (data >> 52) & 0xFF;
    crc_data[1] = (data >> 44) & 0xFF;
    crc_data[2] = (data >> 36) & 0xFF;
    crc_data[3] = (data >> 28) & 0xFF;
    crc_data[4] = (data >> 20) & 0xFF;
    crc_data[5] = (data >> 12) & 0xFF;
    return suzuki_crc8(crc_data, 6);
}

static uint8_t suzuki_custom_to_btn(uint8_t custom) {
    switch(custom) {
        case 1: return 3;
        case 2: return 4;
        case 3: return 2;
        case 4: return 1;
        default: return 4;
    }
}

static uint8_t suzuki_btn_to_custom(uint8_t btn) {
    switch(btn) {
        case 1: return 4;
        case 2: return 3;
        case 3: return 1;
        case 4: return 2;
        default: return 2;
    }
}

static bool suzuki_verify_crc(uint64_t data) {
    uint8_t received_crc = (data >> 4) & 0xFF;
    uint8_t calculated_crc = suzuki_calculate_crc(data);
    return (received_crc == calculated_crc);
}

const SubGhzProtocolDecoder subghz_protocol_suzuki_decoder = {
    .alloc = subghz_protocol_decoder_suzuki_alloc,
    .free = subghz_protocol_decoder_suzuki_free,
    .feed = subghz_protocol_decoder_suzuki_feed,
    .reset = subghz_protocol_decoder_suzuki_reset,
    .get_hash_data = subghz_protocol_decoder_suzuki_get_hash_data,
    .serialize = subghz_protocol_decoder_suzuki_serialize,
    .deserialize = subghz_protocol_decoder_suzuki_deserialize,
    .get_string = subghz_protocol_decoder_suzuki_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_suzuki_encoder = {
    .alloc = subghz_protocol_encoder_suzuki_alloc,
    .free = subghz_protocol_encoder_suzuki_free,
    .deserialize = subghz_protocol_encoder_suzuki_deserialize,
    .stop = subghz_protocol_encoder_suzuki_stop,
    .yield = subghz_protocol_encoder_suzuki_yield,
};

const SubGhzProtocol subghz_protocol_suzuki = {
    .name = SUBGHZ_PROTOCOL_SUZUKI_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_FM | 
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send | SubGhzProtocolFlag_Automotive,
    .decoder = &subghz_protocol_suzuki_decoder,
    .encoder = &subghz_protocol_suzuki_encoder,
};

void *subghz_protocol_decoder_suzuki_alloc(SubGhzEnvironment *environment)
{
    UNUSED(environment);
    SubGhzProtocolDecoderSuzuki *instance = malloc(sizeof(SubGhzProtocolDecoderSuzuki));
    instance->base.protocol = &subghz_protocol_suzuki;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_suzuki_free(void *context)
{
    furi_assert(context);
    SubGhzProtocolDecoderSuzuki *instance = context;
    free(instance);
}

void subghz_protocol_decoder_suzuki_reset(void *context)
{
    furi_assert(context);
    SubGhzProtocolDecoderSuzuki *instance = context;
    instance->decoder.parser_step = SuzukiDecoderStepReset;
}

void subghz_protocol_decoder_suzuki_feed(void *context, bool level, uint32_t duration)
{
    furi_assert(context);
    SubGhzProtocolDecoderSuzuki *instance = context;

    switch (instance->decoder.parser_step)
    {
    case SuzukiDecoderStepReset:
        if (!level) return;
        if (DURATION_DIFF(duration, subghz_protocol_suzuki_const.te_short) > subghz_protocol_suzuki_const.te_delta) return;
        
        instance->decoder.decode_data = 0;
        instance->decoder.decode_count_bit = 0;
        instance->decoder.parser_step = SuzukiDecoderStepCountPreamble;
        instance->header_count = 0;
        break;

    case SuzukiDecoderStepCountPreamble:
        if (level)
        {
            if (instance->header_count >= SUZUKI_MIN_PREAMBLE_COUNT)
            {
                if (DURATION_DIFF(duration, subghz_protocol_suzuki_const.te_long) <= subghz_protocol_suzuki_const.te_delta)
                {
                    instance->decoder.parser_step = SuzukiDecoderStepDecodeData;
                    suzuki_add_bit(instance, 1);
                }
            }
        }
        else
        {
            if (DURATION_DIFF(duration, subghz_protocol_suzuki_const.te_short) <= subghz_protocol_suzuki_const.te_delta)
            {
                instance->decoder.te_last = duration;
                instance->header_count++;
            }
            else
            {
                instance->decoder.parser_step = SuzukiDecoderStepReset;
            }
        }
        break;

    case SuzukiDecoderStepDecodeData:
        if (level)
        {
            if (duration < subghz_protocol_suzuki_const.te_long)
            {
                uint32_t diff_long = 500 - duration;
                if (diff_long > 99)
                {
                    uint32_t diff_short = (duration < 250) ? (250 - duration) : (duration - 250);
                    if (diff_short <= 99) suzuki_add_bit(instance, 0);
                }
                else
                {
                    suzuki_add_bit(instance, 1);
                }
            }
            else
            {
                uint32_t diff_long = duration - 500;
                if (diff_long <= 99) suzuki_add_bit(instance, 1);
            }
        }
        else
        {
            uint32_t diff_gap = (duration < SUZUKI_GAP_TIME) ? (SUZUKI_GAP_TIME - duration) : (duration - SUZUKI_GAP_TIME);
            
            if (diff_gap <= SUZUKI_GAP_DELTA)
            {
                if (instance->decoder.decode_count_bit == 64)
                {
                    instance->generic.data = instance->decoder.decode_data;
                    instance->generic.data_count_bit = 64;

                    if (suzuki_verify_crc(instance->generic.data))
                    {
                        uint64_t data = instance->generic.data;
                        uint32_t data_high = (uint32_t)(data >> 32);
                        uint32_t data_low = (uint32_t)data;
                        
                        instance->generic.serial = ((data_high & 0xFFF) << 16) | (data_low >> 16);
                        instance->generic.btn = (data_low >> 12) & 0xF;
                        instance->generic.cnt = (data_high << 4) >> 16;

                        if(subghz_custom_btn_get_original() == 0) {
                            uint8_t custom = suzuki_btn_to_custom(instance->generic.btn);
                            subghz_custom_btn_set_original(custom);
                        }
                        subghz_custom_btn_set_max(4);

                        if (instance->base.callback)
                        {
                            instance->base.callback(&instance->base, instance->base.context);
                        }
                    }
                }
                instance->decoder.decode_data = 0;
                instance->decoder.decode_count_bit = 0;
                instance->decoder.parser_step = SuzukiDecoderStepReset;
            }
        }
        break;
    }
}

uint8_t subghz_protocol_decoder_suzuki_get_hash_data(void *context)
{
    furi_assert(context);
    SubGhzProtocolDecoderSuzuki *instance = context;
    return subghz_protocol_blocks_get_hash_data(&instance->decoder, (instance->generic.data_count_bit / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_suzuki_serialize(void *context, FlipperFormat *flipper_format, SubGhzRadioPreset *preset)
{
    furi_assert(context);
    SubGhzProtocolDecoderSuzuki *instance = context;
    
    uint32_t temp_serial = instance->generic.serial;
    uint32_t temp_cnt = instance->generic.cnt;
    uint32_t temp_btn = instance->generic.btn;
    
    instance->generic.serial = 0;
    instance->generic.cnt = 0;
    instance->generic.btn = 0;
    
    SubGhzProtocolStatus ret = subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    
    instance->generic.serial = temp_serial;
    instance->generic.cnt = temp_cnt;
    instance->generic.btn = temp_btn;
    
    return ret;
    if(ret == SubGhzProtocolStatusOk) {
        flipper_format_write_uint32(flipper_format, "Serial", &temp_serial, 1);
        flipper_format_write_uint32(flipper_format, "Btn", &temp_btn, 1);
        flipper_format_write_uint32(flipper_format, "Cnt", &temp_cnt, 1);
    }
}

SubGhzProtocolStatus subghz_protocol_decoder_suzuki_deserialize(void *context, FlipperFormat *flipper_format)
{
    furi_assert(context);
    SubGhzProtocolDecoderSuzuki *instance = context;
    
    SubGhzProtocolStatus ret = subghz_block_generic_deserialize(&instance->generic, flipper_format);
    
    if(ret == SubGhzProtocolStatusOk) {
        uint64_t data = instance->generic.data;
        instance->generic.cnt = (uint32_t)((data >> 44) & 0xFFFFF);
        instance->generic.serial = (uint32_t)((data >> 16) & 0x0FFFFFFF);
        instance->generic.btn = (uint8_t)((data >> 12) & 0xF);
        
        if(subghz_custom_btn_get_original() == 0) {
            uint8_t custom = suzuki_btn_to_custom(instance->generic.btn);
            subghz_custom_btn_set_original(custom);
        }
        subghz_custom_btn_set_max(4);
    }
    
    return ret;
}

static const char *suzuki_get_button_name(uint8_t btn)
{
    switch (btn)
    {
    case 1: return "Panic";
    case 2: return "Trunk";
    case 3: return "Lock";
    case 4: return "Unlock";
    default: return "Unknown";
    }
}

void subghz_protocol_decoder_suzuki_get_string(void *context, FuriString *output) {
    furi_assert(context);
    SubGhzProtocolDecoderSuzuki *instance = context;
    
    uint64_t data = instance->generic.data;
    uint32_t key_high = (data >> 32) & 0xFFFFFFFF;
    uint32_t key_low = data & 0xFFFFFFFF;
    uint8_t received_crc = (data >> 4) & 0xFF;
    
    uint8_t calculated_crc = suzuki_calculate_crc(data);
    bool crc_valid = (received_crc == calculated_crc);
    
    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Key:%08lX%08lX\r\n"
        "Sn:%07lX Cnt:%04lX\r\n"
        "Btn:%02X:[%s]\r\n"
        "CRC:%02X %s",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        key_high,
        key_low,
        instance->generic.serial,
        instance->generic.cnt,
        instance->generic.btn,
        suzuki_get_button_name(instance->generic.btn),
        received_crc,
        crc_valid ? "(OK)" : "(FAIL)");
}

void *subghz_protocol_encoder_suzuki_alloc(SubGhzEnvironment *environment)
{
    UNUSED(environment);
    SubGhzProtocolEncoderSuzuki *instance = malloc(sizeof(SubGhzProtocolEncoderSuzuki));
    instance->base.protocol = &subghz_protocol_suzuki;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.upload = NULL;
    instance->encoder.is_running = false;
    return instance;
}

void subghz_protocol_encoder_suzuki_free(void *context)
{
    furi_assert(context);
    SubGhzProtocolEncoderSuzuki *instance = context;
    if(instance->encoder.upload) {
        free(instance->encoder.upload);
    }
    free(instance);
}

SubGhzProtocolStatus subghz_protocol_encoder_suzuki_deserialize(void *context, FlipperFormat *flipper_format)
{
    furi_assert(context);
    SubGhzProtocolEncoderSuzuki *instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    do {
        ret = subghz_block_generic_deserialize(&instance->generic, flipper_format);
        if (ret != SubGhzProtocolStatusOk) break;

        uint64_t data = instance->generic.data;
        
        instance->generic.cnt = (uint32_t)((data >> 44) & 0xFFFFF);
        instance->generic.serial = (uint32_t)((data >> 16) & 0x0FFFFFFF);
        instance->generic.btn = (uint8_t)((data >> 12) & 0xF);

        if(subghz_custom_btn_get_original() == 0) {
            uint8_t custom = suzuki_btn_to_custom(instance->generic.btn);
            subghz_custom_btn_set_original(custom);
        }
        subghz_custom_btn_set_max(4);

        uint32_t mult = furi_hal_subghz_get_rolling_counter_mult();
        instance->generic.cnt = (instance->generic.cnt + mult) & 0xFFFFF;

        uint8_t selected = subghz_custom_btn_get() == SUBGHZ_CUSTOM_BTN_OK ?
                          subghz_custom_btn_get_original() :
                          subghz_custom_btn_get();
        uint8_t btn = suzuki_custom_to_btn(selected);
        instance->generic.btn = btn;

        uint64_t new_data = 0;
        new_data |= ((uint64_t)(instance->generic.cnt & 0xFFFFF) << 44);
        new_data |= ((uint64_t)(instance->generic.serial & 0x0FFFFFFF) << 16);
        new_data |= ((uint64_t)(instance->generic.btn & 0xF) << 12);
        
        uint8_t crc = suzuki_calculate_crc(new_data);
        new_data |= ((uint64_t)crc << 4);
        
        instance->generic.data = new_data;

        if(!flipper_format_rewind(flipper_format)) {
            ret = SubGhzProtocolStatusErrorParserOthers;
            break;
        }

        uint8_t key_data[8];
        for(size_t i = 0; i < 8; i++) {
            key_data[i] = (instance->generic.data >> (56 - i * 8)) & 0xFF;
        }

        if(!flipper_format_update_hex(flipper_format, "Key", key_data, 8)) {
            ret = SubGhzProtocolStatusErrorParserKey;
            break;
        }

        size_t preamble_count = SUZUKI_ENCODER_PREAMBLE_COUNT;
        size_t bit_count = 64;
        instance->encoder.size_upload = (preamble_count * 2) + (bit_count * 2) + 1;

        if(instance->encoder.upload) {
            free(instance->encoder.upload);
        }
        instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));

        size_t index = 0;

        for (size_t i = 0; i < preamble_count; i++) {
            instance->encoder.upload[index++] = level_duration_make(true, subghz_protocol_suzuki_const.te_short);
            instance->encoder.upload[index++] = level_duration_make(false, subghz_protocol_suzuki_const.te_short);
        }

        for (size_t i = 0; i < bit_count; i++) {
            uint8_t bit = (instance->generic.data >> (63 - i)) & 1;
            
            if (bit) {
                instance->encoder.upload[index++] = level_duration_make(true, subghz_protocol_suzuki_const.te_long);
            } else {
                instance->encoder.upload[index++] = level_duration_make(true, subghz_protocol_suzuki_const.te_short);
            }
            instance->encoder.upload[index++] = level_duration_make(false, subghz_protocol_suzuki_const.te_short);
        }

        instance->encoder.upload[index++] = level_duration_make(false, SUZUKI_GAP_TIME);

        instance->encoder.is_running = true;
        instance->encoder.repeat = 5;
        instance->encoder.front = 0;

        ret = SubGhzProtocolStatusOk;

    } while (false);

    return ret;
}

void subghz_protocol_encoder_suzuki_stop(void *context)
{
    SubGhzProtocolEncoderSuzuki *instance = context;
    instance->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_suzuki_yield(void *context)
{
    SubGhzProtocolEncoderSuzuki *instance = context;

    if (instance->encoder.repeat == 0 || !instance->encoder.is_running)
    {
        instance->encoder.is_running = false;
        return level_duration_reset();
    }

    LevelDuration ret = instance->encoder.upload[instance->encoder.front];

    if (++instance->encoder.front == instance->encoder.size_upload)
    {
        instance->encoder.repeat--;
        instance->encoder.front = 0;
    }

    return ret;
}
