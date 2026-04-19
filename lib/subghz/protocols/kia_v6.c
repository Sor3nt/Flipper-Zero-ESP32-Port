#include "kia_v6.h"
#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"
#include "../blocks/custom_btn_i.h"
#include <lib/toolbox/manchester_decoder.h>
#include <flipper_format/flipper_format.h>
#include <furi_hal_crypto.h>

#define TAG "SubGhzProtocolKiaV6"

#define KIA_V6_XOR_MASK_LOW  0x84AF25FB
#define KIA_V6_XOR_MASK_HIGH 0x638766AB

#define KIA_V6_PREAMBLE_PAIRS_1  640
#define KIA_V6_PREAMBLE_PAIRS_2  38
#define KIA_V6_UPLOAD_SIZE       2000

static const SubGhzBlockConst subghz_protocol_kia_v6_const = {
    .te_short = 200,
    .te_long = 400,
    .te_delta = 100,
    .min_count_bit_for_found = 144,
};

static const uint8_t aes_sbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16
};

struct SubGhzProtocolDecoderKiaV6 {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
    uint16_t header_count;
    ManchesterState manchester_state;

    uint32_t data_part1_low;
    uint32_t data_part1_high;
    uint32_t stored_part1_low;
    uint32_t stored_part1_high;
    uint32_t stored_part2_low;
    uint32_t stored_part2_high;
    uint16_t data_part3;

    uint8_t bit_count;

    uint32_t saved_part1_low;
    uint32_t saved_part1_high;
    uint32_t saved_part2_low;
    uint32_t saved_part2_high;
    uint16_t saved_part3;

    uint32_t saved_serial;
    uint8_t saved_btn;
    uint32_t saved_cnt;
    uint8_t saved_fx;
    uint8_t saved_crc1;
    uint8_t saved_crc2;
    bool saved_crc_valid;

    uint8_t fx_field;
    uint8_t crc1_field;
    uint8_t crc2_field;
};

struct SubGhzProtocolEncoderKiaV6 {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
    uint32_t stored_part1_low;
    uint32_t stored_part1_high;
    uint32_t stored_part2_low;
    uint32_t stored_part2_high;
    uint16_t data_part3;
    uint8_t fx_field;
};

typedef enum {
    KiaV6DecoderStepReset = 0,
    KiaV6DecoderStepWaitFirstHigh,
    KiaV6DecoderStepCountPreamble,
    KiaV6DecoderStepWaitLongHigh,
    KiaV6DecoderStepData,
} KiaV6DecoderStep;

static uint8_t kia_v6_crc8(uint8_t *data, int len, uint8_t init, uint8_t polynomial) {
    uint8_t crc = init;
    uint8_t *pbVar3 = data;
    while (pbVar3 != data + len) {
        crc = crc ^ *pbVar3;
        for (int j = 8; j > 0; j--) {
            uint8_t bVar1 = (uint8_t)(crc << 1);
            if ((crc & 0x80) != 0) {
                bVar1 = bVar1 ^ polynomial;
            }
            crc = bVar1;
        }
        pbVar3++;
    }
    return crc;
}

static uint8_t kia_v6_btn_to_custom(uint8_t btn) {
    switch(btn) {
    case 0x01: return 1; // Lock
    case 0x02: return 2; // Unlock
    case 0x03: return 3; // Trunk
    case 0x04: return 4; // Panic
    default:   return 2; // Unknown → Unlock
    }
}

static uint8_t kia_v6_custom_to_btn(uint8_t custom) {
    switch(custom) {
    case 1: return 0x01; // Lock
    case 2: return 0x02; // Unlock
    case 3: return 0x03; // Trunk
    case 4: return 0x04; // Panic
    default: return 0x02;
    }
}

