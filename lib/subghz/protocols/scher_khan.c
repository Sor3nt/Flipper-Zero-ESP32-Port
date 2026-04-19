#include "scher_khan.h"
#include "../blocks/custom_btn_i.h"
#include "../blocks/generic.h"

#include <string.h>

#define TAG "SubGhzProtocolScherKhan"

static const uint8_t sk_pi_bytes[146] = {
    0x24, 0x3F, 0x6A, 0x88, 0x85, 0xA3, 0x08, 0xD3,
    0x13, 0x19, 0x8A, 0x2E, 0x03, 0x70, 0x73, 0x44,
    0xA4, 0x09, 0x38, 0x22, 0x29, 0x9F, 0x31, 0xD0,
    0x08, 0x2E, 0xFA, 0x98, 0xEC, 0x4E, 0x6C, 0x89,
    0x45, 0x28, 0x21, 0xE6, 0x38, 0xD0, 0x13, 0x77,
    0xBE, 0x54, 0x66, 0xCF, 0x34, 0xE9, 0x0C, 0x6C,
    0xC0, 0xAC, 0x29, 0xB7, 0xC9, 0x7C, 0x50, 0xDD,
    0x3F, 0x84, 0xD5, 0xB5, 0xB5, 0x47, 0x09, 0x17,
    0x92, 0x16, 0xD5, 0xD9, 0x89, 0x79, 0xFB, 0x1B,
    0xD1, 0x31, 0x0B, 0xA6, 0x98, 0xDF, 0xB5, 0xAC,
    0x2F, 0xFD, 0x72, 0xDB, 0xD0, 0x1A, 0xDF, 0xB7,
    0xB8, 0xE1, 0xAF, 0xED, 0x6A, 0x26, 0x7E, 0x96,
    0xBA, 0x7C, 0x90, 0x45, 0xF1, 0x2C, 0x7F, 0x99,
    0x24, 0xA1, 0x99, 0x47, 0xB3, 0x91, 0x6C, 0xF7,
    0x08, 0x01, 0xF2, 0xE2, 0x85, 0x8E, 0xFC, 0x16,
    0x63, 0x69, 0x20, 0xD8, 0x71, 0x57, 0x4E, 0x69,
    0xA4, 0x58, 0xFE, 0xA3, 0xF4, 0x93, 0x3D, 0x7E,
    0x0D, 0x95, 0x74, 0x8F, 0x72, 0x8E, 0xB6, 0x58,
    0x71, 0x8B,
};

static const uint8_t sk_pro1_encoded[64] = {
    0x63, 0x5A, 0x58, 0x98, 0xD6, 0xB3, 0x7E, 0x91,
    0x37, 0x1E, 0xBB, 0x78, 0x43, 0x06, 0x20, 0x65,
    0xC7, 0x4B, 0x3F, 0x73, 0x0D, 0xAE, 0x36, 0x86,
    0x1E, 0x7A, 0xDA, 0xEB, 0xBB, 0x0F, 0x5A, 0xA9,
    0x26, 0x3A, 0x51, 0xB2, 0x7A, 0xE0, 0x44, 0x61,
    0xAC, 0x53, 0x02, 0xFA, 0x42, 0xBD, 0x2D, 0x6F,
    0x85, 0xBB, 0x4A, 0xB5, 0xF9, 0x28, 0x46, 0xAF,
    0x2B, 0xB4, 0xB2, 0x90, 0xD0, 0x05, 0x19, 0x64,
};

static const uint8_t sk_pro2_encoded[64] = {
    0x90, 0x27, 0xA3, 0x9C, 0xEA, 0x6B, 0x8B, 0x4F,
    0x91, 0x47, 0x58, 0x87, 0x8A, 0xD8, 0xD1, 0x99,
    0x3B, 0xCD, 0x15, 0xFE, 0xF4, 0x1D, 0xEE, 0xE1,
    0xBF, 0xA4, 0x9E, 0xCB, 0x1C, 0x72, 0x5F, 0x95,
    0xAC, 0x28, 0xB0, 0x36, 0xE3, 0x2B, 0x1B, 0xAC,
    0x30, 0x91, 0xFE, 0x62, 0xD0, 0xD3, 0x6B, 0xA6,
    0x4F, 0x64, 0xC0, 0xF2, 0xD2, 0xCF, 0xCA, 0x36,
    0x53, 0x3D, 0x36, 0xAA, 0x33, 0x67, 0x19, 0x7F,
};

static const uint8_t sk_pro_crc_encoded[2] = {
    0x5B, 0x8C,
};

static void sk_pi_decode(uint8_t* out, const uint8_t* encoded, size_t offset, size_t len) {
    for(size_t i = 0; i < len; i++) {
        out[i] = encoded[i] ^ sk_pi_bytes[offset + i];
    }
}

