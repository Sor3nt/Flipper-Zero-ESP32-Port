#include "kia_v5.h"

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"
#include "../blocks/custom_btn_i.h"
#include <lib/toolbox/manchester_decoder.h>
#include <lib/toolbox/manchester_encoder.h>

#define TAG "SubGhzProtocolKiaV5"

static const SubGhzBlockConst subghz_protocol_kia_v5_const = {
    .te_short = 400,
    .te_long = 800,
    .te_delta = 150,
    .min_count_bit_for_found = 64,
};

static const uint8_t keystore_bytes[] = {0x53, 0x54, 0x46, 0x52, 0x4b, 0x45, 0x30, 0x30};

static uint8_t reverse_byte(uint8_t b) {
    uint8_t r = 0;
    for(int i = 0; i < 8; i++) {
        if(b & (1 << i)) r |= (1 << (7 - i));
    }
    return r;
}

static uint64_t bit_reverse_64(uint64_t input) {
    uint64_t output = 0;
    for(int i = 0; i < 8; i++) {
        uint8_t byte = (input >> (i * 8)) & 0xFF;
        uint8_t reversed = reverse_byte(byte);
        output |= ((uint64_t)reversed << ((7 - i) * 8));
    }
    return output;
}

static uint8_t kia_v5_calculate_crc(uint64_t yek) {
    uint8_t crc = 0;
    for(int i = 0; i < 16; i++) {
        crc ^= (yek >> (i * 4)) & 0x0F;
    }
    return crc & 0x07;
}

static uint16_t mixer_decode(uint32_t encrypted) {
    uint8_t s0 = (encrypted & 0xFF);
    uint8_t s1 = (encrypted >> 8) & 0xFF;
    uint8_t s2 = (encrypted >> 16) & 0xFF;
    uint8_t s3 = (encrypted >> 24) & 0xFF;

    int round_index = 1;
    for(size_t i = 0; i < 18; i++) {
        uint8_t r = keystore_bytes[round_index] & 0xFF;
        int steps = 8;
        while(steps > 0) {
            uint8_t base;
            if((s3 & 0x40) == 0) {
                base = (s3 & 0x02) == 0 ? 0x74 : 0x2E;
            } else {
                base = (s3 & 0x02) == 0 ? 0x3A : 0x5C;
            }
            if(s2 & 0x08) {
                base = (((base >> 4) & 0x0F) | ((base & 0x0F) << 4)) & 0xFF;
            }
            if(s1 & 0x01) {
                base = ((base & 0x3F) << 2) & 0xFF;
            }
            if(s0 & 0x01) {
                base = (base << 1) & 0xFF;
            }
            uint8_t temp = (s3 ^ s1) & 0xFF;
            s3 = ((s3 & 0x7F) << 1) & 0xFF;
            if(s2 & 0x80) s3 |= 0x01;
            s2 = ((s2 & 0x7F) << 1) & 0xFF;
            if(s1 & 0x80) s2 |= 0x01;
            s1 = ((s1 & 0x7F) << 1) & 0xFF;
            if(s0 & 0x80) s1 |= 0x01;
            s0 = ((s0 & 0x7F) << 1) & 0xFF;
            uint8_t chk = (base ^ (r ^ temp)) & 0xFF;
            if(chk & 0x80) s0 |= 0x01;
            r = ((r & 0x7F) << 1) & 0xFF;
            steps--;
        }
        round_index = (round_index - 1) & 0x7;
    }
    return (s0 + (s1 << 8)) & 0xFFFF;
}