static void get_kia_v6_aes_key(uint8_t* aes_key) {
    uint64_t keystore_a = 0x37CE21F8C9F862A8ULL ^ 0x5448455049524154ULL;
    uint32_t keystore_a_hi = (keystore_a >> 32) & 0xFFFFFFFF;
    uint32_t keystore_a_lo = keystore_a & 0xFFFFFFFF;
    uint32_t uVar15_a = keystore_a_lo ^ KIA_V6_XOR_MASK_LOW;
    uint32_t uVar5_a  = KIA_V6_XOR_MASK_HIGH ^ keystore_a_hi;
    uint64_t val64_a  = ((uint64_t)uVar5_a << 32) | uVar15_a;
    for (int i = 0; i < 8; i++) {
        aes_key[i] = (val64_a >> (56 - i * 8)) & 0xFF;
    }
    uint64_t keystore_b = 0x3FC629F0C1F06AA0ULL ^ 0x5448455049524154ULL;
    uint32_t keystore_b_hi = (keystore_b >> 32) & 0xFFFFFFFF;
    uint32_t keystore_b_lo = keystore_b & 0xFFFFFFFF;
    uint32_t uVar15_b = keystore_b_lo ^ KIA_V6_XOR_MASK_LOW;
    uint32_t uVar5_b  = KIA_V6_XOR_MASK_HIGH ^ keystore_b_hi;
    uint64_t val64_b  = ((uint64_t)uVar5_b << 32) | uVar15_b;
    for (int i = 0; i < 8; i++) {
        aes_key[i + 8] = (val64_b >> (56 - i * 8)) & 0xFF;
    }
}

static bool kia_v6_decrypt(SubGhzProtocolDecoderKiaV6* instance) {
    uint8_t encrypted_data[16];

    encrypted_data[0]  = (instance->stored_part1_high >> 8) & 0xFF;
    encrypted_data[1]  =  instance->stored_part1_high & 0xFF;
    encrypted_data[2]  = (instance->stored_part1_low >> 24) & 0xFF;
    encrypted_data[3]  = (instance->stored_part1_low >> 16) & 0xFF;
    encrypted_data[4]  = (instance->stored_part1_low >> 8) & 0xFF;
    encrypted_data[5]  =  instance->stored_part1_low & 0xFF;
    encrypted_data[6]  = (instance->stored_part2_high >> 24) & 0xFF;
    encrypted_data[7]  = (instance->stored_part2_high >> 16) & 0xFF;
    encrypted_data[8]  = (instance->stored_part2_high >> 8) & 0xFF;
    encrypted_data[9]  =  instance->stored_part2_high & 0xFF;
    encrypted_data[10] = (instance->stored_part2_low >> 24) & 0xFF;
    encrypted_data[11] = (instance->stored_part2_low >> 16) & 0xFF;
    encrypted_data[12] = (instance->stored_part2_low >> 8) & 0xFF;
    encrypted_data[13] =  instance->stored_part2_low & 0xFF;
    encrypted_data[14] = (instance->data_part3 >> 8) & 0xFF;
    encrypted_data[15] =  instance->data_part3 & 0xFF;

    uint8_t fx_byte0 = (instance->stored_part1_high >> 24) & 0xFF;
    uint8_t fx_byte1 = (instance->stored_part1_high >> 16) & 0xFF;
    instance->fx_field = ((fx_byte0 & 0xF) << 4) | (fx_byte1 & 0xF);

    uint8_t aes_key[16];
    get_kia_v6_aes_key(aes_key);
    uint8_t decrypted_buf[16];
    furi_hal_crypto_aes128_ecb_decrypt(aes_key, encrypted_data, decrypted_buf);
    memcpy(encrypted_data, decrypted_buf, 16);

    uint8_t *decrypted = encrypted_data;
    uint8_t calculated_crc = kia_v6_crc8(decrypted, 15, 0xFF, 0x07);
    uint8_t stored_crc = decrypted[15];

    instance->generic.serial = ((uint32_t)decrypted[4] << 16) |
                               ((uint32_t)decrypted[5] << 8)  |
                               decrypted[6];
    instance->generic.btn = decrypted[7];
    instance->generic.cnt = ((uint32_t)decrypted[8]  << 24) |
                            ((uint32_t)decrypted[9]  << 16) |
                            ((uint32_t)decrypted[10] << 8)  |
                            decrypted[11];
    instance->crc1_field = decrypted[12];
    instance->crc2_field = decrypted[15];

    bool crc_valid = (calculated_crc ^ stored_crc) < 2;

    instance->saved_serial    = instance->generic.serial;
    instance->saved_btn       = instance->generic.btn;
    instance->saved_cnt       = instance->generic.cnt;
    instance->saved_fx        = instance->fx_field;
    instance->saved_crc1      = instance->crc1_field;
    instance->saved_crc2      = instance->crc2_field;
    instance->saved_crc_valid = crc_valid;
    instance->saved_part1_low  = instance->stored_part1_low;
    instance->saved_part1_high = instance->stored_part1_high;
    instance->saved_part2_low  = instance->stored_part2_low;
    instance->saved_part2_high = instance->stored_part2_high;
    instance->saved_part3      = instance->data_part3;

    return crc_valid;
}