static uint8_t scher_khan_pro_checksum(const uint8_t* data, uint8_t poly) {
    uint8_t cs = 0xFF;
    for(uint8_t a = 0; a < 6; a++) {
        uint8_t c = data[a];
        for(uint8_t b = 0; b < 8; b++) {
            uint8_t _cs = cs;
            cs <<= 1;
            if((_cs ^ c) & 0x80) cs ^= poly;
            c <<= 1;
        }
    }
    return cs;
}

static void scher_khan_pro_decrypt(
    const uint8_t* encrypted,
    uint8_t* decrypted,
    const uint8_t* key_table) {
    memset(decrypted, 0, 7);
    decrypted[0] = encrypted[0];
    uint8_t pkt_cnt = encrypted[0] & 0x0F;

    for(int i = 1; i < 6; i++) {
        for(int j = 0; j < 4; j++) {
            uint8_t cr = key_table[(pkt_cnt << 2) + j];
            uint8_t s = encrypted[i];

            uint8_t bit = 0x80 >> (cr >> 4);
            if((s & bit) == bit) decrypted[i] |= 0x80 >> (j << 1);

            bit = 0x80 >> (cr & 0x0F);
            if((s & bit) == bit) decrypted[i] |= 0x80 >> ((j << 1) + 1);
        }
        pkt_cnt = (pkt_cnt + 1) & 0x0F;
        decrypted[i] ^= encrypted[i - 1];
    }
}

static void scher_khan_pro_encrypt(
    const uint8_t* plain,
    uint8_t* encrypted,
    const uint8_t* key_table) {
    uint8_t src[7];
    memcpy(src, plain, 7);
    memset(encrypted, 0, 7);
    encrypted[0] = src[0];
    uint8_t pkt_cnt = src[0] & 0x0F;

    for(int i = 1; i < 6; i++) {
        src[i] ^= encrypted[i - 1];

        for(int j = 0; j < 4; j++) {
            uint8_t cr = key_table[(pkt_cnt << 2) + j];
            uint8_t s = src[i];

            uint8_t bit = 0x80 >> (j << 1);
            if((s & bit) == bit) encrypted[i] |= 0x80 >> (cr >> 4);

            bit = 0x80 >> ((j << 1) + 1);
            if((s & bit) == bit) encrypted[i] |= 0x80 >> (cr & 0x0F);
        }
        pkt_cnt = (pkt_cnt + 1) & 0x0F;
    }
}

static const char* scher_khan_btn_name(uint8_t btn) {
    switch(btn) {
    case 0x1: return "Lock";
    case 0x2: return "Unlock";
    case 0x3: return "Lock+Unlock";
    case 0x4: return "Trunk";
    case 0x5: return "Lock+Trunk";
    case 0x6: return "Unlock+Trunk";
    case 0x7: return "Lk+Ul+Tr";
    case 0x8: return "Start";
    case 0x9: return "Lock+Start";
    case 0xA: return "Unlock+Start";
    case 0xC: return "Trunk+Start";
    case 0xF: return "All/Panic";
    default:  return "?";
    }
}

static uint8_t scher_khan_btn_to_custom(uint8_t btn) {
    switch(btn) {
    case 0x1: return SUBGHZ_CUSTOM_BTN_UP;
    case 0x2: return SUBGHZ_CUSTOM_BTN_DOWN;
    case 0x4: return SUBGHZ_CUSTOM_BTN_LEFT;
    case 0x8: return SUBGHZ_CUSTOM_BTN_RIGHT;
    default:  return SUBGHZ_CUSTOM_BTN_OK;
    }
}

// Page 0: Lock(1), Unlock(2), Trunk(4), Start(8)
// Page 1: Lock+Unlock(3), Lock+Trunk(5), Unlock+Trunk(6), Lk+Ul+Tr(7)
// Page 2: Lock+Start(9), Unlock+Start(A), Trunk+Start(C), All/Panic(F)
static uint8_t scher_khan_custom_to_btn(uint8_t custom, uint8_t original_btn) {
    if(custom == SUBGHZ_CUSTOM_BTN_OK) return original_btn;

    uint8_t page = subghz_custom_btn_get_page();

    FURI_LOG_I(TAG, "custom_to_btn: original=0x%02X custom=%u page=%u",
        original_btn, custom, page);

    if(page == 0) {
        switch(custom) {
        case SUBGHZ_CUSTOM_BTN_UP:    return 0x1; // Lock
        case SUBGHZ_CUSTOM_BTN_DOWN:  return 0x2; // Unlock
        case SUBGHZ_CUSTOM_BTN_LEFT:  return 0x4; // Trunk
        case SUBGHZ_CUSTOM_BTN_RIGHT: return 0x8; // Start
        }
    } else if(page == 1) {
        switch(custom) {
        case SUBGHZ_CUSTOM_BTN_UP:    return 0x3; // Lock+Unlock
        case SUBGHZ_CUSTOM_BTN_DOWN:  return 0x5; // Lock+Trunk
        case SUBGHZ_CUSTOM_BTN_LEFT:  return 0x6; // Unlock+Trunk
        case SUBGHZ_CUSTOM_BTN_RIGHT: return 0x7; // Lk+Ul+Tr
        }
    } else {
        switch(custom) {
        case SUBGHZ_CUSTOM_BTN_UP:    return 0x9; // Lock+Start
        case SUBGHZ_CUSTOM_BTN_DOWN:  return 0xA; // Unlock+Start
        case SUBGHZ_CUSTOM_BTN_LEFT:  return 0xC; // Trunk+Start
        case SUBGHZ_CUSTOM_BTN_RIGHT: return 0xF; // All/Panic
        }
    }
    return original_btn;
}

