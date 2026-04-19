#include "kia_v3_v4.h"

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"
#include "../blocks/custom_btn_i.h"

#define TAG "SubGhzProtocolKiaV3V4"

#define KIA_MF_KEY 0xA8F5DFFC8DAA5CDBULL

#define KIA_V3_V4_PREAMBLE_PAIRS     16
#define KIA_V3_V4_TOTAL_BURSTS       3
#define KIA_V3_V4_INTER_BURST_GAP_US 10000
#define KIA_V3_V4_SYNC_DURATION      1200

static const char* kia_version_names[] = {"KIA/HYU V4", "KIA/HYU V3"};

static const SubGhzBlockConst subghz_protocol_kia_v3_v4_const = {
    .te_short = 400,
    .te_long = 800,
    .te_delta = 150,
    .min_count_bit_for_found = 68,
};

struct SubGhzProtocolDecoderKiaV3V4 {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
    uint16_t header_count;

    uint8_t raw_bits[32];
    uint16_t raw_bit_count;
    bool is_v3_sync;

    uint32_t encrypted;
    uint32_t decrypted;
    uint8_t crc;
    uint8_t version;
};

struct SubGhzProtocolEncoderKiaV3V4 {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;

    uint32_t serial;
    uint8_t btn;
    uint16_t cnt;
    uint8_t version;
    uint8_t crc;

    uint32_t encrypted;
    uint32_t decrypted;
};

typedef enum {
    KiaV3V4DecoderStepReset = 0,
    KiaV3V4DecoderStepCheckPreamble,
    KiaV3V4DecoderStepCollectRawBits,
} KiaV3V4DecoderStep;

static uint32_t keeloq_common_decrypt(uint32_t data, uint64_t key) {
    uint32_t block = data;
    uint64_t tkey = key;
    for(int i = 0; i < 528; i++) {
        int lutkey = ((block >> 0) & 1) | ((block >> 7) & 2) | ((block >> 17) & 4) |
                     ((block >> 22) & 8) | ((block >> 26) & 16);
        int lsb =
            ((block >> 31) ^ ((block >> 15) & 1) ^ ((0x3A5C742E >> lutkey) & 1) ^
             ((tkey >> 15) & 1));
        block = ((block & 0x7FFFFFFF) << 1) | lsb;
        tkey = ((tkey & 0x7FFFFFFFFFFFFFFFULL) << 1) | (tkey >> 63);
    }
    return block;
}

static uint32_t keeloq_common_encrypt(uint32_t data, uint64_t key) {
    uint32_t block = data;
    uint64_t tkey = key;

    for(int i = 0; i < 528; i++) {
        int lutkey = ((block >> 1) & 1) | ((block >> 8) & 2) | ((block >> 18) & 4) |
                     ((block >> 23) & 8) | ((block >> 27) & 16);
        int msb =
            ((block >> 0) ^ ((block >> 16) & 1) ^ ((0x3A5C742E >> lutkey) & 1) ^
             ((tkey >> 0) & 1));
        block = ((block >> 1) & 0x7FFFFFFF) | (msb << 31);
        tkey = ((tkey >> 1) & 0x7FFFFFFFFFFFFFFFULL) | ((tkey & 1) << 63);
    }
    return block;
}

static uint8_t reverse8(uint8_t byte) {
    byte = (byte & 0xF0) >> 4 | (byte & 0x0F) << 4;
    byte = (byte & 0xCC) >> 2 | (byte & 0x33) << 2;
    byte = (byte & 0xAA) >> 1 | (byte & 0x55) << 1;
    return byte;
}

static void kia_v3_v4_add_raw_bit(SubGhzProtocolDecoderKiaV3V4* instance, bool bit) {
    if(instance->raw_bit_count < 256) {
        uint16_t byte_idx = instance->raw_bit_count / 8;
        uint8_t bit_idx = 7 - (instance->raw_bit_count % 8);
        if(bit) {
            instance->raw_bits[byte_idx] |= (1 << bit_idx);
        } else {
            instance->raw_bits[byte_idx] &= ~(1 << bit_idx);
        }
        instance->raw_bit_count++;
    }
}