static uint32_t mixer_encode(uint16_t counter, uint16_t seed) {
    uint8_t s0 = counter & 0xFF;
    uint8_t s1 = (counter >> 8) & 0xFF;
    uint8_t s2 = seed & 0xFF;
    uint8_t s3 = (seed >> 8) & 0xFF;

    int dec_rounds[18];
    int ri = 1;
    for(int i = 0; i < 18; i++) { dec_rounds[i] = ri; ri = (ri - 1) & 0x7; }

    for(int i = 17; i >= 0; i--) {
        uint8_t r_orig = keystore_bytes[dec_rounds[i]] & 0xFF;
        uint8_t r_steps[8];
        r_steps[0] = r_orig;
        for(int k = 1; k < 8; k++)
            r_steps[k] = ((r_steps[k-1] & 0x7F) << 1) & 0xFF;

        for(int step = 7; step >= 0; step--) {
            uint8_t r = r_steps[step];
            uint8_t feedback_bit = s0 & 0x01;
            uint8_t s3b = s3 >> 1;
            uint8_t s2b = (s2 >> 1) | ((s3 & 0x01) << 7);
            uint8_t s1b = (s1 >> 1) | ((s2 & 0x01) << 7);
            uint8_t s0b = (s0 >> 1) | ((s1 & 0x01) << 7);
            uint8_t base, temp, chk;
            if((s3b & 0x40) == 0) base = (s3b & 0x02) == 0 ? 0x74 : 0x2E;
            else                   base = (s3b & 0x02) == 0 ? 0x3A : 0x5C;
            if(s2b & 0x08) base = (((base >> 4) & 0x0F) | ((base & 0x0F) << 4)) & 0xFF;
            if(s1b & 0x01) base = ((base & 0x3F) << 2) & 0xFF;
            if(s0b & 0x01) base = (base << 1) & 0xFF;
            temp = (s3b ^ s1b) & 0xFF;
            chk  = (base ^ (r ^ temp)) & 0xFF;
            if(((chk >> 7) & 1) == feedback_bit) s3 = s3b;
            else                                  s3 = s3b | 0x80;
            s2 = s2b; s1 = s1b; s0 = s0b;
        }
    }
    return ((uint32_t)s3 << 24) | ((uint32_t)s2 << 16) | ((uint32_t)s1 << 8) | s0;
}

struct SubGhzProtocolDecoderKiaV5 {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
    uint16_t header_count;
    ManchesterState manchester_state;
    uint64_t decoded_data;
    uint64_t saved_key;
    uint8_t bit_count;
    uint64_t yek;
    uint8_t crc;
};

struct SubGhzProtocolEncoderKiaV5 {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
};

typedef enum {
    KiaV5DecoderStepReset = 0,
    KiaV5DecoderStepCheckPreamble,
    KiaV5DecoderStepData,
} KiaV5DecoderStep;

const SubGhzProtocolDecoder subghz_protocol_kia_v5_decoder = {
    .alloc = subghz_protocol_decoder_kia_v5_alloc,
    .free = subghz_protocol_decoder_kia_v5_free,
    .feed = subghz_protocol_decoder_kia_v5_feed,
    .reset = subghz_protocol_decoder_kia_v5_reset,
    .get_hash_data = subghz_protocol_decoder_kia_v5_get_hash_data,
    .serialize = subghz_protocol_decoder_kia_v5_serialize,
    .deserialize = subghz_protocol_decoder_kia_v5_deserialize,
    .get_string = subghz_protocol_decoder_kia_v5_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_kia_v5_encoder = {
    .alloc = subghz_protocol_encoder_kia_v5_alloc,
    .free = subghz_protocol_encoder_kia_v5_free,
    .deserialize = subghz_protocol_encoder_kia_v5_deserialize,
    .stop = subghz_protocol_encoder_kia_v5_stop,
    .yield = subghz_protocol_encoder_kia_v5_yield,
};

const SubGhzProtocol subghz_protocol_kia_v5 = {
    .name = SUBGHZ_PROTOCOL_KIA_V5_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send | SubGhzProtocolFlag_Automotive,
    .decoder = &subghz_protocol_kia_v5_decoder,
    .encoder = &subghz_protocol_kia_v5_encoder,
};

void* subghz_protocol_encoder_kia_v5_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderKiaV5* instance = malloc(sizeof(SubGhzProtocolEncoderKiaV5));
    instance->base.protocol = &subghz_protocol_kia_v5;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.size_upload = 400;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.repeat = 10;
    instance->encoder.is_running = false;
    return instance;
}

void subghz_protocol_encoder_kia_v5_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderKiaV5* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void subghz_protocol_encoder_kia_v5_stop(void* context) {
    SubGhzProtocolEncoderKiaV5* instance = context;
    instance->encoder.is_running = false;
}

static bool subghz_protocol_encoder_kia_v5_get_upload(SubGhzProtocolEncoderKiaV5* instance);