static uint8_t scher_khan_get_btn_code(uint8_t original_btn) {
    return scher_khan_custom_to_btn(subghz_custom_btn_get(), original_btn);
}

static const SubGhzBlockConst subghz_protocol_scher_khan_const = {
    .te_short = 750,
    .te_long = 1100,
    .te_delta = 160,
    .min_count_bit_for_found = 35,
};

struct SubGhzProtocolDecoderScherKhan {
    SubGhzProtocolDecoderBase base;

    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    uint16_t header_count;
    const char* protocol_name;
};

struct SubGhzProtocolEncoderScherKhan {
    SubGhzProtocolEncoderBase base;

    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;

    const char* protocol_name;
};

typedef enum {
    ScherKhanDecoderStepReset = 0,
    ScherKhanDecoderStepCheckPreambula,
    ScherKhanDecoderStepSaveDuration,
    ScherKhanDecoderStepCheckDuration,
} ScherKhanDecoderStep;

static void subghz_protocol_scher_khan_check_remote_controller(
    SubGhzBlockGeneric* instance,
    const char** protocol_name);

const SubGhzProtocolDecoder subghz_protocol_scher_khan_decoder = {
    .alloc = subghz_protocol_decoder_scher_khan_alloc,
    .free = subghz_protocol_decoder_scher_khan_free,

    .feed = subghz_protocol_decoder_scher_khan_feed,
    .reset = subghz_protocol_decoder_scher_khan_reset,

    .get_hash_data = subghz_protocol_decoder_scher_khan_get_hash_data,
    .serialize = subghz_protocol_decoder_scher_khan_serialize,
    .deserialize = subghz_protocol_decoder_scher_khan_deserialize,
    .get_string = subghz_protocol_decoder_scher_khan_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_scher_khan_encoder = {
    .alloc = subghz_protocol_encoder_scher_khan_alloc,
    .free = subghz_protocol_encoder_scher_khan_free,

    .deserialize = subghz_protocol_encoder_scher_khan_deserialize,
    .stop = subghz_protocol_encoder_scher_khan_stop,
    .yield = subghz_protocol_encoder_scher_khan_yield,
};

const SubGhzProtocol subghz_protocol_scher_khan = {
    .name = SUBGHZ_PROTOCOL_SCHER_KHAN_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send | SubGhzProtocolFlag_Automotive,

    .decoder = &subghz_protocol_scher_khan_decoder,
    .encoder = &subghz_protocol_scher_khan_encoder,
};

// ======================== ENCODER ========================

void* subghz_protocol_encoder_scher_khan_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderScherKhan* instance = malloc(sizeof(SubGhzProtocolEncoderScherKhan));

    instance->base.protocol = &subghz_protocol_scher_khan;
    instance->generic.protocol_name = instance->base.protocol->name;

    instance->encoder.repeat = 7;
    instance->encoder.size_upload = 256;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    instance->encoder.front = 0;

    return instance;
}