static uint8_t kia_v3_v4_calculate_crc(uint8_t* bytes) {
    uint8_t crc = 0;
    for(int i = 0; i < 8; i++) {
        crc ^= (bytes[i] & 0x0F) ^ (bytes[i] >> 4);
    }
    return crc & 0x0F;
}

static bool kia_v3_v4_process_buffer(SubGhzProtocolDecoderKiaV3V4* instance) {
    if(instance->raw_bit_count < 68) {
        return false;
    }

    uint8_t* b = instance->raw_bits;

    if(instance->is_v3_sync) {
        uint16_t num_bytes = (instance->raw_bit_count + 7) / 8;
        for(uint16_t i = 0; i < num_bytes; i++) {
            b[i] = ~b[i];
        }
    }

    uint8_t crc = (b[8] >> 4) & 0x0F;

    uint32_t encrypted = ((uint32_t)reverse8(b[3]) << 24) | ((uint32_t)reverse8(b[2]) << 16) |
                         ((uint32_t)reverse8(b[1]) << 8) | (uint32_t)reverse8(b[0]);

    uint32_t serial = ((uint32_t)reverse8(b[7] & 0xF0) << 24) | ((uint32_t)reverse8(b[6]) << 16) |
                      ((uint32_t)reverse8(b[5]) << 8) | (uint32_t)reverse8(b[4]);

    uint8_t btn = (reverse8(b[7]) & 0xF0) >> 4;
    uint8_t our_serial_lsb = serial & 0xFF;

    uint32_t decrypted = keeloq_common_decrypt(encrypted, KIA_MF_KEY);
    uint8_t dec_btn = (decrypted >> 28) & 0x0F;
    uint8_t dec_serial_lsb = (decrypted >> 16) & 0xFF;

    if(dec_btn != btn || dec_serial_lsb != our_serial_lsb) {
        return false;
    }

    instance->encrypted = encrypted;
    instance->decrypted = decrypted;
    instance->crc = crc;
    instance->generic.serial = serial;
    instance->generic.btn = btn;
    instance->generic.cnt = decrypted & 0xFFFF;
    instance->version = instance->is_v3_sync ? 1 : 0;

    uint64_t key_data = ((uint64_t)b[0] << 56) | ((uint64_t)b[1] << 48) | ((uint64_t)b[2] << 40) |
                        ((uint64_t)b[3] << 32) | ((uint64_t)b[4] << 24) | ((uint64_t)b[5] << 16) |
                        ((uint64_t)b[6] << 8) | (uint64_t)b[7];
    instance->generic.data = key_data;
    instance->generic.data_count_bit = 68;
    
    instance->decoder.decode_data = key_data;
    instance->decoder.decode_count_bit = 68;

    if(subghz_custom_btn_get_original() == 0) {
        subghz_custom_btn_set_original(instance->generic.btn);
    }
    subghz_custom_btn_set_max(5);

    return true;
}