static void kia_v6_encrypt_payload(
    uint8_t fx_field,
    uint32_t serial,
    uint8_t btn,
    uint32_t cnt,
    uint32_t* out_part1_low,
    uint32_t* out_part1_high,
    uint32_t* out_part2_low,
    uint32_t* out_part2_high,
    uint16_t* out_part3) {
    uint8_t plain[16];
    memset(plain, 0, 16);
    plain[0]  = fx_field;
    plain[4]  = (serial >> 16) & 0xFF;
    plain[5]  = (serial >> 8) & 0xFF;
    plain[6]  =  serial & 0xFF;
    plain[7]  =  btn & 0x0F;
    plain[8]  = (cnt >> 24) & 0xFF;
    plain[9]  = (cnt >> 16) & 0xFF;
    plain[10] = (cnt >> 8) & 0xFF;
    plain[11] =  cnt & 0xFF;
    plain[12] = aes_sbox[cnt & 0xFF];
    plain[15] = kia_v6_crc8(plain, 15, 0xFF, 0x07);

    uint8_t aes_key[16];
    get_kia_v6_aes_key(aes_key);
    uint8_t encrypted[16];
    furi_hal_crypto_aes128_ecb_encrypt(aes_key, plain, encrypted);
    memcpy(plain, encrypted, 16);

    uint8_t fx_hi = 0x20 | (fx_field >> 4);
    uint8_t fx_lo = fx_field & 0x0F;
    *out_part1_high = ((uint32_t)fx_hi   << 24) | ((uint32_t)fx_lo   << 16) |
                      ((uint32_t)plain[0] << 8)  |  plain[1];
    *out_part1_low  = ((uint32_t)plain[2] << 24) | ((uint32_t)plain[3] << 16) |
                      ((uint32_t)plain[4] << 8)  |  plain[5];
    *out_part2_high = ((uint32_t)plain[6] << 24) | ((uint32_t)plain[7] << 16) |
                      ((uint32_t)plain[8] << 8)  |  plain[9];
    *out_part2_low  = ((uint32_t)plain[10] << 24) | ((uint32_t)plain[11] << 16) |
                      ((uint32_t)plain[12] << 8)  |  plain[13];
    *out_part3 = ((uint16_t)plain[14] << 8) | plain[15];
}

void* subghz_protocol_decoder_kia_v6_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderKiaV6* instance = malloc(sizeof(SubGhzProtocolDecoderKiaV6));
    instance->base.protocol = &subghz_protocol_kia_v6;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_kia_v6_free(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderKiaV6* instance = context;
    free(instance);
}

void subghz_protocol_decoder_kia_v6_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderKiaV6* instance = context;
    instance->decoder.parser_step = KiaV6DecoderStepReset;
    instance->header_count = 0;
    instance->bit_count = 0;
    instance->data_part1_low = 0;
    instance->data_part1_high = 0;
    manchester_advance(instance->manchester_state, ManchesterEventReset, &instance->manchester_state, NULL);
}