LevelDuration subghz_protocol_encoder_kia_v5_yield(void* context) {
    SubGhzProtocolEncoderKiaV5* instance = context;
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

static bool subghz_protocol_encoder_kia_v5_get_upload(SubGhzProtocolEncoderKiaV5* instance) {
    furi_assert(instance);

    uint8_t btn_index = subghz_custom_btn_get() == SUBGHZ_CUSTOM_BTN_OK ?
                      subghz_custom_btn_get_original() :
                      subghz_custom_btn_get();

    uint8_t btn;
    const uint8_t btn_map[5] = {0xFF, 0x02, 0x01, 0x04, 0x08};
    if(btn_index == 0) {
        btn = subghz_custom_btn_get_original();
    } else {
        btn = btn_map[btn_index];
    }

    instance->generic.btn = btn;

    uint64_t yek = 0;
    yek |= ((uint64_t)(btn & 0x0F) << 60);
    yek |= ((uint64_t)(instance->generic.serial & 0x0FFFFFFF) << 32);

    uint16_t seed = ((uint16_t)(btn & 0x0F) << 12) | (instance->generic.serial & 0x0FFF);
    uint32_t encrypted = mixer_encode(instance->generic.cnt, seed);
    yek |= (uint64_t)encrypted;

    uint8_t crc = kia_v5_calculate_crc(yek);
    uint64_t key = bit_reverse_64(yek);

    instance->generic.data = key;
    instance->generic.data_count_bit = 64;

    size_t index = 0;

    for(size_t i = 0; i < 80; i++) {
        instance->encoder.upload[index++] = level_duration_make(true, subghz_protocol_kia_v5_const.te_short);
        instance->encoder.upload[index++] = level_duration_make(false, subghz_protocol_kia_v5_const.te_short);
    }

    instance->encoder.upload[index++] = level_duration_make(true, subghz_protocol_kia_v5_const.te_long);
    instance->encoder.upload[index++] = level_duration_make(false, subghz_protocol_kia_v5_const.te_short);

    bool bits[67];
    for(int i = 0; i < 64; i++) {
        bits[i] = (key >> (63 - i)) & 1;
    }
    for(int i = 0; i < 3; i++) {
        bits[64 + i] = (crc >> (2 - i)) & 1;
    }

    bool prev_bit = true;

    for(int i = 0; i < 67; i++) {
        bool curr_bit = bits[i];
        if(prev_bit && curr_bit) {
            instance->encoder.upload[index++] = level_duration_make(true, subghz_protocol_kia_v5_const.te_short);
            instance->encoder.upload[index++] = level_duration_make(false, subghz_protocol_kia_v5_const.te_short);
        } else if(prev_bit && !curr_bit) {
            instance->encoder.upload[index++] = level_duration_make(true, subghz_protocol_kia_v5_const.te_long);
        } else if(!prev_bit && curr_bit) {
            instance->encoder.upload[index++] = level_duration_make(false, subghz_protocol_kia_v5_const.te_long);
        } else {
            instance->encoder.upload[index++] = level_duration_make(false, subghz_protocol_kia_v5_const.te_short);
            instance->encoder.upload[index++] = level_duration_make(true, subghz_protocol_kia_v5_const.te_short);
        }
        prev_bit = curr_bit;
    }

    instance->encoder.upload[index++] = level_duration_make(prev_bit, subghz_protocol_kia_v5_const.te_short);
    instance->encoder.upload[index++] = level_duration_make(false, subghz_protocol_kia_v5_const.te_long * 10);
    instance->encoder.size_upload = index;

    return true;
}

SubGhzProtocolStatus
    subghz_protocol_encoder_kia_v5_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolEncoderKiaV5* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    static uint32_t call_count = 0;
    call_count++;
    FURI_LOG_I(TAG, "deserialize #%lu, cnt before=%04lX", call_count, (uint32_t)instance->generic.cnt);

    do {
        ret = subghz_block_generic_deserialize(&instance->generic, flipper_format);
        if(ret != SubGhzProtocolStatusOk) break;

        if(instance->generic.data_count_bit < subghz_protocol_kia_v5_const.min_count_bit_for_found) {
            ret = SubGhzProtocolStatusErrorParserBitCount;
            break;
        }

        uint32_t yek_high = 0, yek_low = 0;
        uint64_t yek = 0;
        if(flipper_format_read_uint32(flipper_format, "YekHi", &yek_high, 1) &&
           flipper_format_read_uint32(flipper_format, "YekLo", &yek_low, 1)) {
            yek = ((uint64_t)yek_high << 32) | yek_low;
        } else {
            yek = bit_reverse_64(instance->generic.data);
        }

        instance->generic.serial = (uint32_t)((yek >> 32) & 0x0FFFFFFF);
        instance->generic.btn = (uint8_t)((yek >> 60) & 0x0F);

        uint32_t encrypted = (uint32_t)(yek & 0xFFFFFFFF);
        instance->generic.cnt = mixer_decode(encrypted);

        uint32_t mult = furi_hal_subghz_get_rolling_counter_mult();
	instance->generic.cnt = (instance->generic.cnt + mult) & 0xFFFF;
        FURI_LOG_I(TAG, "deserialize #%lu, cnt after=%04lX", call_count, (uint32_t)instance->generic.cnt);

        if(subghz_custom_btn_get_original() == 0) {
            subghz_custom_btn_set_original(instance->generic.btn);
        }
        subghz_custom_btn_set_max(4);

        if(!subghz_protocol_encoder_kia_v5_get_upload(instance)) {
            ret = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }

        if(!flipper_format_rewind(flipper_format)) {
            ret = SubGhzProtocolStatusErrorParserOthers;
            break;
        }

        uint8_t key_data[sizeof(uint64_t)] = {0};
        for(size_t i = 0; i < sizeof(uint64_t); i++) {
            key_data[sizeof(uint64_t) - i - 1] = (instance->generic.data >> i * 8) & 0xFF;
        }
        if(!flipper_format_update_hex(flipper_format, "Key", key_data, sizeof(uint64_t))) {
            ret = SubGhzProtocolStatusErrorParserKey;
            break;
        }

        uint32_t temp_btn = instance->generic.btn;
        if(!flipper_format_update_uint32(flipper_format, "Btn", &temp_btn, 1)) {
            ret = SubGhzProtocolStatusErrorParserOthers;
            break;
        }

        if(!flipper_format_update_uint32(flipper_format, "Cnt", &instance->generic.cnt, 1)) {
            ret = SubGhzProtocolStatusErrorParserOthers;
            break;
        }

        yek = bit_reverse_64(instance->generic.data);
        yek_high = (uint32_t)(yek >> 32);
        yek_low = (uint32_t)(yek & 0xFFFFFFFF);
        if(!flipper_format_update_uint32(flipper_format, "YekHi", &yek_high, 1)) {
            ret = SubGhzProtocolStatusErrorParserOthers;
            break;
        }
        if(!flipper_format_update_uint32(flipper_format, "YekLo", &yek_low, 1)) {
            ret = SubGhzProtocolStatusErrorParserOthers;
            break;
        }

        uint8_t crc = kia_v5_calculate_crc(yek);
        uint32_t crc_temp = crc;
        if(!flipper_format_update_uint32(flipper_format, "CRC", &crc_temp, 1)) {
            ret = SubGhzProtocolStatusErrorParserOthers;
            break;
        }

        instance->encoder.is_running = true;

    } while(false);

    return ret;
}