const SubGhzProtocolDecoder subghz_protocol_kia_v3_v4_decoder = {
    .alloc = subghz_protocol_decoder_kia_v3_v4_alloc,
    .free = subghz_protocol_decoder_kia_v3_v4_free,
    .feed = subghz_protocol_decoder_kia_v3_v4_feed,
    .reset = subghz_protocol_decoder_kia_v3_v4_reset,
    .get_hash_data = subghz_protocol_decoder_kia_v3_v4_get_hash_data,
    .serialize = subghz_protocol_decoder_kia_v3_v4_serialize,
    .deserialize = subghz_protocol_decoder_kia_v3_v4_deserialize,
    .get_string = subghz_protocol_decoder_kia_v3_v4_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_kia_v3_v4_encoder = {
    .alloc = subghz_protocol_encoder_kia_v3_v4_alloc,
    .free = subghz_protocol_encoder_kia_v3_v4_free,
    .deserialize = subghz_protocol_encoder_kia_v3_v4_deserialize,
    .stop = subghz_protocol_encoder_kia_v3_v4_stop,
    .yield = subghz_protocol_encoder_kia_v3_v4_yield,
};

const SubGhzProtocol subghz_protocol_kia_v3_v4 = {
    .name = SUBGHZ_PROTOCOL_KIA_V3_V4_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | 
            SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Load | 
            SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send | SubGhzProtocolFlag_Automotive,
    .decoder = &subghz_protocol_kia_v3_v4_decoder,
    .encoder = &subghz_protocol_kia_v3_v4_encoder,
};

static const char* subghz_protocol_kia_v3_v4_get_name_button(uint8_t btn) {
    switch(btn) {
    case 0x1: return "Lock";
    case 0x2: return "Unlock";
    case 0x3: return "Trunk";
    case 0x4: return "Panic";
    case 0x8: return "Horn";
    default:  return "Unknown";
    }
}

void* subghz_protocol_encoder_kia_v3_v4_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderKiaV3V4* instance = malloc(sizeof(SubGhzProtocolEncoderKiaV3V4));

    instance->base.protocol = &subghz_protocol_kia_v3_v4;
    instance->generic.protocol_name = instance->base.protocol->name;

    instance->serial = 0;
    instance->btn = 0;
    instance->cnt = 0;
    instance->version = 0;

    instance->encoder.size_upload = 600;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.repeat = 10;
    instance->encoder.front = 0;
    instance->encoder.is_running = false;

    return instance;
}

void subghz_protocol_encoder_kia_v3_v4_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderKiaV3V4* instance = context;
    if(instance->encoder.upload) {
        free(instance->encoder.upload);
    }
    free(instance);
}

static void subghz_protocol_encoder_kia_v3_v4_build_packet(
    SubGhzProtocolEncoderKiaV3V4* instance,
    uint8_t* raw_bytes) {
    uint32_t plaintext = (instance->cnt & 0xFFFF) | ((instance->serial & 0xFF) << 16) |
                         (0x1 << 24) | ((instance->btn & 0x0F) << 28);

    instance->decrypted = plaintext;

    uint32_t encrypted = keeloq_common_encrypt(plaintext, KIA_MF_KEY);
    instance->encrypted = encrypted;

    raw_bytes[0] = reverse8((encrypted >> 0) & 0xFF);
    raw_bytes[1] = reverse8((encrypted >> 8) & 0xFF);
    raw_bytes[2] = reverse8((encrypted >> 16) & 0xFF);
    raw_bytes[3] = reverse8((encrypted >> 24) & 0xFF);

    uint32_t serial_btn = (instance->serial & 0x0FFFFFFF) |
                          ((uint32_t)(instance->btn & 0x0F) << 28);
    raw_bytes[4] = reverse8((serial_btn >> 0) & 0xFF);
    raw_bytes[5] = reverse8((serial_btn >> 8) & 0xFF);
    raw_bytes[6] = reverse8((serial_btn >> 16) & 0xFF);
    raw_bytes[7] = reverse8((serial_btn >> 24) & 0xFF);

    uint8_t crc = kia_v3_v4_calculate_crc(raw_bytes);
    raw_bytes[8] = (crc << 4);
    instance->crc = crc;

    instance->generic.data = ((uint64_t)raw_bytes[0] << 56) | ((uint64_t)raw_bytes[1] << 48) |
                             ((uint64_t)raw_bytes[2] << 40) | ((uint64_t)raw_bytes[3] << 32) |
                             ((uint64_t)raw_bytes[4] << 24) | ((uint64_t)raw_bytes[5] << 16) |
                             ((uint64_t)raw_bytes[6] << 8) | (uint64_t)raw_bytes[7];
    instance->generic.data_count_bit = 68;
}