void subghz_protocol_encoder_scher_khan_free(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderScherKhan* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

/**
 * Build the RF upload buffer matching the real Scher-Khan waveform.
 *
 * Real signal structure (from Boot_sh5.sub / Trunk_sh5.sub analysis):
 *
 *   Preamble:  6x pairs of ~750µs high / ~750µs low
 *   Header:    3x pairs of ~1500µs high / ~1500µs low
 *   Start bit: 1x pair  of ~750µs high / ~750µs low
 *   Data:      bit 0 = ~750µs  high / ~750µs  low
 *              bit 1 = ~1100µs high / ~1100µs low
 *   End:       ~1500µs high (marks frame end for decoder)
 */
static bool subghz_protocol_encoder_scher_khan_get_upload(
    SubGhzProtocolEncoderScherKhan* instance,
    uint8_t btn) {
    furi_check(instance);

    // For 51-bit dynamic: rebuild data with new button and incremented counter
    if(instance->generic.data_count_bit == 51) {
        if((instance->generic.cnt + 1) > 0xFFFF) {
            instance->generic.cnt = 0;
        } else {
            instance->generic.cnt += 1;
        }

        uint64_t upper = instance->generic.data & 0x7FFFFFFF0000ULL;
        upper = (upper & ~(0x0FULL << 24)) | ((uint64_t)(btn & 0x0F) << 24);
        instance->generic.data = upper | (instance->generic.cnt & 0xFFFF);
        instance->generic.btn = btn;
    }

    if(instance->generic.data_count_bit == 57) {
        if((instance->generic.cnt + 1) > 0xFFFF) {
            instance->generic.cnt = 0;
        } else {
            instance->generic.cnt += 1;
        }

        uint8_t kb[7];
        for(int i = 0; i < 7; i++) {
            kb[i] = (uint8_t)((instance->generic.data >> (48 - i * 8)) & 0xFF);
        }

        uint8_t crc_polys[2];
        sk_pi_decode(crc_polys, sk_pro_crc_encoded, 144, 2);

        uint8_t key_table[64];
        uint8_t poly;
        uint8_t crc1 = scher_khan_pro_checksum(kb, crc_polys[0]);
        if(crc1 == kb[6]) {
            sk_pi_decode(key_table, sk_pro1_encoded, 0, 64);
            poly = crc_polys[0];
        } else {
            sk_pi_decode(key_table, sk_pro2_encoded, 64, 64);
            poly = crc_polys[1];
        }

        uint8_t decrypted[7];
        scher_khan_pro_decrypt(kb, decrypted, key_table);

        decrypted[3] = (decrypted[3] & 0xE0) | (btn & 0x0F);
        decrypted[4] = (uint8_t)(instance->generic.cnt >> 8);
        decrypted[5] = (uint8_t)(instance->generic.cnt & 0xFF);

        uint8_t encrypted[7];
        scher_khan_pro_encrypt(decrypted, encrypted, key_table);

        encrypted[6] = scher_khan_pro_checksum(encrypted, poly);

        instance->generic.data = 0;
        for(int i = 0; i < 7; i++) {
            instance->generic.data |= ((uint64_t)encrypted[i] << (48 - i * 8));
        }
        instance->generic.data &= 0x01FFFFFFFFFFFFFFULL;
        instance->generic.btn = btn;
    }

    size_t index = 0;
    size_t needed = (6 * 2) + (3 * 2) + (1 * 2) + (instance->generic.data_count_bit * 2) + 1;
    if(needed > instance->encoder.size_upload) {
        FURI_LOG_E(TAG, "Upload buffer too small: need %zu, have %zu",
            needed, instance->encoder.size_upload);
        return false;
    }

    for(uint8_t i = 0; i < 6; i++) {
        instance->encoder.upload[index++] =
            level_duration_make(true, (uint32_t)subghz_protocol_scher_khan_const.te_short);
        instance->encoder.upload[index++] =
            level_duration_make(false, (uint32_t)subghz_protocol_scher_khan_const.te_short);
    }

    for(uint8_t i = 0; i < 3; i++) {
        instance->encoder.upload[index++] =
            level_duration_make(true, (uint32_t)subghz_protocol_scher_khan_const.te_short * 2);
        instance->encoder.upload[index++] =
            level_duration_make(false, (uint32_t)subghz_protocol_scher_khan_const.te_short * 2);
    }

    instance->encoder.upload[index++] =
        level_duration_make(true, (uint32_t)subghz_protocol_scher_khan_const.te_short);
    instance->encoder.upload[index++] =
        level_duration_make(false, (uint32_t)subghz_protocol_scher_khan_const.te_short);

    for(uint8_t i = instance->generic.data_count_bit; i > 0; i--) {
        if(bit_read(instance->generic.data, i - 1)) {
            instance->encoder.upload[index++] =
                level_duration_make(true, (uint32_t)subghz_protocol_scher_khan_const.te_long);
            instance->encoder.upload[index++] =
                level_duration_make(false, (uint32_t)subghz_protocol_scher_khan_const.te_long);
        } else {
            instance->encoder.upload[index++] =
                level_duration_make(true, (uint32_t)subghz_protocol_scher_khan_const.te_short);
            instance->encoder.upload[index++] =
                level_duration_make(false, (uint32_t)subghz_protocol_scher_khan_const.te_short);
        }
    }

    instance->encoder.upload[index++] =
        level_duration_make(true, (uint32_t)subghz_protocol_scher_khan_const.te_short * 2);

    instance->encoder.size_upload = index;

    FURI_LOG_I(TAG, "Upload built: %zu entries, %d bits, btn=0x%02X, cnt=0x%04lX, data=0x%016llX",
        index, instance->generic.data_count_bit, instance->generic.btn,
        (unsigned long)instance->generic.cnt, instance->generic.data);

    return true;
}

static SubGhzProtocolStatus subghz_protocol_encoder_scher_khan_serialize_internal(
    SubGhzProtocolEncoderScherKhan* instance,
    FlipperFormat* flipper_format) {

    const char* pname = NULL;
    subghz_protocol_scher_khan_check_remote_controller(&instance->generic, &pname);

    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    do {
        if(!flipper_format_insert_or_update_string_cstr(
               flipper_format, "Protocol", instance->generic.protocol_name)) {
            break;
        }

        uint32_t bits = instance->generic.data_count_bit;
        if(!flipper_format_insert_or_update_uint32(flipper_format, "Bit", &bits, 1)) {
            break;
        }

        char key_str[20];
        snprintf(key_str, sizeof(key_str), "%016llX", instance->generic.data);
        if(!flipper_format_insert_or_update_string_cstr(flipper_format, "Key", key_str)) {
            break;
        }

        if(!flipper_format_insert_or_update_uint32(
               flipper_format, "Serial", &instance->generic.serial, 1)) {
            break;
        }

        uint32_t temp = instance->generic.btn;
        if(!flipper_format_insert_or_update_uint32(flipper_format, "Btn", &temp, 1)) {
            break;
        }

        if(!flipper_format_insert_or_update_uint32(
               flipper_format, "Cnt", &instance->generic.cnt, 1)) {
            break;
        }

        ret = SubGhzProtocolStatusOk;
    } while(false);

    return ret;
}

SubGhzProtocolStatus
    subghz_protocol_encoder_scher_khan_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolEncoderScherKhan* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    flipper_format_rewind(flipper_format);

    do {
        FuriString* temp_str = furi_string_alloc();
        if(!flipper_format_read_string(flipper_format, "Protocol", temp_str)) {
            FURI_LOG_E(TAG, "Missing Protocol");
            furi_string_free(temp_str);
            break;
        }

        if(!furi_string_equal(temp_str, instance->base.protocol->name)) {
            FURI_LOG_E(TAG, "Wrong protocol %s != %s",
                furi_string_get_cstr(temp_str), instance->base.protocol->name);
            furi_string_free(temp_str);
            break;
        }
        furi_string_free(temp_str);

        uint32_t bit_count_temp;
        if(!flipper_format_read_uint32(flipper_format, "Bit", &bit_count_temp, 1)) {
            FURI_LOG_E(TAG, "Missing Bit");
            break;
        }
        instance->generic.data_count_bit = (uint8_t)bit_count_temp;

        temp_str = furi_string_alloc();
        if(!flipper_format_read_string(flipper_format, "Key", temp_str)) {
            FURI_LOG_E(TAG, "Missing Key");
            furi_string_free(temp_str);
            break;
        }

        const char* key_str = furi_string_get_cstr(temp_str);
        uint64_t key = 0;
        size_t str_len = strlen(key_str);
        size_t hex_pos = 0;
        for(size_t i = 0; i < str_len && hex_pos < 16; i++) {
            char c = key_str[i];
            if(c == ' ') continue;

            uint8_t nibble;
            if(c >= '0' && c <= '9') {
                nibble = c - '0';
            } else if(c >= 'A' && c <= 'F') {
                nibble = c - 'A' + 10;
            } else if(c >= 'a' && c <= 'f') {
                nibble = c - 'a' + 10;
            } else {
                FURI_LOG_E(TAG, "Invalid hex character: %c", c);
                furi_string_free(temp_str);
                break;
            }
            key = (key << 4) | nibble;
            hex_pos++;
        }
        furi_string_free(temp_str);

        if(hex_pos == 0) {
            FURI_LOG_E(TAG, "Invalid key length");
            break;
        }

        instance->generic.data = key;
        FURI_LOG_I(TAG, "Parsed key: 0x%016llX", instance->generic.data);

        if(instance->generic.data == 0) {
            FURI_LOG_E(TAG, "Key is zero after parsing!");
            break;
        }

        if(!flipper_format_read_uint32(flipper_format, "Serial", &instance->generic.serial, 1)) {
            instance->generic.serial = 0;
            FURI_LOG_I(TAG, "Serial not found, defaulting to 0");
        } else {
            FURI_LOG_I(TAG, "Read serial: 0x%08lX", instance->generic.serial);
        }

        uint32_t btn_temp;
        if(flipper_format_read_uint32(flipper_format, "Btn", &btn_temp, 1)) {
            instance->generic.btn = (uint8_t)btn_temp;
            FURI_LOG_I(TAG, "Read button: 0x%02X", instance->generic.btn);
        } else {
            instance->generic.btn = 0;
            FURI_LOG_I(TAG, "Button not found, defaulting to 0");
        }

        subghz_custom_btn_set_original(scher_khan_btn_to_custom(instance->generic.btn));
        subghz_custom_btn_set_max(4);
        subghz_custom_btn_set_pages(true);
        subghz_custom_btn_set_max_pages(3);

        uint32_t cnt_temp;
        if(flipper_format_read_uint32(flipper_format, "Cnt", &cnt_temp, 1)) {
            instance->generic.cnt = (uint16_t)cnt_temp;
            FURI_LOG_I(TAG, "Read counter: 0x%04lX", (unsigned long)instance->generic.cnt);
        } else {
            instance->generic.cnt = 0;
        }

        if(!flipper_format_read_uint32(
               flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1)) {
            instance->encoder.repeat = 7;
            FURI_LOG_D(TAG, "Repeat not found, using default 7");
        }

        const char* pname = NULL;
        subghz_protocol_scher_khan_check_remote_controller(&instance->generic, &pname);
        instance->protocol_name = pname;

        uint8_t selected_btn = scher_khan_get_btn_code(instance->generic.btn);

        FURI_LOG_I(TAG,
            "Building upload: original_btn=0x%02X, selected_btn=0x%02X, bits=%d",
            instance->generic.btn, selected_btn, instance->generic.data_count_bit);

        if(!subghz_protocol_encoder_scher_khan_get_upload(instance, selected_btn)) {
            FURI_LOG_E(TAG, "Failed to generate upload");
            break;
        }

        subghz_protocol_encoder_scher_khan_serialize_internal(instance, flipper_format);

        instance->encoder.is_running = true;
        instance->encoder.front = 0;

        FURI_LOG_I(TAG, "Encoder ready: repeat=%u, size_upload=%zu",
            instance->encoder.repeat, instance->encoder.size_upload);

        ret = SubGhzProtocolStatusOk;
    } while(false);

    return ret;
}

