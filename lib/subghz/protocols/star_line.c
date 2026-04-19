#include "star_line.h"
#include "keeloq_common.h"
#include <lib/subghz/subghz_keystore.h>
#include "../subghz_keystore_i.h"
#include "../blocks/custom_btn_i.h"

#define TAG "SubGhzProtocolStarLine"

static const char* star_line_btn_name(uint8_t btn) {
    switch(btn) {
    case 0x01: return "Lock";
    case 0x02: return "Unlock";
    case 0x03: return "Trunk";
    case 0x04: return "Panic";
    case 0x21: return "Lock";
    case 0x22: return "Unlock";
    case 0x23: return "Trunk";
    case 0x24: return "Start";
    case 0x25: return "Stop";
    case 0x26: return "Extra";
    default:   return "Unknown";
    }
}

static uint8_t star_line_btn_to_custom(uint8_t btn) {
    switch(btn) {
    case 0x01:
    case 0x21: return SUBGHZ_CUSTOM_BTN_UP;      // 1 = Lock
    case 0x02:
    case 0x22: return SUBGHZ_CUSTOM_BTN_DOWN;    // 2 = Unlock
    case 0x03:
    case 0x23: return SUBGHZ_CUSTOM_BTN_LEFT;    // 3 = Trunk
    case 0x04:
    case 0x24: return SUBGHZ_CUSTOM_BTN_RIGHT;   // 4 = Start/Panic
    default:   return SUBGHZ_CUSTOM_BTN_OK;
    }
}

static uint8_t star_line_custom_to_btn(uint8_t custom, uint8_t original_btn) {
    if(custom == SUBGHZ_CUSTOM_BTN_OK) return original_btn;

    bool is_twage = (original_btn & 0x20) != 0;

    switch(custom) {
    case SUBGHZ_CUSTOM_BTN_UP:    return is_twage ? 0x21 : 0x01;  // Lock
    case SUBGHZ_CUSTOM_BTN_DOWN:  return is_twage ? 0x22 : 0x02;  // Unlock
    case SUBGHZ_CUSTOM_BTN_LEFT:  return is_twage ? 0x23 : 0x03;  // Trunk
    case SUBGHZ_CUSTOM_BTN_RIGHT: return is_twage ? 0x24 : 0x04;  // Start/Panic
    default: return original_btn;
    }
}

//static uint8_t star_line_get_btn_code(uint8_t original_btn) {
//    uint8_t custom = subghz_custom_btn_get();
//    if(custom == SUBGHZ_CUSTOM_BTN_OK) return original_btn;
//    return star_line_custom_to_btn(custom, original_btn);
//}

static uint8_t star_line_get_btn_code(uint8_t original_btn) {
    uint8_t custom = subghz_custom_btn_get();
    bool is_twage  = (original_btn & 0x20) != 0;
    uint8_t page   = subghz_custom_btn_get_page();

    FURI_LOG_I(TAG, "get_btn_code: original=0x%02X custom=%u page=%u is_twage=%d",
        original_btn, custom, page, is_twage);

    if(subghz_custom_btn_get_long()) {
        subghz_custom_btn_set_long(false);
        switch(custom) {
        case SUBGHZ_CUSTOM_BTN_UP:    return is_twage ? 0x25 : 0x01;
        case SUBGHZ_CUSTOM_BTN_DOWN:  return is_twage ? 0x26 : 0x02;
        case SUBGHZ_CUSTOM_BTN_LEFT:  return is_twage ? 0x21 : 0x03;
        case SUBGHZ_CUSTOM_BTN_RIGHT: return is_twage ? 0x22 : 0x04;
        default: return original_btn;
        }
    }

    if(custom == SUBGHZ_CUSTOM_BTN_OK) return original_btn;

    if(!is_twage) {
        return star_line_custom_to_btn(custom, original_btn);
    }

    if(page == 0) {
        switch(custom) {
        case SUBGHZ_CUSTOM_BTN_UP:    return 0x21; // Lock
        case SUBGHZ_CUSTOM_BTN_DOWN:  return 0x22; // Unlock
        case SUBGHZ_CUSTOM_BTN_LEFT:  return 0x23; // Trunk
        case SUBGHZ_CUSTOM_BTN_RIGHT: return 0x24; // Start
        default: return original_btn;
        }
    } else {
        switch(custom) {
        case SUBGHZ_CUSTOM_BTN_UP:    return 0x25; // Stop
        case SUBGHZ_CUSTOM_BTN_DOWN:  return 0x26; // Extra
        case SUBGHZ_CUSTOM_BTN_LEFT:  return 0x21; // Lock
        case SUBGHZ_CUSTOM_BTN_RIGHT: return 0x22; // Unlock
        default: return original_btn;
        }
    }
}