void subghz_protocol_encoder_kia_v5_set_button(void* context, uint8_t button) {
    furi_assert(context);
    SubGhzProtocolEncoderKiaV5* instance = context;
    instance->generic.btn = button;
}

void subghz_protocol_encoder_kia_v5_set_counter(void* context, uint16_t counter) {
    furi_assert(context);
    SubGhzProtocolEncoderKiaV5* instance = context;
    instance->generic.cnt = counter;
}

void subghz_protocol_encoder_kia_v5_increment_counter(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderKiaV5* instance = context;
    if(instance->generic.cnt < 0xFFFF) {
        instance->generic.cnt++;
    } else {
        instance->generic.cnt = 0;
    }
}

uint16_t subghz_protocol_encoder_kia_v5_get_counter(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderKiaV5* instance = context;
    return instance->generic.cnt;
}

uint8_t subghz_protocol_encoder_kia_v5_get_button(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderKiaV5* instance = context;
    return instance->generic.btn;
}

static void kia_v5_add_bit(SubGhzProtocolDecoderKiaV5* instance, bool bit) {
    instance->decoded_data = (instance->decoded_data << 1) | (bit ? 1 : 0);
    instance->bit_count++;
}

void* subghz_protocol_decoder_kia_v5_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderKiaV5* instance = malloc(sizeof(SubGhzProtocolDecoderKiaV5));
    instance->base.protocol = &subghz_protocol_kia_v5;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_kia_v5_free(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderKiaV5* instance = context;
    free(instance);
}