void subghz_protocol_encoder_scher_khan_stop(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderScherKhan* instance = context;
    instance->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_scher_khan_yield(void* context) {
    SubGhzProtocolEncoderScherKhan* instance = context;

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

// ======================== DECODER ========================

void* subghz_protocol_decoder_scher_khan_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderScherKhan* instance = malloc(sizeof(SubGhzProtocolDecoderScherKhan));
    instance->base.protocol = &subghz_protocol_scher_khan;
    instance->generic.protocol_name = instance->base.protocol->name;

    return instance;
}

void subghz_protocol_decoder_scher_khan_free(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderScherKhan* instance = context;
    free(instance);
}

void subghz_protocol_decoder_scher_khan_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderScherKhan* instance = context;
    instance->decoder.parser_step = ScherKhanDecoderStepReset;
}

void subghz_protocol_decoder_scher_khan_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderScherKhan* instance = context;

    switch(instance->decoder.parser_step) {
    case ScherKhanDecoderStepReset:
        if((level) && (DURATION_DIFF(duration, subghz_protocol_scher_khan_const.te_short * 2) <
                       subghz_protocol_scher_khan_const.te_delta)) {
            instance->decoder.parser_step = ScherKhanDecoderStepCheckPreambula;
            instance->decoder.te_last = duration;
            instance->header_count = 0;
        }
        break;
    case ScherKhanDecoderStepCheckPreambula:
        if(level) {
            if((DURATION_DIFF(duration, subghz_protocol_scher_khan_const.te_short * 2) <
                subghz_protocol_scher_khan_const.te_delta) ||
               (DURATION_DIFF(duration, subghz_protocol_scher_khan_const.te_short) <
                subghz_protocol_scher_khan_const.te_delta)) {
                instance->decoder.te_last = duration;
            } else {
                instance->decoder.parser_step = ScherKhanDecoderStepReset;
            }
        } else if(
            (DURATION_DIFF(duration, subghz_protocol_scher_khan_const.te_short * 2) <
             subghz_protocol_scher_khan_const.te_delta) ||
            (DURATION_DIFF(duration, subghz_protocol_scher_khan_const.te_short) <
             subghz_protocol_scher_khan_const.te_delta)) {
            if(DURATION_DIFF(
                   instance->decoder.te_last, subghz_protocol_scher_khan_const.te_short * 2) <
               subghz_protocol_scher_khan_const.te_delta) {
                instance->header_count++;
                break;
            } else if(
                DURATION_DIFF(
                    instance->decoder.te_last, subghz_protocol_scher_khan_const.te_short) <
                subghz_protocol_scher_khan_const.te_delta) {
                if(instance->header_count >= 2) {
                    instance->decoder.parser_step = ScherKhanDecoderStepSaveDuration;
                    instance->decoder.decode_data = 0;
                    instance->decoder.decode_count_bit = 1;
                } else {
                    instance->decoder.parser_step = ScherKhanDecoderStepReset;
                }
            } else {
                instance->decoder.parser_step = ScherKhanDecoderStepReset;
            }
        } else {
            instance->decoder.parser_step = ScherKhanDecoderStepReset;
        }
        break;
    case ScherKhanDecoderStepSaveDuration:
        if(level) {
            if(duration >= (subghz_protocol_scher_khan_const.te_delta * 2UL +
                            subghz_protocol_scher_khan_const.te_long)) {
                instance->decoder.parser_step = ScherKhanDecoderStepReset;
                if(instance->decoder.decode_count_bit >=
                   subghz_protocol_scher_khan_const.min_count_bit_for_found) {
                    instance->generic.data = instance->decoder.decode_data;
                    instance->generic.data_count_bit = instance->decoder.decode_count_bit;
                    if(instance->base.callback)
                        instance->base.callback(&instance->base, instance->base.context);
                }
                instance->decoder.decode_data = 0;
                instance->decoder.decode_count_bit = 0;
                break;
            } else {
                instance->decoder.te_last = duration;
                instance->decoder.parser_step = ScherKhanDecoderStepCheckDuration;
            }

        } else {
            instance->decoder.parser_step = ScherKhanDecoderStepReset;
        }
        break;
    case ScherKhanDecoderStepCheckDuration:
        if(!level) {
            if((DURATION_DIFF(
                    instance->decoder.te_last, subghz_protocol_scher_khan_const.te_short) <
                subghz_protocol_scher_khan_const.te_delta) &&
               (DURATION_DIFF(duration, subghz_protocol_scher_khan_const.te_short) <
                subghz_protocol_scher_khan_const.te_delta)) {
                subghz_protocol_blocks_add_bit(&instance->decoder, 0);
                instance->decoder.parser_step = ScherKhanDecoderStepSaveDuration;
            } else if(
                (DURATION_DIFF(
                     instance->decoder.te_last, subghz_protocol_scher_khan_const.te_long) <
                 subghz_protocol_scher_khan_const.te_delta) &&
                (DURATION_DIFF(duration, subghz_protocol_scher_khan_const.te_long) <
                 subghz_protocol_scher_khan_const.te_delta)) {
                subghz_protocol_blocks_add_bit(&instance->decoder, 1);
                instance->decoder.parser_step = ScherKhanDecoderStepSaveDuration;
            } else {
                instance->decoder.parser_step = ScherKhanDecoderStepReset;
            }
        } else {
            instance->decoder.parser_step = ScherKhanDecoderStepReset;
        }
        break;
    }
}