static void subghz_protocol_encoder_kia_v3_v4_get_upload(SubGhzProtocolEncoderKiaV3V4* instance) {
    furi_assert(instance);

    uint8_t raw_bytes[9];
    subghz_protocol_encoder_kia_v3_v4_build_packet(instance, raw_bytes);

    if(instance->version == 1) {
        for(int i = 0; i < 9; i++) {
            raw_bytes[i] = ~raw_bytes[i];
        }
    }

    size_t index = 0;

    for(uint8_t burst = 0; burst < KIA_V3_V4_TOTAL_BURSTS; burst++) {
        if(burst > 0) {
            instance->encoder.upload[index++] =
                level_duration_make(false, KIA_V3_V4_INTER_BURST_GAP_US);
        }

        for(int i = 0; i < KIA_V3_V4_PREAMBLE_PAIRS; i++) {
            instance->encoder.upload[index++] =
                level_duration_make(true, subghz_protocol_kia_v3_v4_const.te_short);
            instance->encoder.upload[index++] =
                level_duration_make(false, subghz_protocol_kia_v3_v4_const.te_short);
        }

        if(instance->version == 0) {
            instance->encoder.upload[index++] = level_duration_make(true, KIA_V3_V4_SYNC_DURATION);
            instance->encoder.upload[index++] =
                level_duration_make(false, subghz_protocol_kia_v3_v4_const.te_short);
        } else {
            instance->encoder.upload[index++] =
                level_duration_make(true, subghz_protocol_kia_v3_v4_const.te_short);
            instance->encoder.upload[index++] =
                level_duration_make(false, KIA_V3_V4_SYNC_DURATION);
        }

        for(int byte_idx = 0; byte_idx < 9; byte_idx++) {
            int bits_in_byte = (byte_idx == 8) ? 4 : 8;

            for(int bit_idx = 7; bit_idx >= (8 - bits_in_byte); bit_idx--) {
                bool bit = (raw_bytes[byte_idx] >> bit_idx) & 1;

                if(bit) {
                    instance->encoder.upload[index++] =
                        level_duration_make(true, subghz_protocol_kia_v3_v4_const.te_long);
                    instance->encoder.upload[index++] =
                        level_duration_make(false, subghz_protocol_kia_v3_v4_const.te_short);
                } else {
                    instance->encoder.upload[index++] =
                        level_duration_make(true, subghz_protocol_kia_v3_v4_const.te_short);
                    instance->encoder.upload[index++] =
                        level_duration_make(false, subghz_protocol_kia_v3_v4_const.te_long);
                }
            }
        }
    }

    instance->encoder.size_upload = index;
    instance->encoder.front = 0;
}