void subghz_protocol_decoder_kia_v6_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderKiaV6* instance = context;

    switch(instance->decoder.parser_step) {
    case KiaV6DecoderStepReset:
        if(level == 0) return;
        if(DURATION_DIFF(duration, subghz_protocol_kia_v6_const.te_short) < subghz_protocol_kia_v6_const.te_delta) {
            instance->decoder.parser_step = KiaV6DecoderStepWaitFirstHigh;
            instance->decoder.te_last = duration;
            instance->header_count = 0;
            manchester_advance(instance->manchester_state, ManchesterEventReset, &instance->manchester_state, NULL);
        }
        return;

    case KiaV6DecoderStepWaitFirstHigh:
        if(level != 0) return;
        uint32_t diff_short = DURATION_DIFF(duration, subghz_protocol_kia_v6_const.te_short);
        uint32_t diff_long  = DURATION_DIFF(duration, subghz_protocol_kia_v6_const.te_long);
        uint32_t diff = (diff_long < diff_short) ? diff_long : diff_short;

        if(diff_long < subghz_protocol_kia_v6_const.te_delta && diff_long < diff_short) {
            if(instance->header_count >= 0x259) {
                instance->header_count = 0;
                instance->decoder.te_last = duration;
                instance->decoder.parser_step = KiaV6DecoderStepWaitLongHigh;
                return;
            }
        }
        if(diff >= subghz_protocol_kia_v6_const.te_delta) {
            instance->decoder.parser_step = KiaV6DecoderStepReset;
            return;
        }
        if(DURATION_DIFF(instance->decoder.te_last, subghz_protocol_kia_v6_const.te_short) < subghz_protocol_kia_v6_const.te_delta) {
            instance->decoder.te_last = duration;
            instance->header_count++;
            return;
        } else {
            instance->decoder.parser_step = KiaV6DecoderStepReset;
            return;
        }

    case KiaV6DecoderStepWaitLongHigh:
        if(level == 0) {
            instance->decoder.parser_step = KiaV6DecoderStepReset;
            return;
        }
        uint32_t diff_long_check  = DURATION_DIFF(duration, subghz_protocol_kia_v6_const.te_long);
        uint32_t diff_short_check = DURATION_DIFF(duration, subghz_protocol_kia_v6_const.te_short);
        if(diff_long_check >= subghz_protocol_kia_v6_const.te_delta) {
            if(diff_short_check >= subghz_protocol_kia_v6_const.te_delta) {
                instance->decoder.parser_step = KiaV6DecoderStepReset;
                return;
            }
        }
        if(DURATION_DIFF(instance->decoder.te_last, subghz_protocol_kia_v6_const.te_long) >= subghz_protocol_kia_v6_const.te_delta) {
            instance->decoder.parser_step = KiaV6DecoderStepReset;
            return;
        }
        instance->decoder.decode_data = 0;
        instance->decoder.decode_count_bit = 0;
        subghz_protocol_blocks_add_bit(&instance->decoder, 1);
        subghz_protocol_blocks_add_bit(&instance->decoder, 1);
        subghz_protocol_blocks_add_bit(&instance->decoder, 0);
        subghz_protocol_blocks_add_bit(&instance->decoder, 1);
        instance->data_part1_low  = (uint32_t)(instance->decoder.decode_data & 0xFFFFFFFF);
        instance->data_part1_high = (uint32_t)((instance->decoder.decode_data >> 32) & 0xFFFFFFFF);
        instance->bit_count = instance->decoder.decode_count_bit;
        instance->decoder.parser_step = KiaV6DecoderStepData;
        return;

    case KiaV6DecoderStepData: {
        ManchesterEvent event;
        bool data_bit;

        if(DURATION_DIFF(duration, subghz_protocol_kia_v6_const.te_short) < subghz_protocol_kia_v6_const.te_delta) {
            event = (level & 0x7F) << 1;
        } else if(DURATION_DIFF(duration, subghz_protocol_kia_v6_const.te_long) < subghz_protocol_kia_v6_const.te_delta) {
            event = level ? ManchesterEventLongHigh : ManchesterEventLongLow;
        } else {
            instance->decoder.parser_step = KiaV6DecoderStepReset;
            return;
        }

        if(manchester_advance(instance->manchester_state, event, &instance->manchester_state, &data_bit)) {
            uint32_t uVar4 = instance->data_part1_low;
            uint32_t uVar5 = (uVar4 << 1) | (data_bit ? 1 : 0);
            uint32_t carry = (uVar4 >> 31) & 1;
            uVar4 = (instance->data_part1_high << 1) | carry;
            instance->data_part1_low  = uVar5;
            instance->data_part1_high = uVar4;
            instance->decoder.decode_data = ((uint64_t)uVar4 << 32) | uVar5;
            instance->bit_count++;

            if(instance->bit_count == 0x40) {
                instance->stored_part1_low  = ~uVar5;
                instance->stored_part1_high = ~uVar4;
                instance->data_part1_low  = 0;
                instance->data_part1_high = 0;
            } else if(instance->bit_count == 0x80) {
                instance->stored_part2_low  = ~uVar5;
                instance->stored_part2_high = ~uVar4;
                instance->data_part1_low  = 0;
                instance->data_part1_high = 0;
            }
        }

        instance->decoder.te_last = duration;

        if(instance->bit_count != subghz_protocol_kia_v6_const.min_count_bit_for_found) {
            return;
        }

        instance->generic.data_count_bit = subghz_protocol_kia_v6_const.min_count_bit_for_found;
        instance->data_part3 = ~((uint16_t)instance->data_part1_low);
        instance->generic.data = ((uint64_t)instance->stored_part1_high << 32) | instance->stored_part1_low;

        kia_v6_decrypt(instance);

        if(instance->base.callback) {
            instance->base.callback(&instance->base, instance->base.context);
        }

        instance->data_part1_low  = 0;
        instance->data_part1_high = 0;
        instance->bit_count = 0;
        instance->decoder.parser_step = KiaV6DecoderStepReset;
        return;
    }

    default:
        return;
    }
}