static const SubGhzBlockConst subghz_protocol_star_line_const = {
    .te_short = 250,
    .te_long = 500,
    .te_delta = 120,
    .min_count_bit_for_found = 64,
};

struct SubGhzProtocolDecoderStarLine {
    SubGhzProtocolDecoderBase base;

    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    uint16_t header_count;
    SubGhzKeystore* keystore;
    const char* manufacture_name;

    FuriString* manufacture_from_file;
};

struct SubGhzProtocolEncoderStarLine {
    SubGhzProtocolEncoderBase base;

    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;

    SubGhzKeystore* keystore;
    const char* manufacture_name;

    FuriString* manufacture_from_file;
};

typedef enum {
    StarLineDecoderStepReset = 0,
    StarLineDecoderStepCheckPreambula,
    StarLineDecoderStepSaveDuration,
    StarLineDecoderStepCheckDuration,
} StarLineDecoderStep;

const SubGhzProtocolDecoder subghz_protocol_star_line_decoder = {
    .alloc = subghz_protocol_decoder_star_line_alloc,
    .free = subghz_protocol_decoder_star_line_free,

    .feed = subghz_protocol_decoder_star_line_feed,
    .reset = subghz_protocol_decoder_star_line_reset,

    .get_hash_data = subghz_protocol_decoder_star_line_get_hash_data,
    .serialize = subghz_protocol_decoder_star_line_serialize,
    .deserialize = subghz_protocol_decoder_star_line_deserialize,
    .get_string = subghz_protocol_decoder_star_line_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_star_line_encoder = {
    .alloc = subghz_protocol_encoder_star_line_alloc,
    .free = subghz_protocol_encoder_star_line_free,

    .deserialize = subghz_protocol_encoder_star_line_deserialize,
    .stop = subghz_protocol_encoder_star_line_stop,
    .yield = subghz_protocol_encoder_star_line_yield,
};

const SubGhzProtocol subghz_protocol_star_line = {
    .name = SUBGHZ_PROTOCOL_STAR_LINE_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send | SubGhzProtocolFlag_Automotive,

    .decoder = &subghz_protocol_star_line_decoder,
    .encoder = &subghz_protocol_star_line_encoder,
};

static void subghz_protocol_star_line_check_remote_controller(
    SubGhzBlockGeneric* instance,
    SubGhzKeystore* keystore,
    const char** manufacture_name);

void* subghz_protocol_encoder_star_line_alloc(SubGhzEnvironment* environment) {
    SubGhzProtocolEncoderStarLine* instance = malloc(sizeof(SubGhzProtocolEncoderStarLine));

    instance->base.protocol = &subghz_protocol_star_line;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->keystore = subghz_environment_get_keystore(environment);

    instance->manufacture_from_file = furi_string_alloc();

    instance->encoder.repeat = 40;
    instance->encoder.size_upload = 256;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;

    return instance;
}

void subghz_protocol_encoder_star_line_free(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderStarLine* instance = context;
    furi_string_free(instance->manufacture_from_file);
    free(instance->encoder.upload);
    free(instance);
}

static bool
    subghz_protocol_star_line_gen_data(SubGhzProtocolEncoderStarLine* instance, uint8_t btn) {

    if((instance->generic.cnt + 1) > 0xFFFF) {
        instance->generic.cnt = 0;
    } else {
        instance->generic.cnt += 1;
    }

    uint32_t fix = btn << 24 | instance->generic.serial;
    uint32_t decrypt = btn << 24 | (instance->generic.serial & 0xFF) << 16 | instance->generic.cnt;
    uint32_t hop = 0;
    uint64_t man = 0;
    uint64_t code_found_reverse;
    int res = 0;

    if(instance->manufacture_name == 0x0) {
        instance->manufacture_name = "";
    }

    if(strcmp(instance->manufacture_name, "Unknown") == 0) {
        code_found_reverse = subghz_protocol_blocks_reverse_key(
            instance->generic.data, instance->generic.data_count_bit);
        hop = code_found_reverse & 0x00000000ffffffff;
    } else {
        uint8_t kl_type_en = instance->keystore->kl_type;
        for
            M_EACH(
                manufacture_code,
                *subghz_keystore_get_data(instance->keystore),
                SubGhzKeyArray_t) {
                res = strcmp(
                    furi_string_get_cstr(manufacture_code->name), instance->manufacture_name);
                if(res == 0) {
                    switch(manufacture_code->type) {
                    case KEELOQ_LEARNING_SIMPLE:
                        hop =
                            subghz_protocol_keeloq_common_encrypt(decrypt, manufacture_code->key);
                        break;
                    case KEELOQ_LEARNING_NORMAL:
                        man = subghz_protocol_keeloq_common_normal_learning(
                            fix, manufacture_code->key);
                        hop = subghz_protocol_keeloq_common_encrypt(decrypt, man);
                        break;
                    case KEELOQ_LEARNING_UNKNOWN:
                        if(kl_type_en == 1) {
                            hop = subghz_protocol_keeloq_common_encrypt(
                                decrypt, manufacture_code->key);
                        }
                        if(kl_type_en == 2) {
                            man = subghz_protocol_keeloq_common_normal_learning(
                                fix, manufacture_code->key);
                            hop = subghz_protocol_keeloq_common_encrypt(decrypt, man);
                        }
                        break;
                    }
                    break;
                }
            }
    }
    if(hop) {
        uint64_t yek = (uint64_t)fix << 32 | hop;
        instance->generic.data =
            subghz_protocol_blocks_reverse_key(yek, instance->generic.data_count_bit);
        return true;
    } else {
        instance->manufacture_name = "Unknown";
        return false;
    }
}

bool subghz_protocol_star_line_create_data(
    void* context,
    FlipperFormat* flipper_format,
    uint32_t serial,
    uint8_t btn,
    uint16_t cnt,
    const char* manufacture_name,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolEncoderStarLine* instance = context;
    instance->generic.serial = serial;
    instance->generic.cnt = cnt;
    instance->manufacture_name = manufacture_name;
    instance->generic.data_count_bit = 64;
    bool res = subghz_protocol_star_line_gen_data(instance, btn);
    if(res) {
        return SubGhzProtocolStatusOk ==
               subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    }
    return res;
}

static bool subghz_protocol_encoder_star_line_get_upload(
    SubGhzProtocolEncoderStarLine* instance,
    uint8_t btn) {
    furi_check(instance);

    if(!subghz_protocol_star_line_gen_data(instance, btn)) {
        return false;
    }

    size_t index = 0;
    size_t size_upload = 6 * 2 + (instance->generic.data_count_bit * 2);
    if(size_upload > instance->encoder.size_upload) {
        FURI_LOG_E(TAG, "Size upload exceeds allocated encoder buffer.");
        return false;
    } else {
        instance->encoder.size_upload = size_upload;
    }

    for(uint8_t i = 6; i > 0; i--) {
        instance->encoder.upload[index++] =
            level_duration_make(true, (uint32_t)subghz_protocol_star_line_const.te_long * 2);
        instance->encoder.upload[index++] =
            level_duration_make(false, (uint32_t)subghz_protocol_star_line_const.te_long * 2);
    }

    for(uint8_t i = instance->generic.data_count_bit; i > 0; i--) {
        if(bit_read(instance->generic.data, i - 1)) {
            instance->encoder.upload[index++] =
                level_duration_make(true, (uint32_t)subghz_protocol_star_line_const.te_long);
            instance->encoder.upload[index++] =
                level_duration_make(false, (uint32_t)subghz_protocol_star_line_const.te_long);
        } else {
            instance->encoder.upload[index++] =
                level_duration_make(true, (uint32_t)subghz_protocol_star_line_const.te_short);
            instance->encoder.upload[index++] =
                level_duration_make(false, (uint32_t)subghz_protocol_star_line_const.te_short);
        }
    }

    return true;
}

static SubGhzProtocolStatus subghz_protocol_encoder_star_line_serialize(
    SubGhzProtocolEncoderStarLine* instance,
    FlipperFormat* flipper_format) {
    subghz_protocol_star_line_check_remote_controller(
        &instance->generic, instance->keystore, &instance->manufacture_name);

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

        if(!flipper_format_insert_or_update_string_cstr(
               flipper_format, "Manufacture", instance->manufacture_name)) {
            break;
        }

        ret = SubGhzProtocolStatusOk;
    } while(false);

    return ret;
}