SubGhzProtocolStatus
    subghz_protocol_encoder_kia_v3_v4_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolEncoderKiaV3V4* instance = context;

    instance->encoder.is_running = false;
    instance->encoder.front = 0;

    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    flipper_format_rewind(flipper_format);

    do {
        FuriString* temp_str = furi_string_alloc();
        if(!flipper_format_read_string(flipper_format, "Protocol", temp_str)) {
            furi_string_free(temp_str);
            break;
        }

        const char* proto_str = furi_string_get_cstr(temp_str);
        if(!furi_string_equal(temp_str, instance->base.protocol->name) &&
           strcmp(proto_str, "KIA/HYU V3") != 0 && strcmp(proto_str, "KIA/HYU V4") != 0) {
            furi_string_free(temp_str);
            break;
        }

        bool version_from_protocol_name = false;

        if(strcmp(proto_str, "KIA/HYU V3") == 0) {
            instance->version = 1;
            version_from_protocol_name = true;
        } else if(strcmp(proto_str, "KIA/HYU V4") == 0) {
            instance->version = 0;
            version_from_protocol_name = true;
        }

        furi_string_free(temp_str);

        flipper_format_rewind(flipper_format);
        uint32_t bit_count_temp;
        if(!flipper_format_read_uint32(flipper_format, "Bit", &bit_count_temp, 1)) {
            break;
        }
        instance->generic.data_count_bit = 68;

        flipper_format_rewind(flipper_format);
        temp_str = furi_string_alloc();
        if(!flipper_format_read_string(flipper_format, "Key", temp_str)) {
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
                break;
            }

            key = (key << 4) | nibble;
            hex_pos++;
        }

        furi_string_free(temp_str);

        if(hex_pos != 16) {
            FURI_LOG_E(TAG, "Invalid key length: %zu nibbles", hex_pos);
            break;
        }

        instance->generic.data = key;

        flipper_format_rewind(flipper_format);
        if(!flipper_format_read_uint32(flipper_format, "Serial", &instance->serial, 1)) {
            uint8_t b[8];
            b[0] = (key >> 56) & 0xFF;
            b[1] = (key >> 48) & 0xFF;
            b[2] = (key >> 40) & 0xFF;
            b[3] = (key >> 32) & 0xFF;
            b[4] = (key >> 24) & 0xFF;
            b[5] = (key >> 16) & 0xFF;
            b[6] = (key >> 8) & 0xFF;
            b[7] = key & 0xFF;

            instance->serial = ((uint32_t)reverse8(b[7] & 0xF0) << 24) |
                               ((uint32_t)reverse8(b[6]) << 16) | ((uint32_t)reverse8(b[5]) << 8) |
                               (uint32_t)reverse8(b[4]);
        } else {
        }
        instance->generic.serial = instance->serial;

        flipper_format_rewind(flipper_format);
        uint32_t btn_temp;
        if(flipper_format_read_uint32(flipper_format, "Btn", &btn_temp, 1)) {
            instance->btn = (uint8_t)btn_temp;
        } else {
            uint8_t b7 = instance->generic.data & 0xFF;
            instance->btn = (reverse8(b7) & 0xF0) >> 4;
        }

        flipper_format_rewind(flipper_format);
        uint32_t cnt_temp;
        if(flipper_format_read_uint32(flipper_format, "Cnt", &cnt_temp, 1)) {
            instance->cnt = (uint16_t)cnt_temp;
        } else {
            flipper_format_rewind(flipper_format);
            uint32_t decrypted_temp;
            if(flipper_format_read_uint32(flipper_format, "Decrypted", &decrypted_temp, 1)) {
                instance->cnt = decrypted_temp & 0xFFFF;
            } else {
                instance->cnt = 0;
            }
        }

        flipper_format_rewind(flipper_format);
        uint32_t version_temp;
        if(flipper_format_read_uint32(flipper_format, "Version", &version_temp, 1)) {
            if(!version_from_protocol_name) {
                instance->version = (uint8_t)version_temp;
            }
        } else if(!version_from_protocol_name) {
            instance->version = 0;
        }

        flipper_format_rewind(flipper_format);
        if(!flipper_format_read_uint32(
               flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1)) {
            instance->encoder.repeat = 10;
        }

        if(subghz_custom_btn_get_original() == 0) {
            subghz_custom_btn_set_original(instance->btn);
        }
        subghz_custom_btn_set_max(5);

        uint8_t selected_btn;
        if(subghz_custom_btn_get() == SUBGHZ_CUSTOM_BTN_OK) {
            selected_btn = subghz_custom_btn_get_original();
        } else {
            selected_btn = subghz_custom_btn_get();
        }
        
        if(selected_btn == 5) {
            instance->btn = 0x8;
        } else if(selected_btn >= 1 && selected_btn <= 4) {
            instance->btn = selected_btn;
        }

        uint32_t mult = furi_hal_subghz_get_rolling_counter_mult();
        instance->cnt = (instance->cnt + mult) & 0xFFFF;

        instance->generic.btn = instance->btn;
        instance->generic.cnt = instance->cnt;

        subghz_protocol_encoder_kia_v3_v4_get_upload(instance);

        if(!flipper_format_rewind(flipper_format)) {
            ret = SubGhzProtocolStatusErrorParserOthers;
            break;
        }
        uint8_t key_data[sizeof(uint64_t)] = {0};
        for(size_t i = 0; i < sizeof(uint64_t); i++) {
            key_data[sizeof(uint64_t) - i - 1] = (instance->generic.data >> (i * 8)) & 0xFF;
        }
        if(!flipper_format_update_hex(flipper_format, "Key", key_data, sizeof(uint64_t))) {
        }

        if(!flipper_format_rewind(flipper_format)) {
            ret = SubGhzProtocolStatusErrorParserOthers;
            break;
        }
        uint32_t cnt_to_write = instance->cnt;
        if(!flipper_format_update_uint32(flipper_format, "Cnt", &cnt_to_write, 1)) {
        }

        if(!flipper_format_rewind(flipper_format)) {
            ret = SubGhzProtocolStatusErrorParserOthers;
            break;
        }
        uint32_t btn_to_write = instance->btn;
        if(!flipper_format_update_uint32(flipper_format, "Btn", &btn_to_write, 1)) {
        }

        if(!flipper_format_rewind(flipper_format)) {
            ret = SubGhzProtocolStatusErrorParserOthers;
            break;
        }
        uint32_t decrypted_to_write = instance->decrypted;
        if(!flipper_format_update_uint32(flipper_format, "Decrypted", &decrypted_to_write, 1)) {
        }

        if(!flipper_format_rewind(flipper_format)) {
            ret = SubGhzProtocolStatusErrorParserOthers;
            break;
        }
        uint32_t encrypted_to_write = instance->encrypted;
        if(!flipper_format_update_uint32(flipper_format, "Encrypted", &encrypted_to_write, 1)) {
        }

        if(!flipper_format_rewind(flipper_format)) {
            ret = SubGhzProtocolStatusErrorParserOthers;
            break;
        }
        uint32_t crc_to_write = instance->crc;
        if(!flipper_format_update_uint32(flipper_format, "CRC", &crc_to_write, 1)) {
        }

        instance->encoder.is_running = true;
        instance->encoder.front = 0;

        ret = SubGhzProtocolStatusOk;
    } while(false);

    return ret;
}