uint8_t subghz_protocol_decoder_kia_v6_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderKiaV6* instance = context;
    uint8_t hash = 0;
    hash ^= (instance->stored_part1_low  >> 24) & 0xFF;
    hash ^= (instance->stored_part1_low  >> 16) & 0xFF;
    hash ^= (instance->stored_part1_low  >>  8) & 0xFF;
    hash ^=  instance->stored_part1_low         & 0xFF;
    hash ^= (instance->stored_part1_high >> 24) & 0xFF;
    hash ^= (instance->stored_part1_high >> 16) & 0xFF;
    hash ^= (instance->stored_part1_high >>  8) & 0xFF;
    hash ^=  instance->stored_part1_high        & 0xFF;
    return hash;
}

SubGhzProtocolStatus subghz_protocol_decoder_kia_v6_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderKiaV6* instance = context;

    SubGhzProtocolStatus ret = subghz_block_generic_serialize(&instance->generic, flipper_format, preset);

    if(ret == SubGhzProtocolStatusOk) {
        uint32_t key2_low = instance->stored_part2_low;
        if(!flipper_format_write_uint32(flipper_format, "Key_2", &key2_low, 1)) {
            ret = SubGhzProtocolStatusErrorParserOthers;
        }
    }
    if(ret == SubGhzProtocolStatusOk) {
        uint32_t key2_high = instance->stored_part2_high;
        if(!flipper_format_write_uint32(flipper_format, "Key_3", &key2_high, 1)) {
            ret = SubGhzProtocolStatusErrorParserOthers;
        }
    }
    if(ret == SubGhzProtocolStatusOk) {
        uint32_t key3 = instance->data_part3;
        if(!flipper_format_write_uint32(flipper_format, "Key_4", &key3, 1)) {
            ret = SubGhzProtocolStatusErrorParserOthers;
        }
    }
    if(ret == SubGhzProtocolStatusOk) {
        uint32_t fx = instance->fx_field;
        if(!flipper_format_write_uint32(flipper_format, "Fx", &fx, 1)) {
            ret = SubGhzProtocolStatusErrorParserOthers;
        }
    }
    return ret;
}

SubGhzProtocolStatus subghz_protocol_decoder_kia_v6_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderKiaV6* instance = context;

    SubGhzProtocolStatus ret = subghz_block_generic_deserialize(&instance->generic, flipper_format);
    if(ret == SubGhzProtocolStatusOk) {
        if(instance->generic.data_count_bit < subghz_protocol_kia_v6_const.min_count_bit_for_found) {
            ret = SubGhzProtocolStatusErrorParserBitCount;
        }
    }
    if(ret == SubGhzProtocolStatusOk) {
        uint32_t temp;
        if(flipper_format_read_uint32(flipper_format, "Key_2", &temp, 1)) {
            instance->stored_part2_low = temp;
        }
        if(flipper_format_read_uint32(flipper_format, "Key_3", &temp, 1)) {
            instance->stored_part2_high = temp;
        }
        if(flipper_format_read_uint32(flipper_format, "Key_4", &temp, 1)) {
            instance->data_part3 = (uint16_t)temp;
        }
        if(flipper_format_read_uint32(flipper_format, "Fx", &temp, 1)) {
            instance->fx_field = (uint8_t)temp;
        }
        instance->stored_part1_high = (uint32_t)((instance->generic.data >> 32) & 0xFFFFFFFF);
        instance->stored_part1_low  = (uint32_t)(instance->generic.data & 0xFFFFFFFF);
        kia_v6_decrypt(instance);
    }
    return ret;
}