SubGhzProtocolStatus
    subghz_protocol_encoder_star_line_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolEncoderStarLine* instance = context;
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
            FURI_LOG_E(
                TAG,
                "Wrong protocol %s != %s",
                furi_string_get_cstr(temp_str),
                instance->base.protocol->name);
            furi_string_free(temp_str);
            break;
        }
        furi_string_free(temp_str);

        uint32_t bit_count_temp;
        if(!flipper_format_read_uint32(flipper_format, "Bit", &bit_count_temp, 1)) {
            FURI_LOG_E(TAG, "Missing Bit");
            break;
        }

        instance->generic.data_count_bit = subghz_protocol_star_line_const.min_count_bit_for_found;

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

        if(hex_pos != 16) {
            FURI_LOG_E(TAG, "Invalid key length: %zu nibbles (expected 16)", hex_pos);
            break;
        }

        instance->generic.data = key;
        FURI_LOG_I(TAG, "Parsed key: 0x%016llX", instance->generic.data);

        if(instance->generic.data == 0) {
            FURI_LOG_E(TAG, "Key is zero after parsing!");
            break;
        }

        if(!flipper_format_read_uint32(flipper_format, "Serial", &instance->generic.serial, 1)) {
            instance->generic.serial = instance->generic.data >> 24;
            FURI_LOG_I(TAG, "Extracted serial: 0x%08lX", instance->generic.serial);
        } else {
            FURI_LOG_I(TAG, "Read serial: 0x%08lX", instance->generic.serial);
        }

        uint32_t btn_temp;
        if(flipper_format_read_uint32(flipper_format, "Btn", &btn_temp, 1)) {
            instance->generic.btn = (uint8_t)btn_temp;
            FURI_LOG_I(TAG, "Read button: 0x%02X", instance->generic.btn);
        } else {
            instance->generic.btn = (instance->generic.data >> 16) & 0xFF;
            FURI_LOG_I(TAG, "Extracted button: 0x%02X", instance->generic.btn);
        }

        subghz_custom_btn_set_original(star_line_btn_to_custom(instance->generic.btn));
        subghz_custom_btn_set_max(4);
        subghz_custom_btn_set_pages(true);
        subghz_custom_btn_set_max_pages(2);

        uint32_t cnt_temp;
        if(flipper_format_read_uint32(flipper_format, "Cnt", &cnt_temp, 1)) {
            instance->generic.cnt = (uint16_t)cnt_temp;
            FURI_LOG_I(TAG, "Read counter: 0x%03lX", (unsigned long)instance->generic.cnt);
        }

        if(!flipper_format_read_uint32(
               flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1)) {
            instance->encoder.repeat = 40;
            FURI_LOG_D(
                TAG, "Repeat not found in file, using default 40 for continuous transmission");
        }
        if(!flipper_format_rewind(flipper_format)) {
            FURI_LOG_E(TAG, "Rewind error");
            break;
        }
        if(flipper_format_read_string(
               flipper_format, "Manufacture", instance->manufacture_from_file)) {
            instance->manufacture_name = furi_string_get_cstr(instance->manufacture_from_file);
            instance->keystore->mfname = instance->manufacture_name;
        } else {
            FURI_LOG_D(TAG, "ENCODER: Missing Manufacture");
        }

        subghz_protocol_star_line_check_remote_controller(
            &instance->generic, instance->keystore, &instance->manufacture_name);

        uint8_t selected_btn = star_line_get_btn_code(instance->generic.btn);
        subghz_protocol_encoder_star_line_get_upload(instance, selected_btn);

        subghz_protocol_encoder_star_line_serialize(instance, flipper_format);

        instance->encoder.is_running = true;

        FURI_LOG_I(
            TAG,
            "Encoder deserialized: repeat=%u, size_upload=%zu, is_running=%d, front=%zu",
            instance->encoder.repeat,
            instance->encoder.size_upload,
            instance->encoder.is_running,
            instance->encoder.front);

        ret = SubGhzProtocolStatusOk;
    } while(false);

    return ret;
}