static void subghz_protocol_scher_khan_check_remote_controller(
    SubGhzBlockGeneric* instance,
    const char** protocol_name) {

    switch(instance->data_count_bit) {
    case 35:
        *protocol_name = "MAGIC CODE, Static";
        instance->serial = 0;
        instance->btn = 0;
        instance->cnt = 0;
        break;
    case 51:
        *protocol_name = "MAGIC CODE, Dynamic";
        instance->serial = ((instance->data >> 24) & 0xFFFFFF0) | ((instance->data >> 20) & 0x0F);
        instance->btn = (instance->data >> 24) & 0x0F;
        instance->cnt = instance->data & 0xFFFF;
        break;
    case 57: {
        uint8_t kb[7];
        for(int i = 0; i < 7; i++) {
            kb[i] = (uint8_t)((instance->data >> (48 - i * 8)) & 0xFF);
        }

        uint8_t crc_polys[2];
        sk_pi_decode(crc_polys, sk_pro_crc_encoded, 144, 2);

        uint8_t key_table[64];
        uint8_t decrypted[7];
        bool found = false;

        uint8_t computed_crc = scher_khan_pro_checksum(kb, crc_polys[0]);
        if(computed_crc == kb[6]) {
            sk_pi_decode(key_table, sk_pro1_encoded, 0, 64);
            scher_khan_pro_decrypt(kb, decrypted, key_table);
            *protocol_name = "MAGIC CODE PRO1";
            found = true;
        }

        if(!found) {
            computed_crc = scher_khan_pro_checksum(kb, crc_polys[1]);
            if(computed_crc == kb[6]) {
                sk_pi_decode(key_table, sk_pro2_encoded, 64, 64);
                scher_khan_pro_decrypt(kb, decrypted, key_table);
                *protocol_name = "MAGIC CODE PRO2";
                found = true;
            }
        }

        if(found) {
            instance->serial =
                ((uint32_t)decrypted[0] << 16) |
                ((uint32_t)decrypted[1] << 8) |
                (uint32_t)decrypted[2];
            instance->btn = decrypted[3] & 0x0F;
            instance->cnt =
                ((uint16_t)decrypted[4] << 8) | decrypted[5];
        } else {
            *protocol_name = "MAGIC CODE PRO/PRO2";
            instance->serial = 0;
            instance->btn = 0;
            instance->cnt = 0;
        }
        break;
    }
    case 63:
        *protocol_name = "MAGIC CODE, Response";
        instance->serial = 0;
        instance->btn = 0;
        instance->cnt = 0;
        break;
    case 64:
        *protocol_name = "MAGICAR, Response";
        instance->serial = 0;
        instance->btn = 0;
        instance->cnt = 0;
        break;
    case 81:
    case 82:
        *protocol_name = "MAGIC CODE PRO,\n Response";
        instance->serial = 0;
        instance->btn = 0;
        instance->cnt = 0;
        break;
    default:
        *protocol_name = "Unknown";
        instance->serial = 0;
        instance->btn = 0;
        instance->cnt = 0;
        break;
    }
}