void subghz_protocol_decoder_kia_v6_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderKiaV6* instance = context;

    uint32_t key1_hi = instance->saved_part1_high;
    uint32_t key1_lo = instance->saved_part1_low;
    uint32_t key2_hi = instance->saved_part2_high;
    uint32_t key2_lo = instance->saved_part2_low;

    uint32_t key2_uVar4    = key2_hi << 16;
    uint32_t key2_uVar2    = key2_lo >> 16;
    uint32_t key2_uVar1    = key2_hi >> 16;
    uint32_t key2_combined = key2_uVar4 | key2_uVar2;
    uint32_t key2_uVar3    = key2_lo << 16;
    uint32_t key2_second   = (instance->saved_part3 & 0xFFFF) | key2_uVar3;
    uint32_t serial_6      = instance->saved_serial & 0xFFFFFF;

    const char* crc_status = instance->saved_crc_valid ? "(OK)" : "(FAIL)";

    const char* btn_name;
    switch(instance->saved_btn & 0x0F) {
    case 0x01: btn_name = "Lock";    break;
    case 0x02: btn_name = "Unlock";  break;
    case 0x03: btn_name = "Trunk";   break;
    case 0x04: btn_name = "Panic";   break;
    default:   btn_name = "Unknown"; break;
    }

    furi_string_printf(
        output,
        "%s %dbit\r\n"
        "%08lX%08lX%04lX\r\n"
        "%08lX%08lX Fx:%02X\r\n"
        "Ser:%06lX Btn:%01X[%s]\r\n"
        "Cnt:%08lX CRC:%02X-%02X\r\n"
        "CRC %s",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        key1_hi,
        key1_lo,
        key2_uVar1,
        key2_combined,
        key2_second,
        instance->saved_fx,
        serial_6,
        instance->saved_btn & 0x0F,
        btn_name,
        instance->saved_cnt,
        instance->saved_crc1,
        instance->saved_crc2,
        crc_status);
}

static inline void kia_v6_encode_manchester_bit(
    LevelDuration* upload,
    size_t* index,
    bool bit,
    uint32_t te) {
    if(bit) {
        upload[(*index)++] = level_duration_make(false, te);
        upload[(*index)++] = level_duration_make(true, te);
    } else {
        upload[(*index)++] = level_duration_make(true, te);
        upload[(*index)++] = level_duration_make(false, te);
    }
}

static void kia_v6_encode_message(
    LevelDuration* upload,
    size_t* index,
    int preamble_pairs,
    uint32_t p1_lo,
    uint32_t p1_hi,
    uint32_t p2_lo,
    uint32_t p2_hi,
    uint16_t p3) {
    const uint32_t te_short = subghz_protocol_kia_v6_const.te_short;

    for(int i = 0; i < preamble_pairs; i++) {
        upload[(*index)++] = level_duration_make(true, te_short);
        upload[(*index)++] = level_duration_make(false, te_short);
    }
    upload[(*index)++] = level_duration_make(false, te_short);
    upload[(*index)++] = level_duration_make(true, subghz_protocol_kia_v6_const.te_long);
    upload[(*index)++] = level_duration_make(false, te_short);

    for(int b = 60; b >= 0; b--) {
        uint32_t word  = (b >= 32) ? p1_hi : p1_lo;
        int shift      = (b >= 32) ? (b - 32) : b;
        kia_v6_encode_manchester_bit(upload, index, ((~word) >> shift) & 1, te_short);
    }
    for(int b = 63; b >= 0; b--) {
        uint32_t word  = (b >= 32) ? p2_hi : p2_lo;
        int shift      = (b >= 32) ? (b - 32) : b;
        kia_v6_encode_manchester_bit(upload, index, ((~word) >> shift) & 1, te_short);
    }
    for(int b = 15; b >= 0; b--) {
        kia_v6_encode_manchester_bit(upload, index, ((~p3) >> b) & 1, te_short);
    }
}