void subghz_protocol_encoder_star_line_stop(void* context) {
    SubGhzProtocolEncoderStarLine* instance = context;
    instance->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_star_line_yield(void* context) {
    SubGhzProtocolEncoderStarLine* instance = context;

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

void* subghz_protocol_decoder_star_line_alloc(SubGhzEnvironment* environment) {
    SubGhzProtocolDecoderStarLine* instance = malloc(sizeof(SubGhzProtocolDecoderStarLine));
    instance->base.protocol = &subghz_protocol_star_line;
    instance->generic.protocol_name = instance->base.protocol->name;

    instance->manufacture_from_file = furi_string_alloc();

    instance->keystore = subghz_environment_get_keystore(environment);

    return instance;
}

void subghz_protocol_decoder_star_line_free(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderStarLine* instance = context;
    furi_string_free(instance->manufacture_from_file);

    free(instance);
}

void subghz_protocol_decoder_star_line_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderStarLine* instance = context;
    instance->decoder.parser_step = StarLineDecoderStepReset;
    subghz_keystore_reset_kl(instance->keystore);
}

void subghz_protocol_decoder_star_line_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderStarLine* instance = context;

    switch(instance->decoder.parser_step) {
    case StarLineDecoderStepReset:
        if(level) {
            if(DURATION_DIFF(duration, subghz_protocol_star_line_const.te_long * 2) <
               subghz_protocol_star_line_const.te_delta * 2) {
                instance->decoder.parser_step = StarLineDecoderStepCheckPreambula;
                instance->header_count++;
            } else if(instance->header_count > 4) {
                instance->decoder.decode_data = 0;
                instance->decoder.decode_count_bit = 0;
                instance->decoder.te_last = duration;
                instance->decoder.parser_step = StarLineDecoderStepCheckDuration;
            }
        } else {
            instance->header_count = 0;
        }
        break;
    case StarLineDecoderStepCheckPreambula:
        if((!level) && (DURATION_DIFF(duration, subghz_protocol_star_line_const.te_long * 2) <
                        subghz_protocol_star_line_const.te_delta * 2)) {
            instance->decoder.parser_step = StarLineDecoderStepReset;
        } else {
            instance->header_count = 0;
            instance->decoder.parser_step = StarLineDecoderStepReset;
        }
        break;
    case StarLineDecoderStepSaveDuration:
        if(level) {
            if(duration >= (subghz_protocol_star_line_const.te_long +
                            subghz_protocol_star_line_const.te_delta)) {
                instance->decoder.parser_step = StarLineDecoderStepReset;
                if((instance->decoder.decode_count_bit >=
                    subghz_protocol_star_line_const.min_count_bit_for_found) &&
                   (instance->decoder.decode_count_bit <=
                    subghz_protocol_star_line_const.min_count_bit_for_found + 2)) {
                    if(instance->generic.data != instance->decoder.decode_data) {
                        instance->generic.data = instance->decoder.decode_data;
                        instance->generic.data_count_bit =
                            subghz_protocol_star_line_const.min_count_bit_for_found;
                        if(instance->base.callback)
                            instance->base.callback(&instance->base, instance->base.context);
                    }
                }
                instance->decoder.decode_data = 0;
                instance->decoder.decode_count_bit = 0;
                instance->header_count = 0;
                break;
            } else {
                instance->decoder.te_last = duration;
                instance->decoder.parser_step = StarLineDecoderStepCheckDuration;
            }

        } else {
            instance->decoder.parser_step = StarLineDecoderStepReset;
        }
        break;
    case StarLineDecoderStepCheckDuration:
        if(!level) {
            if((DURATION_DIFF(instance->decoder.te_last, subghz_protocol_star_line_const.te_short) <
                subghz_protocol_star_line_const.te_delta) &&
               (DURATION_DIFF(duration, subghz_protocol_star_line_const.te_short) <
                subghz_protocol_star_line_const.te_delta)) {
                if(instance->decoder.decode_count_bit <
                   subghz_protocol_star_line_const.min_count_bit_for_found) {
                    subghz_protocol_blocks_add_bit(&instance->decoder, 0);
                } else {
                    instance->decoder.decode_count_bit++;
                }
                instance->decoder.parser_step = StarLineDecoderStepSaveDuration;
            } else if(
                (DURATION_DIFF(instance->decoder.te_last, subghz_protocol_star_line_const.te_long) <
                 subghz_protocol_star_line_const.te_delta) &&
                (DURATION_DIFF(duration, subghz_protocol_star_line_const.te_long) <
                 subghz_protocol_star_line_const.te_delta)) {
                if(instance->decoder.decode_count_bit <
                   subghz_protocol_star_line_const.min_count_bit_for_found) {
                    subghz_protocol_blocks_add_bit(&instance->decoder, 1);
                } else {
                    instance->decoder.decode_count_bit++;
                }
                instance->decoder.parser_step = StarLineDecoderStepSaveDuration;
            } else {
                instance->decoder.parser_step = StarLineDecoderStepReset;
            }
        } else {
            instance->decoder.parser_step = StarLineDecoderStepReset;
        }
        break;
    }
}