uint8_t subghz_protocol_decoder_scher_khan_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderScherKhan* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_scher_khan_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderScherKhan* instance = context;

    const char* pname = NULL;
    subghz_protocol_scher_khan_check_remote_controller(&instance->generic, &pname);

    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    do {
        if(!flipper_format_write_header_cstr(
               flipper_format, "Flipper SubGhz Key File", 1)) {
            break;
        }
        if(preset != NULL) {
            if(!flipper_format_insert_or_update_uint32(
                   flipper_format, "Frequency", &preset->frequency, 1)) {
                break;
            }
            FuriString* preset_str = furi_string_alloc();
            subghz_block_generic_get_preset_name(
                furi_string_get_cstr(preset->name), preset_str);
            if(!flipper_format_insert_or_update_string_cstr(
                   flipper_format, "Preset", furi_string_get_cstr(preset_str))) {
                furi_string_free(preset_str);
                break;
            }
            furi_string_free(preset_str);
        }

        if(!flipper_format_insert_or_update_string_cstr(
               flipper_format, "Protocol", instance->generic.protocol_name)) {
            break;
        }

        uint32_t bits = instance->generic.data_count_bit;
        if(!flipper_format_insert_or_update_uint32(flipper_format, "Bit", &bits, 1)) {
            break;
        }

        char key_str[20];
        snprintf(key_str, sizeof(key_str), "%016llX", instance->generic.data);
        if(!flipper_format_insert_or_update_string_cstr(flipper_format, "Key", key_str)) {
            break;
        }

        if(!flipper_format_insert_or_update_uint32(
               flipper_format, "Serial", &instance->generic.serial, 1)) {
            break;
        }

        uint32_t temp = instance->generic.btn;
        if(!flipper_format_insert_or_update_uint32(flipper_format, "Btn", &temp, 1)) {
            break;
        }

        if(!flipper_format_insert_or_update_uint32(
               flipper_format, "Cnt", &instance->generic.cnt, 1)) {
            break;
        }

        ret = SubGhzProtocolStatusOk;
    } while(false);

    return ret;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_scher_khan_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderScherKhan* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    flipper_format_rewind(flipper_format);

    do {
        FuriString* temp_str = furi_string_alloc();
        if(!flipper_format_read_string(flipper_format, "Protocol", temp_str)) {
            FURI_LOG_E(TAG, "Missing Protocol");
            furi_string_free(temp_str);
            break;
        }

        if(!furi_string_equal(temp_str, instance->base.protocol->name)) {
            FURI_LOG_E(TAG, "Wrong protocol %s != %s",
                furi_string_get_cstr(temp_str), instance->base.protocol->name);
            furi_string_free(temp_str);
            break;
        }
        furi_string_free(temp_str);

        uint32_t bit_count_temp;
        if(!flipper_format_read_uint32(flipper_format, "Bit", &bit_count_temp, 1)) {
            FURI_LOG_E(TAG, "Missing Bit");
            break;
        }
        instance->generic.data_count_bit = (uint8_t)bit_count_temp;

        temp_str = furi_string_alloc();
        if(!flipper_format_read_string(flipper_format, "Key", temp_str)) {
            FURI_LOG_E(TAG, "Missing Key");
            furi_string_free(temp_str);
            break;
        }

        const char* key_str = furi_string_get_cstr(temp_str);
        uint64_t key = 0;
        size_t str_len = strlen(key_str);
        size_t hex_pos = 0;
        for(size_t i = 0; i < str_len && hex_pos < 16; i++) {
            char c = key_str[i];
            if(c == ' ') continue;
            uint8_t nibble;
            if(c >= '0' && c <= '9') nibble = c - '0';
            else if(c >= 'A' && c <= 'F') nibble = c - 'A' + 10;
            else if(c >= 'a' && c <= 'f') nibble = c - 'a' + 10;
            else {
                FURI_LOG_E(TAG, "Invalid hex character: %c", c);
                furi_string_free(temp_str);
                break;
            }
            key = (key << 4) | nibble;
            hex_pos++;
        }
        furi_string_free(temp_str);

        if(hex_pos == 0) {
            FURI_LOG_E(TAG, "Invalid key length");
            break;
        }

        instance->generic.data = key;

        if(!flipper_format_read_uint32(flipper_format, "Serial", &instance->generic.serial, 1)) {
            instance->generic.serial = 0;
        }

        uint32_t btn_temp;
        if(flipper_format_read_uint32(flipper_format, "Btn", &btn_temp, 1)) {
            instance->generic.btn = (uint8_t)btn_temp;
        } else {
            instance->generic.btn = 0;
        }

        uint32_t cnt_temp;
        if(flipper_format_read_uint32(flipper_format, "Cnt", &cnt_temp, 1)) {
            instance->generic.cnt = (uint16_t)cnt_temp;
        } else {
            instance->generic.cnt = 0;
        }

        FURI_LOG_I(TAG, "Decoder deserialized");
        ret = SubGhzProtocolStatusOk;
    } while(false);

    return ret;
}

void subghz_protocol_decoder_scher_khan_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderScherKhan* instance = context;

    subghz_protocol_scher_khan_check_remote_controller(
        &instance->generic, &instance->protocol_name);

    subghz_custom_btn_set_original(scher_khan_btn_to_custom(instance->generic.btn));
    subghz_custom_btn_set_max(4);
    subghz_custom_btn_set_pages(true);
        subghz_custom_btn_set_max_pages(3);

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Key:0x%lX%08lX\r\n"
        "Sn:%07lX Btn:[%s]\r\n"
        "Cntr:%04lX\r\n"
        "Pt: %s\r\n",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        (uint32_t)(instance->generic.data >> 32),
        (uint32_t)instance->generic.data,
        instance->generic.serial,
        scher_khan_btn_name(scher_khan_get_btn_code(instance->generic.btn)),
        instance->generic.cnt,
        instance->protocol_name);
}