void subghz_protocol_decoder_kia_v5_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderKiaV5* instance = context;
    instance->decoder.parser_step = KiaV5DecoderStepReset;
    instance->header_count = 0;
    instance->bit_count = 0;
    instance->decoded_data = 0;
    instance->saved_key = 0;
    instance->yek = 0;
    instance->crc = 0;
    instance->manchester_state = ManchesterStateMid1;
}

void subghz_protocol_decoder_kia_v5_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderKiaV5* instance = context;

    switch(instance->decoder.parser_step) {
    case KiaV5DecoderStepReset:
        if((level) && (DURATION_DIFF(duration, subghz_protocol_kia_v5_const.te_short) <
                       subghz_protocol_kia_v5_const.te_delta)) {
            instance->decoder.parser_step = KiaV5DecoderStepCheckPreamble;
            instance->decoder.te_last = duration;
            instance->header_count = 1;
            instance->bit_count = 0;
            instance->decoded_data = 0;
            manchester_advance(instance->manchester_state, ManchesterEventReset, &instance->manchester_state, NULL);
        }
        break;

    case KiaV5DecoderStepCheckPreamble:
        if(level) {
            if(DURATION_DIFF(duration, subghz_protocol_kia_v5_const.te_long) <
               subghz_protocol_kia_v5_const.te_delta) {
                if(instance->header_count > 40) {
                    instance->decoder.parser_step = KiaV5DecoderStepData;
                    instance->bit_count = 0;
                    instance->decoded_data = 0;
                    instance->saved_key = 0;
                    instance->header_count = 0;
                } else {
                    instance->decoder.te_last = duration;
                }
            } else if(
                DURATION_DIFF(duration, subghz_protocol_kia_v5_const.te_short) <
                subghz_protocol_kia_v5_const.te_delta) {
                instance->decoder.te_last = duration;
            } else {
                instance->decoder.parser_step = KiaV5DecoderStepReset;
            }
        } else {
            if((DURATION_DIFF(duration, subghz_protocol_kia_v5_const.te_short) <
                subghz_protocol_kia_v5_const.te_delta) &&
               (DURATION_DIFF(instance->decoder.te_last, subghz_protocol_kia_v5_const.te_short) <
                subghz_protocol_kia_v5_const.te_delta)) {
                instance->header_count++;
            } else if(
                (DURATION_DIFF(duration, subghz_protocol_kia_v5_const.te_long) <
                 subghz_protocol_kia_v5_const.te_delta) &&
                (DURATION_DIFF(instance->decoder.te_last, subghz_protocol_kia_v5_const.te_short) <
                 subghz_protocol_kia_v5_const.te_delta)) {
                instance->header_count++;
            } else if(
                DURATION_DIFF(instance->decoder.te_last, subghz_protocol_kia_v5_const.te_long) <
                subghz_protocol_kia_v5_const.te_delta) {
                instance->header_count++;
            } else {
                instance->decoder.parser_step = KiaV5DecoderStepReset;
            }
            instance->decoder.te_last = duration;
        }
        break;

    case KiaV5DecoderStepData: {
        ManchesterEvent event;

        if(DURATION_DIFF(duration, subghz_protocol_kia_v5_const.te_short) <
           subghz_protocol_kia_v5_const.te_delta) {
            event = level ? ManchesterEventShortHigh : ManchesterEventShortLow;
        } else if(
            DURATION_DIFF(duration, subghz_protocol_kia_v5_const.te_long) <
            subghz_protocol_kia_v5_const.te_delta) {
            event = level ? ManchesterEventLongHigh : ManchesterEventLongLow;
        } else {
            if(instance->bit_count >= subghz_protocol_kia_v5_const.min_count_bit_for_found) {
                instance->generic.data = instance->saved_key;
                instance->generic.data_count_bit = (instance->bit_count > 67) ? 67 : instance->bit_count;

                instance->crc = (uint8_t)(instance->decoded_data & 0x07);

                instance->yek = 0;
                for(int i = 0; i < 8; i++) {
                    uint8_t byte = (instance->generic.data >> (i * 8)) & 0xFF;
                    uint8_t reversed = 0;
                    for(int b = 0; b < 8; b++) {
                        if(byte & (1 << b))
                            reversed |= (1 << (7 - b));
                    }
                    instance->yek |= ((uint64_t)reversed << ((7 - i) * 8));
                }

                instance->generic.serial = (uint32_t)((instance->yek >> 32) & 0x0FFFFFFF);
                instance->generic.btn = (uint8_t)((instance->yek >> 60) & 0x0F);

                uint32_t encrypted = (uint32_t)(instance->yek & 0xFFFFFFFF);
                instance->generic.cnt = mixer_decode(encrypted);

                instance->decoder.decode_data = instance->generic.data;
                instance->decoder.decode_count_bit = instance->generic.data_count_bit;

                if(subghz_custom_btn_get_original() == 0) {
                    subghz_custom_btn_set_original(instance->generic.btn);
                }
                subghz_custom_btn_set_max(4);

                if(instance->base.callback)
                    instance->base.callback(&instance->base, instance->base.context);
            }

            instance->decoder.parser_step = KiaV5DecoderStepReset;
            break;
        }

        bool data_bit;
        if(instance->bit_count <= 66 &&
           manchester_advance(instance->manchester_state, event, &instance->manchester_state, &data_bit)) {
            kia_v5_add_bit(instance, data_bit);
            if(instance->bit_count == 64) {
                instance->saved_key = instance->decoded_data;
                instance->decoded_data = 0;
            }
        }

        instance->decoder.te_last = duration;
        break;
    }
    }
}