static inline bool subghz_protocol_star_line_check_decrypt(
    SubGhzBlockGeneric* instance,
    uint32_t decrypt,
    uint8_t btn,
    uint32_t end_serial) {
    furi_check(instance);
    if((decrypt >> 24 == btn) && ((((uint16_t)(decrypt >> 16)) & 0x00FF) == end_serial)) {
        instance->cnt = decrypt & 0x0000FFFF;
        return true;
    }
    return false;
}

static uint8_t subghz_protocol_star_line_check_remote_controller_selector(
    SubGhzBlockGeneric* instance,
    uint32_t fix,
    uint32_t hop,
    SubGhzKeystore* keystore,
    const char** manufacture_name) {
    uint16_t end_serial = (uint16_t)(fix & 0xFF);
    uint8_t btn = (uint8_t)(fix >> 24);
    uint32_t decrypt = 0;
    uint64_t man_normal_learning;
    bool mf_not_set = false;
    if(keystore->mfname == NULL) {
        subghz_keystore_reset_kl(keystore);
    }

    const char* mfname = keystore->mfname;

    if(strcmp(mfname, "Unknown") == 0) {
        return 1;
    } else if(strcmp(mfname, "") == 0) {
        mf_not_set = true;
    }
    for
        M_EACH(manufacture_code, *subghz_keystore_get_data(keystore), SubGhzKeyArray_t) {
            if(mf_not_set || (strcmp(furi_string_get_cstr(manufacture_code->name), mfname) == 0)) {
                switch(manufacture_code->type) {
                case KEELOQ_LEARNING_SIMPLE:
                    decrypt = subghz_protocol_keeloq_common_decrypt(hop, manufacture_code->key);
                    if(subghz_protocol_star_line_check_decrypt(
                           instance, decrypt, btn, end_serial)) {
                        *manufacture_name = furi_string_get_cstr(manufacture_code->name);
                        keystore->mfname = *manufacture_name;
                        return 1;
                    }
                    break;
                case KEELOQ_LEARNING_NORMAL:
                    man_normal_learning =
                        subghz_protocol_keeloq_common_normal_learning(fix, manufacture_code->key);
                    decrypt = subghz_protocol_keeloq_common_decrypt(hop, man_normal_learning);
                    if(subghz_protocol_star_line_check_decrypt(
                           instance, decrypt, btn, end_serial)) {
                        *manufacture_name = furi_string_get_cstr(manufacture_code->name);
                        keystore->mfname = *manufacture_name;
                        return 1;
                    }
                    break;
                case KEELOQ_LEARNING_UNKNOWN:
                    decrypt = subghz_protocol_keeloq_common_decrypt(hop, manufacture_code->key);
                    if(subghz_protocol_star_line_check_decrypt(
                           instance, decrypt, btn, end_serial)) {
                        *manufacture_name = furi_string_get_cstr(manufacture_code->name);
                        keystore->mfname = *manufacture_name;
                        keystore->kl_type = 1;
                        return 1;
                    }
                    uint64_t man_rev = 0;
                    uint64_t man_rev_byte = 0;
                    for(uint8_t i = 0; i < 64; i += 8) {
                        man_rev_byte = (uint8_t)(manufacture_code->key >> i);
                        man_rev = man_rev | man_rev_byte << (56 - i);
                    }
                    decrypt = subghz_protocol_keeloq_common_decrypt(hop, man_rev);
                    if(subghz_protocol_star_line_check_decrypt(
                           instance, decrypt, btn, end_serial)) {
                        *manufacture_name = furi_string_get_cstr(manufacture_code->name);
                        keystore->mfname = *manufacture_name;
                        keystore->kl_type = 1;
                        return 1;
                    }
                    man_normal_learning =
                        subghz_protocol_keeloq_common_normal_learning(fix, manufacture_code->key);
                    decrypt = subghz_protocol_keeloq_common_decrypt(hop, man_normal_learning);
                    if(subghz_protocol_star_line_check_decrypt(
                           instance, decrypt, btn, end_serial)) {
                        *manufacture_name = furi_string_get_cstr(manufacture_code->name);
                        keystore->mfname = *manufacture_name;
                        keystore->kl_type = 2;
                        return 1;
                    }
                    man_normal_learning =
                        subghz_protocol_keeloq_common_normal_learning(fix, man_rev);
                    decrypt = subghz_protocol_keeloq_common_decrypt(hop, man_normal_learning);
                    if(subghz_protocol_star_line_check_decrypt(
                           instance, decrypt, btn, end_serial)) {
                        *manufacture_name = furi_string_get_cstr(manufacture_code->name);
                        keystore->mfname = *manufacture_name;
                        keystore->kl_type = 2;
                        return 1;
                    }
                    break;
                }
            }
        }

    *manufacture_name = "Unknown";
    keystore->mfname = "Unknown";
    instance->cnt = 0;

    return 0;
}