void subghz_protocol_encoder_kia_v3_v4_stop(void* context) {
    if(!context) return;
    SubGhzProtocolEncoderKiaV3V4* instance = context;
    instance->encoder.is_running = false;
    instance->encoder.front = 0;
}

LevelDuration subghz_protocol_encoder_kia_v3_v4_yield(void* context) {
    SubGhzProtocolEncoderKiaV3V4* instance = context;

    if(!instance || !instance->encoder.upload || instance->encoder.repeat == 0 ||
       !instance->encoder.is_running) {
        if(instance) {
            instance->encoder.is_running = false;
        }
        return level_duration_reset();
    }

    if(instance->encoder.front >= instance->encoder.size_upload) {
        instance->encoder.is_running = false;
        instance->encoder.front = 0;
        return level_duration_reset();
    }

    LevelDuration ret = instance->encoder.upload[instance->encoder.front];

    if(++instance->encoder.front == instance->encoder.size_upload) {
        instance->encoder.repeat--;
        instance->encoder.front = 0;
    }

    return ret;
}

void* subghz_protocol_decoder_kia_v3_v4_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderKiaV3V4* instance = malloc(sizeof(SubGhzProtocolDecoderKiaV3V4));
    instance->base.protocol = &subghz_protocol_kia_v3_v4;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->version = 0;
    instance->is_v3_sync = false;
    return instance;
}
void subghz_protocol_decoder_kia_v3_v4_free(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderKiaV3V4* instance = context;
    free(instance);
}

void subghz_protocol_decoder_kia_v3_v4_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderKiaV3V4* instance = context;
    instance->decoder.parser_step = KiaV3V4DecoderStepReset;
    instance->header_count = 0;
    instance->raw_bit_count = 0;
    instance->is_v3_sync = false;
    instance->crc = 0;
    memset(instance->raw_bits, 0, sizeof(instance->raw_bits));
}