uint8_t subghz_protocol_decoder_kia_v5_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderKiaV5* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_kia_v5_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderKiaV5* instance = context;

    SubGhzProtocolStatus ret = subghz_block_generic_serialize(&instance->generic, flipper_format, preset);

    if(ret == SubGhzProtocolStatusOk) {
        if(!flipper_format_write_uint32(flipper_format, "Serial", &instance->generic.serial, 1)) {
            ret = SubGhzProtocolStatusErrorParserOthers;
        }
    }
    if(ret == SubGhzProtocolStatusOk) {
        uint32_t temp = instance->generic.btn;
        if(!flipper_format_write_uint32(flipper_format, "Btn", &temp, 1)) {
            ret = SubGhzProtocolStatusErrorParserOthers;
        }
    }
    if(ret == SubGhzProtocolStatusOk) {
        if(!flipper_format_write_uint32(flipper_format, "Cnt", &instance->generic.cnt, 1)) {
            ret = SubGhzProtocolStatusErrorParserOthers;
        }
    }
    if(ret == SubGhzProtocolStatusOk) {
        uint32_t crc_temp = instance->crc;
        if(!flipper_format_write_uint32(flipper_format, "CRC", &crc_temp, 1)) {
            ret = SubGhzProtocolStatusErrorParserOthers;
        }
    }
    if(ret == SubGhzProtocolStatusOk) {
        uint32_t raw_high = (uint32_t)(instance->generic.data >> 32);
        uint32_t raw_low = (uint32_t)(instance->generic.data & 0xFFFFFFFF);
        if(!flipper_format_write_uint32(flipper_format, "DataHi", &raw_high, 1)) {
            ret = SubGhzProtocolStatusErrorParserOthers;
        }
        if(ret == SubGhzProtocolStatusOk) {
            if(!flipper_format_write_uint32(flipper_format, "DataLo", &raw_low, 1)) {
                ret = SubGhzProtocolStatusErrorParserOthers;
            }
        }
    }
    if(ret == SubGhzProtocolStatusOk) {
        uint32_t yek_high = (uint32_t)(instance->yek >> 32);
        uint32_t yek_low = (uint32_t)(instance->yek & 0xFFFFFFFF);
        if(!flipper_format_write_uint32(flipper_format, "YekHi", &yek_high, 1)) {
            ret = SubGhzProtocolStatusErrorParserOthers;
        }
        if(ret == SubGhzProtocolStatusOk) {
            if(!flipper_format_write_uint32(flipper_format, "YekLo", &yek_low, 1)) {
                ret = SubGhzProtocolStatusErrorParserOthers;
            }
        }
    }

    return ret;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_kia_v5_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderKiaV5* instance = context;

    SubGhzProtocolStatus ret = subghz_block_generic_deserialize(&instance->generic, flipper_format);

    if(ret == SubGhzProtocolStatusOk) {
        if(instance->generic.data_count_bit < subghz_protocol_kia_v5_const.min_count_bit_for_found) {
            ret = SubGhzProtocolStatusErrorParserBitCount;
        }
    }

    if(ret == SubGhzProtocolStatusOk) {
        uint32_t temp_crc = 0;
        if(flipper_format_read_uint32(flipper_format, "CRC", &temp_crc, 1)) {
            instance->crc = temp_crc;
        }

        uint32_t yek_high = 0, yek_low = 0;
        if(flipper_format_read_uint32(flipper_format, "YekHi", &yek_high, 1) &&
           flipper_format_read_uint32(flipper_format, "YekLo", &yek_low, 1)) {
            instance->yek = ((uint64_t)yek_high << 32) | yek_low;
        } else {
            instance->yek = 0;
            for(int i = 0; i < 8; i++) {
                uint8_t byte = (instance->generic.data >> (i * 8)) & 0xFF;
                uint8_t reversed = 0;
                for(int j = 0; j < 8; j++) {
                    if(byte & (1 << j)) {
                        reversed |= (1 << (7 - j));
                    }
                }
                instance->yek |= ((uint64_t)reversed << ((7 - i) * 8));
            }
        }

        instance->generic.serial = (uint32_t)((instance->yek >> 32) & 0x0FFFFFFF);
        instance->generic.btn = (uint8_t)((instance->yek >> 60) & 0x0F);

        uint32_t encrypted = (uint32_t)(instance->yek & 0xFFFFFFFF);
        instance->generic.cnt = mixer_decode(encrypted);

        if(subghz_custom_btn_get_original() == 0) {
            subghz_custom_btn_set_original(instance->generic.btn);
        }
        subghz_custom_btn_set_max(4);
    }

    return ret;
}