static void subghz_protocol_star_line_check_remote_controller(
    SubGhzBlockGeneric* instance,
    SubGhzKeystore* keystore,
    const char** manufacture_name) {
    uint64_t key = subghz_protocol_blocks_reverse_key(instance->data, instance->data_count_bit);
    uint32_t key_fix = key >> 32;
    uint32_t key_hop = key & 0x00000000ffffffff;

    subghz_protocol_star_line_check_remote_controller_selector(
        instance, key_fix, key_hop, keystore, manufacture_name);

    instance->serial = key_fix & 0x00FFFFFF;
    instance->btn = key_fix >> 24;
}

uint8_t subghz_protocol_decoder_star_line_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderStarLine* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_star_line_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderStarLine* instance = context;
    subghz_protocol_star_line_check_remote_controller(
        &instance->generic, instance->keystore, &instance->manufacture_name);

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

        if(!flipper_format_insert_or_update_string_cstr(
               flipper_format, "Manufacture", instance->manufacture_name)) {
            break;
        }

        ret = SubGhzProtocolStatusOk;
    } while(false);

    return ret;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_star_line_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderStarLine* instance = context;
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
            FURI_LOG_E(
                TAG,
                "Wrong protocol %s != %s",
                furi_string_get_cstr(temp_str),
                instance->base.protocol->name);
            furi_string_free(temp_str);
            break;
        }
        furi_string_free(temp_str);

        uint32_t bit_count_temp;
        if(!flipper_format_read_uint32(flipper_format, "Bit", &bit_count_temp, 1)) {
            FURI_LOG_E(TAG, "Missing Bit");
            break;
        }

        instance->generic.data_count_bit = subghz_protocol_star_line_const.min_count_bit_for_found;

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

        if(hex_pos != 16) {
            FURI_LOG_E(TAG, "Invalid key length: %zu nibbles (expected 16)", hex_pos);
            break;
        }

        instance->generic.data = key;
        FURI_LOG_I(TAG, "Parsed key: 0x%016llX", instance->generic.data);

        if(instance->generic.data == 0) {
            FURI_LOG_E(TAG, "Key is zero after parsing!");
            break;
        }

        if(!flipper_format_read_uint32(flipper_format, "Serial", &instance->generic.serial, 1)) {
            instance->generic.serial = instance->generic.data >> 24;
            FURI_LOG_I(TAG, "Extracted serial: 0x%08lX", instance->generic.serial);
        } else {
            FURI_LOG_I(TAG, "Read serial: 0x%08lX", instance->generic.serial);
        }

        uint32_t btn_temp;
        if(flipper_format_read_uint32(flipper_format, "Btn", &btn_temp, 1)) {
            instance->generic.btn = (uint8_t)btn_temp;
            FURI_LOG_I(TAG, "Read button: 0x%02X", instance->generic.btn);
        } else {
            instance->generic.btn = (instance->generic.data >> 16) & 0xFF;
            FURI_LOG_I(TAG, "Extracted button: 0x%02X", instance->generic.btn);
        }

        uint32_t cnt_temp;
        if(flipper_format_read_uint32(flipper_format, "Cnt", &cnt_temp, 1)) {
            instance->generic.cnt = (uint16_t)cnt_temp;
            FURI_LOG_I(TAG, "Read counter: 0x%03lX", (unsigned long)instance->generic.cnt);
        }

        if(!flipper_format_rewind(flipper_format)) {
            FURI_LOG_E(TAG, "Rewind error");
            break;
        }

        if(flipper_format_read_string(
               flipper_format, "Manufacture", instance->manufacture_from_file)) {
            instance->manufacture_name = furi_string_get_cstr(instance->manufacture_from_file);
            instance->keystore->mfname = instance->manufacture_name;
        } else {
            FURI_LOG_D(TAG, "DECODER: Missing Manufacture");
        }

        FURI_LOG_I(TAG, "Decoder deserialized");

        ret = SubGhzProtocolStatusOk;
    } while(false);

    return ret;
}