// FIX: build_upload usa direttamente instance->generic.btn, già risolto dal chiamante
static void kia_v6_encoder_build_upload(SubGhzProtocolEncoderKiaV6* instance) {
    furi_assert(instance);

    kia_v6_encrypt_payload(
        instance->fx_field,
        instance->generic.serial,
        instance->generic.btn & 0x0F,
        instance->generic.cnt,
        &instance->stored_part1_low,
        &instance->stored_part1_high,
        &instance->stored_part2_low,
        &instance->stored_part2_high,
        &instance->data_part3);

    uint32_t p1_lo = instance->stored_part1_low;
    uint32_t p1_hi = instance->stored_part1_high;
    uint32_t p2_lo = instance->stored_part2_low;
    uint32_t p2_hi = instance->stored_part2_high;
    uint16_t p3    = instance->data_part3;

    size_t index = 0;
    kia_v6_encode_message(instance->encoder.upload, &index, KIA_V6_PREAMBLE_PAIRS_1, p1_lo, p1_hi, p2_lo, p2_hi, p3);
    instance->encoder.upload[index++] = level_duration_make(false, subghz_protocol_kia_v6_const.te_long);
    kia_v6_encode_message(instance->encoder.upload, &index, KIA_V6_PREAMBLE_PAIRS_2, p1_lo, p1_hi, p2_lo, p2_hi, p3);
    instance->encoder.upload[index++] = level_duration_make(false, subghz_protocol_kia_v6_const.te_long);

    instance->encoder.size_upload = index;
    instance->encoder.front = 0;
}

void* subghz_protocol_encoder_kia_v6_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderKiaV6* instance = malloc(sizeof(SubGhzProtocolEncoderKiaV6));
    if(!instance) return NULL;
    memset(instance, 0, sizeof(SubGhzProtocolEncoderKiaV6));
    instance->base.protocol = &subghz_protocol_kia_v6;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.size_upload = KIA_V6_UPLOAD_SIZE;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    if(!instance->encoder.upload) {
        free(instance);
        return NULL;
    }
    instance->encoder.repeat = 3;
    instance->encoder.front = 0;
    instance->encoder.is_running = false;
    subghz_custom_btn_set_max(4);
    return instance;
}

void subghz_protocol_encoder_kia_v6_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderKiaV6* instance = context;
    if(instance->encoder.upload) {
        free(instance->encoder.upload);
    }
    free(instance);
}