static const char* subghz_protocol_kia_v5_get_name_button(uint8_t btn) {
    switch(btn) {
        case 0x01: return "Unlock";
        case 0x02: return "Lock";
        case 0x04: return "Trunk";
        case 0x08: return "Horn";
        default: return "Unknown";
    }
}

void subghz_protocol_decoder_kia_v5_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderKiaV5* instance = context;

    uint32_t code_found_hi = instance->generic.data >> 32;
    uint32_t code_found_lo = instance->generic.data & 0xFFFFFFFF;
    uint32_t yek_hi = (uint32_t)(instance->yek >> 32);
    uint32_t yek_lo = (uint32_t)(instance->yek & 0xFFFFFFFF);

    uint8_t calculated_crc = kia_v5_calculate_crc(instance->yek);
    bool crc_valid = (instance->crc == calculated_crc);

    uint16_t seed = ((uint16_t)(instance->generic.btn & 0x0F) << 12) |
                    (instance->generic.serial & 0x0FFF);

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Key:%08lX%08lX\r\n"
        "Yek:%08lX%08lX\r\n"
        "Sn:%07lX Cnt:%04lX\r\n"
        "Btn:%02X:[%s] Seed:%04X\r\n"
        "CRC:%X %s",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        code_found_hi,
        code_found_lo,
        yek_hi,
        yek_lo,
        instance->generic.serial,
        instance->generic.cnt,
        instance->generic.btn,
        subghz_protocol_kia_v5_get_name_button(instance->generic.btn),
        seed,
        instance->crc,
        crc_valid ? "(OK)" : "(FAIL)");
}