//void subghz_protocol_decoder_star_line_get_string(void* context, FuriString* output) {
//    furi_check(context);
//    SubGhzProtocolDecoderStarLine* instance = context;

//    subghz_protocol_star_line_check_remote_controller(
//        &instance->generic, instance->keystore, &instance->manufacture_name);

//    subghz_custom_btn_set_original(star_line_btn_to_custom(instance->generic.btn));
//    subghz_custom_btn_set_max(4);

//    uint32_t code_found_hi = instance->generic.data >> 32;
//    uint32_t code_found_lo = instance->generic.data & 0x00000000ffffffff;

//    uint64_t code_found_reverse = subghz_protocol_blocks_reverse_key(
//        instance->generic.data, instance->generic.data_count_bit);
//    uint32_t code_found_reverse_hi = code_found_reverse >> 32;
//    uint32_t code_found_reverse_lo = code_found_reverse & 0x00000000ffffffff;

//    uint8_t display_btn;
//    uint8_t custom = subghz_custom_btn_get();
//    if(custom == SUBGHZ_CUSTOM_BTN_OK) {
//        display_btn = instance->generic.btn;
//    } else {
//        display_btn = star_line_custom_to_btn(custom, instance->generic.btn);
//    }

//    furi_string_cat_printf(
//        output,
//        "%s %dbit\r\n"
//        "Key:%08lX%08lX\r\n"
//        "Fix:0x%08lX\r\n"
//        "Hop:0x%08lX\r\n"
//        "Btn:[%s] Cnt:%04lX\r\n",
        //"MF:%s\r\n",