void subghz_protocol_decoder_kia_v3_v4_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderKiaV3V4* instance = context;

    switch(instance->decoder.parser_step) {
    case KiaV3V4DecoderStepReset: 
        if(level && DURATION_DIFF(duration, subghz_protocol_kia_v3_v4_const.te_short) <
                        subghz_protocol_kia_v3_v4_const.te_delta) {
            instance->decoder.parser_step = KiaV3V4DecoderStepCheckPreamble;
            instance->decoder.te_last = duration;
            instance->header_count = 1;
        }
        break;

    case KiaV3V4DecoderStepCheckPreamble:
        if(level) {
            if(DURATION_DIFF(duration, subghz_protocol_kia_v3_v4_const.te_short) <
               subghz_protocol_kia_v3_v4_const.te_delta) {
                instance->decoder.te_last = duration;
                instance->header_count++;
            } else if(duration > 1000 && duration < 1500) {
                if(instance->header_count >= 8) {
                    instance->decoder.parser_step = KiaV3V4DecoderStepCollectRawBits;
                    instance->raw_bit_count = 0;
                    instance->is_v3_sync = false;
                    memset(instance->raw_bits, 0, sizeof(instance->raw_bits));
                } else {
                    instance->decoder.parser_step = KiaV3V4DecoderStepReset;
                }
            } else {
                instance->decoder.parser_step = KiaV3V4DecoderStepReset;
            }
        } else {
            if(duration > 1000 && duration < 1500) {
                if(instance->header_count >= 8) {
                    instance->decoder.parser_step = KiaV3V4DecoderStepCollectRawBits;
                    instance->raw_bit_count = 0;
                    instance->is_v3_sync = true;
                    memset(instance->raw_bits, 0, sizeof(instance->raw_bits));
                } else {
                    instance->decoder.parser_step = KiaV3V4DecoderStepReset;
                }
            } else if(
                DURATION_DIFF(duration, subghz_protocol_kia_v3_v4_const.te_short) <
                    subghz_protocol_kia_v3_v4_const.te_delta &&
                DURATION_DIFF(instance->decoder.te_last, subghz_protocol_kia_v3_v4_const.te_short) <
                    subghz_protocol_kia_v3_v4_const.te_delta) {
                instance->header_count++;
            } else if(duration > 1500) {
                instance->decoder.parser_step = KiaV3V4DecoderStepReset;
            }
        }
        break;

    case KiaV3V4DecoderStepCollectRawBits:
        if(level) {
            if(duration > 1000 && duration < 1500) {
                if(kia_v3_v4_process_buffer(instance)) {
                    if(instance->base.callback)
                        instance->base.callback(&instance->base, instance->base.context);
                }
                instance->decoder.parser_step = KiaV3V4DecoderStepReset;
            } else if(
                DURATION_DIFF(duration, subghz_protocol_kia_v3_v4_const.te_short) <
                subghz_protocol_kia_v3_v4_const.te_delta) {
                kia_v3_v4_add_raw_bit(instance, false);
            } else if(
                DURATION_DIFF(duration, subghz_protocol_kia_v3_v4_const.te_long) <
                subghz_protocol_kia_v3_v4_const.te_delta) {
                kia_v3_v4_add_raw_bit(instance, true);
            } else {
                instance->decoder.parser_step = KiaV3V4DecoderStepReset;
            }
        } else {
            if(duration > 1000 && duration < 1500) {
                if(kia_v3_v4_process_buffer(instance)) {
                    if(instance->base.callback)
                        instance->base.callback(&instance->base, instance->base.context);
                }
                instance->decoder.parser_step = KiaV3V4DecoderStepReset;
            } else if(duration > 1500) {
                if(kia_v3_v4_process_buffer(instance)) {
                    if(instance->base.callback)
                        instance->base.callback(&instance->base, instance->base.context);
                }
                instance->decoder.parser_step = KiaV3V4DecoderStepReset;
            }
        }
        break;
    }
}