SubGhzProtocolStatus subghz_protocol_encoder_kia_v6_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolEncoderKiaV6* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    instance->encoder.is_running = false;
    instance->encoder.front = 0;
    instance->encoder.repeat = 3;

    do {
        SubGhzProtocolDecoderKiaV6 dec = {0};

        ret = subghz_block_generic_deserialize(&dec.generic, flipper_format);
        if(ret != SubGhzProtocolStatusOk) break;
        if(dec.generic.data_count_bit < subghz_protocol_kia_v6_const.min_count_bit_for_found) {
            ret = SubGhzProtocolStatusErrorParserBitCount;
            break;
        }

        uint32_t temp;
        if(flipper_format_read_uint32(flipper_format, "Key_2", &temp, 1)) {
            dec.stored_part2_low = temp;
        }
        if(flipper_format_read_uint32(flipper_format, "Key_3", &temp, 1)) {
            dec.stored_part2_high = temp;
        }
        if(flipper_format_read_uint32(flipper_format, "Key_4", &temp, 1)) {
            dec.data_part3 = (uint16_t)temp;
        }
        if(flipper_format_read_uint32(flipper_format, "Fx", &temp, 1)) {
            dec.fx_field = (uint8_t)temp;
        }

        dec.stored_part1_high = (uint32_t)((dec.generic.data >> 32) & 0xFFFFFFFF);
        dec.stored_part1_low  = (uint32_t)(dec.generic.data & 0xFFFFFFFF);

        kia_v6_decrypt(&dec);

        instance->generic.serial         = dec.generic.serial;
        instance->generic.btn            = dec.generic.btn;
        instance->generic.cnt            = dec.generic.cnt;
        instance->generic.data_count_bit = subghz_protocol_kia_v6_const.min_count_bit_for_found;
        instance->fx_field               = dec.fx_field;

        // Salva il btn originale come indice custom 1-4 (identico a Suzuki)
        if(subghz_custom_btn_get_original() == 0) {
            subghz_custom_btn_set_original(kia_v6_btn_to_custom(instance->generic.btn));
        }
        subghz_custom_btn_set_max(4);

        // Incrementa cnt
        if(instance->generic.cnt < 0xFFFFFFFF) {
            if((instance->generic.cnt + furi_hal_subghz_get_rolling_counter_mult()) > 0xFFFFFFFF) {
                instance->generic.cnt = 0;
            } else {
                instance->generic.cnt += furi_hal_subghz_get_rolling_counter_mult();
            }
        } else {
            instance->generic.cnt = 0;
        }

        // Risolvi btn dal tasto premuto (identico a Suzuki)
        instance->generic.btn = kia_v6_custom_to_btn(
            subghz_custom_btn_get() == SUBGHZ_CUSTOM_BTN_OK
                ? subghz_custom_btn_get_original()
                : subghz_custom_btn_get());

        kia_v6_encoder_build_upload(instance);

        // Aggiorna il file .sub con i nuovi valori cifrati
        uint32_t file_part1_low, file_part1_high, file_part2_low, file_part2_high;
        uint16_t file_part3;
        kia_v6_encrypt_payload(
            instance->fx_field,
            instance->generic.serial,
            instance->generic.btn & 0x0F,
            instance->generic.cnt,
            &file_part1_low,
            &file_part1_high,
            &file_part2_low,
            &file_part2_high,
            &file_part3);

        uint64_t new_key = ((uint64_t)file_part1_high << 32) | file_part1_low;
        uint8_t key_data[sizeof(uint64_t)];
        for(size_t i = 0; i < sizeof(uint64_t); i++) {
            key_data[sizeof(uint64_t) - i - 1] = (new_key >> (i * 8)) & 0xFF;
        }
        if(!flipper_format_rewind(flipper_format)) break;
        if(!flipper_format_update_hex(flipper_format, "Key", key_data, sizeof(uint64_t))) {
            ret = SubGhzProtocolStatusErrorParserKey;
            break;
        }

        flipper_format_update_uint32(flipper_format, "Key_2", &file_part2_low, 1);
        flipper_format_update_uint32(flipper_format, "Key_3", &file_part2_high, 1);
        uint32_t file_key3 = file_part3;
        flipper_format_update_uint32(flipper_format, "Key_4", &file_key3, 1);

        instance->encoder.is_running = true;
        ret = SubGhzProtocolStatusOk;
    } while(false);

    return ret;
}

void subghz_protocol_encoder_kia_v6_stop(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderKiaV6* instance = context;
    instance->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_kia_v6_yield(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderKiaV6* instance = context;

    if(!instance->encoder.is_running || instance->encoder.repeat <= 0) {
        instance->encoder.is_running = false;
        return level_duration_reset();
    }

    LevelDuration ret = instance->encoder.upload[instance->encoder.front];
    instance->encoder.front++;
    if(instance->encoder.front >= instance->encoder.size_upload) {
        instance->encoder.repeat--;
        instance->encoder.front = 0;
    }
    return ret;
}

const SubGhzProtocolDecoder subghz_protocol_kia_v6_decoder = {
    .alloc          = subghz_protocol_decoder_kia_v6_alloc,
    .free           = subghz_protocol_decoder_kia_v6_free,
    .feed           = subghz_protocol_decoder_kia_v6_feed,
    .reset          = subghz_protocol_decoder_kia_v6_reset,
    .get_hash_data  = subghz_protocol_decoder_kia_v6_get_hash_data,
    .serialize      = subghz_protocol_decoder_kia_v6_serialize,
    .deserialize    = subghz_protocol_decoder_kia_v6_deserialize,
    .get_string     = subghz_protocol_decoder_kia_v6_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_kia_v6_encoder = {
    .alloc       = subghz_protocol_encoder_kia_v6_alloc,
    .free        = subghz_protocol_encoder_kia_v6_free,
    .deserialize = subghz_protocol_encoder_kia_v6_deserialize,
    .stop        = subghz_protocol_encoder_kia_v6_stop,
    .yield       = subghz_protocol_encoder_kia_v6_yield,
};

const SubGhzProtocol subghz_protocol_kia_v6 = {
    .name = SUBGHZ_PROTOCOL_KIA_V6_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Send | SubGhzProtocolFlag_Automotive,
    .decoder = &subghz_protocol_kia_v6_decoder,
    .encoder = &subghz_protocol_kia_v6_encoder,
};