//        instance->generic.protocol_name,
//        instance->generic.data_count_bit,
//        code_found_hi,
//        code_found_lo,
//        code_found_reverse_hi,
//        code_found_reverse_lo,
//        star_line_btn_name(display_btn),
//        instance->generic.cnt);
        //instance->manufacture_name);

//}


void subghz_protocol_decoder_star_line_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderStarLine* instance = context;

    subghz_protocol_star_line_check_remote_controller(
        &instance->generic, instance->keystore, &instance->manufacture_name);

    subghz_custom_btn_set_original(star_line_btn_to_custom(instance->generic.btn));
    subghz_custom_btn_set_max(4);
    subghz_custom_btn_set_pages(true);
        subghz_custom_btn_set_max_pages(2);

    uint32_t code_found_hi = instance->generic.data >> 32;
    uint32_t code_found_lo = instance->generic.data & 0x00000000ffffffff;

    uint64_t code_found_reverse = subghz_protocol_blocks_reverse_key(
        instance->generic.data, instance->generic.data_count_bit);
    uint32_t code_found_reverse_hi = code_found_reverse >> 32;
    uint32_t code_found_reverse_lo = code_found_reverse & 0x00000000ffffffff;

    uint8_t display_btn;
    uint8_t custom = subghz_custom_btn_get();
    if(custom == SUBGHZ_CUSTOM_BTN_OK) {
        display_btn = instance->generic.btn;
    } else {
        display_btn = star_line_custom_to_btn(custom, instance->generic.btn);
    }

    bool is_twage = (instance->generic.btn & 0x20) != 0;

    if(is_twage) {
        furi_string_cat_printf(
            output,
            "%s %dbit\r\n"
            "Key:%08lX%08lX\r\n"
            "Fix:0x%08lX\r\n"
            "Hop:0x%08lX\r\n"
            "Btn:[%s] Cnt:%04lX\r\n",
            instance->generic.protocol_name,
            instance->generic.data_count_bit,
            code_found_hi,
            code_found_lo,
            code_found_reverse_hi,
            code_found_reverse_lo,
            star_line_btn_name(display_btn),
            instance->generic.cnt);
    } else {
        // Classic: only 4 buttons
        furi_string_cat_printf(
            output,
            "%s %dbit\r\n"
            "Key:%08lX%08lX\r\n"
            "Fix:0x%08lX\r\n"
            "Hop:0x%08lX\r\n"
            "Btn:[%s] Cnt:%04lX\r\n",
            instance->generic.protocol_name,
            instance->generic.data_count_bit,
            code_found_hi,
            code_found_lo,
            code_found_reverse_hi,
            code_found_reverse_lo,
            star_line_btn_name(display_btn),
            instance->generic.cnt);
    }
}