uint8_t subghz_protocol_decoder_kia_v3_v4_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderKiaV3V4* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_kia_v3_v4_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderKiaV3V4* instance = context;

    SubGhzProtocolStatus ret = subghz_block_generic_serialize(&instance->generic, flipper_format, preset);

    if(ret == SubGhzProtocolStatusOk) {
        if(!flipper_format_write_uint32(flipper_format, "Encrypted", &instance->encrypted, 1)) {
            ret = SubGhzProtocolStatusErrorParserOthers;
        }
    }

    if(ret == SubGhzProtocolStatusOk) {
        if(!flipper_format_write_uint32(flipper_format, "Decrypted", &instance->decrypted, 1)) {
            ret = SubGhzProtocolStatusErrorParserOthers;
        }
    }

    if(ret == SubGhzProtocolStatusOk) {
        uint32_t temp = instance->version;
        if(!flipper_format_write_uint32(flipper_format, "Version", &temp, 1)) {
            ret = SubGhzProtocolStatusErrorParserOthers;
        }
    }

    if(ret == SubGhzProtocolStatusOk) {
        uint32_t temp = instance->crc;
        if(!flipper_format_write_uint32(flipper_format, "CRC", &temp, 1)) {
            ret = SubGhzProtocolStatusErrorParserOthers;
        }
    }

    return ret;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_kia_v3_v4_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderKiaV3V4* instance = context;
    
    SubGhzProtocolStatus ret = subghz_block_generic_deserialize(&instance->generic, flipper_format);

    if(ret == SubGhzProtocolStatusOk) {
        if(instance->generic.data_count_bit < 64) {
            ret = SubGhzProtocolStatusErrorParserBitCount;
        }
    }
    
    if(ret == SubGhzProtocolStatusOk) {
        if(!flipper_format_read_uint32(flipper_format, "Encrypted", &instance->encrypted, 1)) {
            instance->encrypted = 0;
        }
        
        if(!flipper_format_read_uint32(flipper_format, "Decrypted", &instance->decrypted, 1)) {
            instance->decrypted = 0;
        }
        
        uint32_t temp_version = 0;
        if(flipper_format_read_uint32(flipper_format, "Version", &temp_version, 1)) {
            instance->version = temp_version;
        } else {
            instance->version = 0;
        }
        
        uint32_t temp_crc = 0;
        if(flipper_format_read_uint32(flipper_format, "CRC", &temp_crc, 1)) {
            instance->crc = temp_crc;
        } else {
            instance->crc = 0;
        }
        
        if(instance->decrypted != 0) {
            instance->generic.btn = (instance->decrypted >> 28) & 0x0F;
            instance->generic.cnt = instance->decrypted & 0xFFFF;
        }
        
        if(instance->generic.data != 0) {
            uint8_t b[8];
            for(int i = 0; i < 8; i++) {
                b[i] = (instance->generic.data >> ((7-i) * 8)) & 0xFF;
            }
            
            instance->generic.serial = ((uint32_t)reverse8(b[7] & 0xF0) << 24) | 
                                      ((uint32_t)reverse8(b[6]) << 16) |
                                      ((uint32_t)reverse8(b[5]) << 8) | 
                                      (uint32_t)reverse8(b[4]);
        }

        if(subghz_custom_btn_get_original() == 0) {
            subghz_custom_btn_set_original(instance->generic.btn);
        }
        subghz_custom_btn_set_max(5);
    }
    
    return ret;
}

static uint64_t compute_yek(uint64_t key) {
    uint64_t yek = 0;
    for(int i = 0; i < 64; i++) {
        yek |= ((key >> i) & 1) << (63 - i);
    }
    return yek;
}

static bool kia_v3_v4_verify_crc_from_data(uint64_t data, uint8_t received_crc) {
    uint8_t bytes[8];
    for(int i = 0; i < 8; i++) {
        bytes[i] = (data >> ((7-i) * 8)) & 0xFF;
    }
    uint8_t calculated_crc = kia_v3_v4_calculate_crc(bytes);
    return (calculated_crc == received_crc);
}

void subghz_protocol_decoder_kia_v3_v4_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderKiaV3V4* instance = context;

    uint64_t yek = compute_yek(instance->generic.data);
    uint32_t key_hi = (uint32_t)(instance->generic.data >> 32);
    uint32_t key_lo = (uint32_t)(instance->generic.data & 0xFFFFFFFF);
    uint32_t yek_hi = (uint32_t)(yek >> 32);
    uint32_t yek_lo = (uint32_t)(yek & 0xFFFFFFFF);

    bool crc_valid = kia_v3_v4_verify_crc_from_data(instance->generic.data, instance->crc);

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Key:%08lX%08lX\r\n"
        "Yek:%08lX%08lX\r\n"
        "Sn:%07lX Btn:%X [%s]\r\n"
        "Dec:%08lX Cnt:%04lX\r\n"
        "CRC:%X %s",
        kia_version_names[instance->version],
        instance->generic.data_count_bit,
        key_hi,
        key_lo,
        yek_hi,
        yek_lo,
        instance->generic.serial,
        instance->generic.btn,
        subghz_protocol_kia_v3_v4_get_name_button(instance->generic.btn),
        instance->decrypted,
        instance->generic.cnt,
        instance->crc,
        crc_valid ? "(OK)" : "(FAIL)");
}
