#include "psa.h"

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"
#include "../blocks/custom_btn_i.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "SubGhzProtocolPSA"

static const SubGhzBlockConst subghz_protocol_psa_const = {
    .te_short = 250,
    .te_long = 500,
    .te_delta = 100,
    .min_count_bit_for_found = 128,
};

#define PSA_TE_SHORT_125 0x7d
#define PSA_TE_LONG_250 0xfa
#define PSA_TE_END_1000 1000
#define PSA_TE_END_500 500
#define PSA_TOLERANCE_99 99
#define PSA_TOLERANCE_100 100
#define PSA_TOLERANCE_49 0x31
#define PSA_TOLERANCE_50 0x32
#define PSA_PATTERN_THRESHOLD_1 0x46
#define PSA_PATTERN_THRESHOLD_2 0x45
#define PSA_MAX_BITS 0x79
#define PSA_KEY1_BITS 0x40
#define PSA_KEY2_BITS 0x50

#define TEA_DELTA 0x9E3779B9U
#define TEA_ROUNDS 32

#define PSA_BF1_CONST_U4 0x0E0F5C41U
#define PSA_BF1_CONST_U5 0x0F5C4123U

static const uint32_t PSA_BF1_KEY_SCHEDULE[4] = {
    0x4A434915U,
    0xD6743C2BU,
    0x1F29D308U,
    0xE6B79A64U,
};

static const uint32_t PSA_BF2_KEY_SCHEDULE[4] = {
    0x4039C240U,
    0xEDA92CABU,
    0x4306C02AU,
    0x02192A04U,
};

#define PSA_BF1_START 0x23000000U
#define PSA_BF1_END 0x24000000U
#define PSA_BF2_START 0xF3000000U
#define PSA_BF2_END 0xF4000000U

// Custom button mapping functions
static const char* psa_button_name(uint8_t btn) {
    switch(btn) {
    case 0x0:
        return "Lock";
    case 0x1:
        return "Unlock";
    case 0x2:
        return "Trunk";
    default:
        return "??";
    }
}

static uint8_t psa_get_btn_code(void) {
    uint8_t custom_btn = subghz_custom_btn_get();
    uint8_t original_raw = subghz_custom_btn_get_original();
    // 0xFF is sentinel for PSA btn 0x0 (Lock)
    uint8_t original_btn = (original_raw == 0xFF) ? 0x0 : original_raw;
    if(custom_btn == SUBGHZ_CUSTOM_BTN_OK)    return original_btn;
    if(custom_btn == SUBGHZ_CUSTOM_BTN_UP)    return 0x0; // Lock
    if(custom_btn == SUBGHZ_CUSTOM_BTN_DOWN)  return 0x1; // Unlock
    if(custom_btn == SUBGHZ_CUSTOM_BTN_LEFT)  return 0x2; // Trunk
    if(custom_btn == SUBGHZ_CUSTOM_BTN_RIGHT) return 0x2; // Trunk
    return original_btn;
}

typedef enum {
    PSADecoderState0 = 0,
    PSADecoderState1 = 1,
    PSADecoderState2 = 2,
    PSADecoderState3 = 3,
    PSADecoderState4 = 4,
} PSADecoderState;

struct SubGhzProtocolDecoderPSA {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    uint32_t state;
    uint32_t prev_duration;

    uint32_t decode_data_low;
    uint32_t decode_data_high;
    uint8_t decode_count_bit;

    uint32_t key1_low;
    uint32_t key1_high;
    uint16_t validation_field;
    uint32_t key2_low;
    uint32_t key2_high;

    uint32_t status_flag;
    uint16_t decrypted;
    uint8_t mode_serialize;

    uint8_t decrypted_button;
    uint32_t decrypted_serial;
    uint32_t decrypted_counter;
    uint16_t decrypted_crc;
    uint32_t decrypted_seed;
    uint8_t decrypted_type;

    uint16_t pattern_counter;
    ManchesterState manchester_state;

    uint32_t last_key1_low;
    uint32_t last_key1_high;

    uint32_t te_sum;
    uint16_t te_count;
    uint32_t te_detected;
};

struct SubGhzProtocolEncoderPSA {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;

    uint32_t key1_low;
    uint32_t key1_high;
    uint16_t validation_field;
    uint32_t key2_low;
    uint32_t counter;
    uint8_t button;
    uint8_t type;
    uint8_t seed;
    uint8_t mode;
    uint32_t serial;
    uint16_t crc;
    bool is_running;
};

const SubGhzProtocolDecoder subghz_protocol_psa_decoder = {
    .alloc = subghz_protocol_decoder_psa_alloc,
    .free = subghz_protocol_decoder_psa_free,
    .feed = subghz_protocol_decoder_psa_feed,
    .reset = subghz_protocol_decoder_psa_reset,
    .get_hash_data = subghz_protocol_decoder_psa_get_hash_data,
    .serialize = subghz_protocol_decoder_psa_serialize,
    .deserialize = subghz_protocol_decoder_psa_deserialize,
    .get_string = subghz_protocol_decoder_psa_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_psa_encoder = {
    .alloc = subghz_protocol_encoder_psa_alloc,
    .free = subghz_protocol_encoder_psa_free,
    .deserialize = subghz_protocol_encoder_psa_deserialize,
    .stop = subghz_protocol_encoder_psa_stop,
    .yield = subghz_protocol_encoder_psa_yield,
};

const SubGhzProtocol subghz_protocol_psa = {
    .name = SUBGHZ_PROTOCOL_PSA_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send | SubGhzProtocolFlag_Automotive,
    .decoder = &subghz_protocol_psa_decoder,
    .encoder = &subghz_protocol_psa_encoder,
};

static uint32_t psa_abs_diff(uint32_t a, uint32_t b) {
    if(a < b) {
        return b - a;
    } else {
        return a - b;
    }
}

static void psa_setup_byte_buffer(uint8_t* buffer, uint32_t key1_low, uint32_t key1_high, uint32_t key2_low) {
    for(int i = 0; i < 8; i++) {
        int shift = i * 8;
        uint8_t byte_val;
        if(shift < 32) {
            byte_val = (uint8_t)((key1_low >> shift) & 0xFF);
        } else {
            byte_val = (uint8_t)((key1_high >> (shift - 32)) & 0xFF);
        }
        buffer[7 - i] = byte_val;
    }
    buffer[9] = (uint8_t)(key2_low & 0xFF);
    buffer[8] = (uint8_t)((key2_low >> 8) & 0xFF);
}

static void psa_calculate_checksum(uint8_t* buffer) {
    uint32_t checksum = 0;
    for(int i = 2; i < 8; i++) {
        checksum += (buffer[i] & 0xF) + ((buffer[i] >> 4) & 0xF);
    }
    buffer[11] = (uint8_t)((checksum * 0x10) & 0xFF);
}

static void psa_copy_reverse(uint8_t* temp, uint8_t* source) {
    temp[0] = source[5];
    temp[1] = source[4];
    temp[2] = source[3];
    temp[3] = source[2];
    temp[4] = source[9];
    temp[5] = source[8];
    temp[6] = source[7];
    temp[7] = source[6];
}

static void psa_second_stage_xor_decrypt(uint8_t* buffer) {
    uint8_t temp[8];
    psa_copy_reverse(temp, buffer);
    buffer[2] = temp[0] ^ temp[6];
    buffer[3] = temp[2] ^ temp[0];
    buffer[4] = temp[6] ^ temp[3];
    buffer[5] = temp[7] ^ temp[1];
    buffer[6] = temp[3] ^ temp[1];
    buffer[7] = temp[6] ^ temp[4] ^ temp[5];
}

static void psa_second_stage_xor_encrypt(uint8_t* buffer) {
    uint8_t E6 = buffer[8];
    uint8_t E7 = buffer[9];
    
    uint8_t P[6];
    P[0] = buffer[2];
    P[1] = buffer[3];
    P[2] = buffer[4];
    P[3] = buffer[5];
    P[4] = buffer[6];
    P[5] = buffer[7];
    
    uint8_t E5 = P[5] ^ E7 ^ E6;
    uint8_t E0 = P[2] ^ E5;
    uint8_t E2 = P[4] ^ E0;
    uint8_t E4 = P[3] ^ E2;
    uint8_t E3 = P[0] ^ E5;
    uint8_t E1 = P[1] ^ E3;
    
    buffer[2] = E0;
    buffer[3] = E1;
    buffer[4] = E2;
    buffer[5] = E3;
    buffer[6] = E4;
    buffer[7] = E5;
}

__attribute__((optimize("O3"), always_inline)) static inline void
    psa_tea_encrypt(
        uint32_t* restrict v0,
        uint32_t* restrict v1,
        const uint32_t* restrict key) {
    uint32_t a = *v0, b = *v1;
    uint32_t sum = 0;
    for(int i = 0; i < TEA_ROUNDS; i++) {
        uint32_t temp = key[sum & 3] + sum;
        sum += TEA_DELTA;
        a += (temp ^ (((b >> 5) ^ (b << 4)) + b));
        temp = key[(sum >> 11) & 3] + sum;
        b += (temp ^ (((a >> 5) ^ (a << 4)) + a));
    }
    *v0 = a;
    *v1 = b;
}

__attribute__((optimize("O3"), always_inline)) static inline void
    psa_tea_decrypt(
        uint32_t* restrict v0,
        uint32_t* restrict v1,
        const uint32_t* restrict key) {
    uint32_t a = *v0, b = *v1;
    uint32_t sum = TEA_DELTA * TEA_ROUNDS;
    for(int i = 0; i < TEA_ROUNDS; i++) {
        uint32_t temp = key[(sum >> 11) & 3] + sum;
        sum -= TEA_DELTA;
        b -= (temp ^ (((a >> 5) ^ (a << 4)) + a));
        temp = key[sum & 3] + sum;
        a -= (temp ^ (((b >> 5) ^ (b << 4)) + b));
    }
    *v0 = a;
    *v1 = b;
}

typedef struct {
    uint32_t s0[TEA_ROUNDS];
    uint32_t s1[TEA_ROUNDS];
} PsaTeaSchedule;

__attribute__((optimize("O3"), always_inline)) static inline void
    psa_tea_build_schedule(const uint32_t* key, PsaTeaSchedule* out) {
    for(int i = 0; i < TEA_ROUNDS; i++) {
        uint32_t sum0 = (uint32_t)((uint64_t)i * TEA_DELTA);
        uint32_t sum1 = (uint32_t)((uint64_t)(i + 1) * TEA_DELTA);
        out->s0[i] = key[sum0 & 3] + sum0;
        out->s1[i] = key[(sum1 >> 11) & 3] + sum1;
    }
}

__attribute__((optimize("O3"), always_inline)) static inline void
    psa_tea_encrypt_with_schedule(uint32_t* restrict v0, uint32_t* restrict v1, const PsaTeaSchedule* sched) {
    uint32_t a = *v0, b = *v1;
    for(int i = 0; i < TEA_ROUNDS; i++) {
        a += (sched->s0[i] ^ (((b >> 5) ^ (b << 4)) + b));
        b += (sched->s1[i] ^ (((a >> 5) ^ (a << 4)) + a));
    }
    *v0 = a;
    *v1 = b;
}

static void psa_prepare_tea_data(uint8_t* buffer, uint32_t* w0, uint32_t* w1) {
    *w0 = ((uint32_t)buffer[3] << 16) | ((uint32_t)buffer[2] << 24) |
          ((uint32_t)buffer[4] << 8) | (uint32_t)buffer[5];
    *w1 = ((uint32_t)buffer[7] << 16) | ((uint32_t)buffer[6] << 24) |
          ((uint32_t)buffer[8] << 8) | (uint32_t)buffer[9];
}

static uint8_t psa_calculate_tea_crc(uint32_t v0, uint32_t v1) {
    uint32_t crc = ((v0 >> 24) & 0xFF) + ((v0 >> 16) & 0xFF) +
                   ((v0 >> 8) & 0xFF) + (v0 & 0xFF);
    crc += ((v1 >> 24) & 0xFF) + ((v1 >> 16) & 0xFF) + ((v1 >> 8) & 0xFF);
    return (uint8_t)(crc & 0xFF);
}

// CRC-16 lookup table (polynomial 0x8005, no reflection)
static const uint16_t psa_crc16_table[256] = {
    0x0000, 0x8005, 0x800F, 0x000A, 0x801B, 0x001E, 0x0014, 0x8011,
    0x8033, 0x0036, 0x003C, 0x8039, 0x0028, 0x802D, 0x8027, 0x0022,
    0x8063, 0x0066, 0x006C, 0x8069, 0x0078, 0x807D, 0x8077, 0x0072,
    0x0050, 0x8055, 0x805F, 0x005A, 0x804B, 0x004E, 0x0044, 0x8041,
    0x80C3, 0x00C6, 0x00CC, 0x80C9, 0x00D8, 0x80DD, 0x80D7, 0x00D2,
    0x00F0, 0x80F5, 0x80FF, 0x00FA, 0x80EB, 0x00EE, 0x00E4, 0x80E1,
    0x00A0, 0x80A5, 0x80AF, 0x00AA, 0x80BB, 0x00BE, 0x00B4, 0x80B1,
    0x8093, 0x0096, 0x009C, 0x8099, 0x0088, 0x808D, 0x8087, 0x0082,
    0x8183, 0x0186, 0x018C, 0x8189, 0x0198, 0x819D, 0x8197, 0x0192,
    0x01B0, 0x81B5, 0x81BF, 0x01BA, 0x81AB, 0x01AE, 0x01A4, 0x81A1,
    0x01E0, 0x81E5, 0x81EF, 0x01EA, 0x81FB, 0x01FE, 0x01F4, 0x81F1,
    0x81D3, 0x01D6, 0x01DC, 0x81D9, 0x01C8, 0x81CD, 0x81C7, 0x01C2,
    0x0140, 0x8145, 0x814F, 0x014A, 0x815B, 0x015E, 0x0154, 0x8151,
    0x8173, 0x0176, 0x017C, 0x8179, 0x0168, 0x816D, 0x8167, 0x0162,
    0x8123, 0x0126, 0x012C, 0x8129, 0x0138, 0x813D, 0x8137, 0x0132,
    0x0110, 0x8115, 0x811F, 0x011A, 0x810B, 0x010E, 0x0104, 0x8101,
    0x8303, 0x0306, 0x030C, 0x8309, 0x0318, 0x831D, 0x8317, 0x0312,
    0x0330, 0x8335, 0x833F, 0x033A, 0x832B, 0x032E, 0x0324, 0x8321,
    0x0360, 0x8365, 0x836F, 0x036A, 0x837B, 0x037E, 0x0374, 0x8371,
    0x8353, 0x0356, 0x035C, 0x8359, 0x0348, 0x834D, 0x8347, 0x0342,
    0x03C0, 0x83C5, 0x83CF, 0x03CA, 0x83DB, 0x03DE, 0x03D4, 0x83D1,
    0x83F3, 0x03F6, 0x03FC, 0x83F9, 0x03E8, 0x83ED, 0x83E7, 0x03E2,
    0x83A3, 0x03A6, 0x03AC, 0x83A9, 0x03B8, 0x83BD, 0x83B7, 0x03B2,
    0x0390, 0x8395, 0x839F, 0x039A, 0x838B, 0x038E, 0x0384, 0x8381,
    0x0280, 0x8285, 0x828F, 0x028A, 0x829B, 0x029E, 0x0294, 0x8291,
    0x82B3, 0x02B6, 0x02BC, 0x82B9, 0x02A8, 0x82AD, 0x82A7, 0x02A2,
    0x82E3, 0x02E6, 0x02EC, 0x82E9, 0x02F8, 0x82FD, 0x82F7, 0x02F2,
    0x02D0, 0x82D5, 0x82DF, 0x02DA, 0x82CB, 0x02CE, 0x02C4, 0x82C1,
    0x8243, 0x0246, 0x024C, 0x8249, 0x0258, 0x825D, 0x8257, 0x0252,
    0x0270, 0x8275, 0x827F, 0x027A, 0x826B, 0x026E, 0x0264, 0x8261,
    0x0220, 0x8225, 0x822F, 0x022A, 0x823B, 0x023E, 0x0234, 0x8231,
    0x8213, 0x0216, 0x021C, 0x8219, 0x0208, 0x820D, 0x8207, 0x0202,
};

static uint16_t psa_calculate_crc16_bf2(uint8_t* buffer, int length) {
    uint16_t crc = 0;
    for(int i = 0; i < length; i++) {
        crc = (crc << 8) ^ psa_crc16_table[((crc >> 8) ^ buffer[i]) & 0xFF];
    }
    return crc;
}

static void psa_unpack_tea_result_to_buffer(uint8_t* buffer, uint32_t v0, uint32_t v1) {
    buffer[2] = (uint8_t)((v0 >> 24) & 0xFF);
    buffer[3] = (uint8_t)((v0 >> 16) & 0xFF);
    buffer[4] = (uint8_t)((v0 >> 8) & 0xFF);
    buffer[5] = (uint8_t)(v0 & 0xFF);
    buffer[6] = (uint8_t)((v1 >> 24) & 0xFF);
    buffer[7] = (uint8_t)((v1 >> 16) & 0xFF);
    buffer[8] = (uint8_t)((v1 >> 8) & 0xFF);
    buffer[9] = (uint8_t)(v1 & 0xFF);
}

static void psa_extract_fields_mode23(uint8_t* buffer, SubGhzProtocolDecoderPSA* instance) {
    instance->decrypted_button = buffer[8] & 0xF;
    instance->decrypted_serial = ((uint32_t)buffer[3] << 8) | ((uint32_t)buffer[2] << 16) | (uint32_t)buffer[4];
    instance->decrypted_counter = (uint32_t)buffer[6] | ((uint32_t)buffer[5] << 8);
    instance->decrypted_crc = (uint16_t)buffer[7];
    instance->decrypted_type = 0x23;
    instance->decrypted_seed = instance->decrypted_serial;
}

static void psa_extract_fields_mode36(uint8_t* buffer, SubGhzProtocolDecoderPSA* instance) {
    instance->decrypted_button = (buffer[5] >> 4) & 0xF;
    instance->decrypted_serial = ((uint32_t)buffer[3] << 8) | ((uint32_t)buffer[2] << 16) | (uint32_t)buffer[4];
    instance->decrypted_counter = ((uint32_t)buffer[7] << 8) | ((uint32_t)buffer[6] << 16) |
                                   (uint32_t)buffer[8] | (((uint32_t)buffer[5] & 0xF) << 24);
    instance->decrypted_crc = (uint16_t)buffer[9];
    instance->decrypted_type = 0x36;
    instance->decrypted_seed = instance->decrypted_serial;
}

__attribute__((optimize("O3"))) static bool psa_brute_force_decrypt_bf1(SubGhzProtocolDecoderPSA* instance, uint8_t* buffer, uint32_t w0, uint32_t w1, PsaDecryptProgressCallback progress_cb, void* progress_ctx) {
	uint32_t bf1_total = PSA_BF1_END - PSA_BF1_START;
	PsaTeaSchedule bf1_sched;
	psa_tea_build_schedule(PSA_BF1_KEY_SCHEDULE, &bf1_sched);
	for(uint32_t counter = PSA_BF1_START; counter < PSA_BF1_END; counter++) {
		if(progress_cb && ((counter - PSA_BF1_START) & 0xFFFF) == 0) {
			uint8_t pct = (uint8_t)(((uint64_t)(counter - PSA_BF1_START) * 50) / bf1_total);
			if(!progress_cb(pct, counter - PSA_BF1_START, progress_ctx)) return false;
		}
        uint32_t wk2 = PSA_BF1_CONST_U4;
        uint32_t wk3 = counter;
        psa_tea_encrypt_with_schedule(&wk2, &wk3, &bf1_sched);

        uint32_t wk0 = (counter << 8) | 0x0E;
        uint32_t wk1 = PSA_BF1_CONST_U5;
        psa_tea_encrypt_with_schedule(&wk0, &wk1, &bf1_sched);

        uint32_t working_key[4] = {wk0, wk1, wk2, wk3};

        uint32_t dec_v0 = w0;
        uint32_t dec_v1 = w1;
        psa_tea_decrypt(&dec_v0, &dec_v1, working_key);

        if((counter & 0xFFFFFF) == (dec_v0 >> 8)) {
            uint8_t crc = psa_calculate_tea_crc(dec_v0, dec_v1);
            if(crc == (dec_v1 & 0xFF)) {
                psa_unpack_tea_result_to_buffer(buffer, dec_v0, dec_v1);
                psa_extract_fields_mode36(buffer, instance);
				instance->decrypted_seed = counter;
                return true;
            }
        }
    }
    return false;
}

__attribute__((optimize("O3"))) static bool psa_brute_force_decrypt_bf2(SubGhzProtocolDecoderPSA* instance, uint8_t* buffer, uint32_t w0, uint32_t w1, PsaDecryptProgressCallback progress_cb, void* progress_ctx) {
	uint32_t bf2_total = PSA_BF2_END - PSA_BF2_START;
	for(uint32_t counter = PSA_BF2_START; counter < PSA_BF2_END; counter++) {
		if(progress_cb && ((counter - PSA_BF2_START) & 0xFFFF) == 0) {
			uint8_t pct = 50 + (uint8_t)(((uint64_t)(counter - PSA_BF2_START) * 50) / bf2_total);
			if(!progress_cb(pct, 0x1000000 + (counter - PSA_BF2_START), progress_ctx)) return false;
		}
        uint32_t working_key[4] = {
            PSA_BF2_KEY_SCHEDULE[0] ^ counter,
            PSA_BF2_KEY_SCHEDULE[1] ^ counter,
            PSA_BF2_KEY_SCHEDULE[2] ^ counter,
            PSA_BF2_KEY_SCHEDULE[3] ^ counter,
        };
        
        uint32_t dec_v0 = w0;
        uint32_t dec_v1 = w1;
        psa_tea_decrypt(&dec_v0, &dec_v1, working_key);
        
        if((counter & 0xFFFFFF) == (dec_v0 >> 8)) {
            psa_unpack_tea_result_to_buffer(buffer, dec_v0, dec_v1);
            
            uint8_t crc_buffer[6] = {
                (uint8_t)((dec_v0 >> 24) & 0xFF),
                (uint8_t)((dec_v0 >> 8) & 0xFF),
                (uint8_t)((dec_v0 >> 16) & 0xFF),
                (uint8_t)(dec_v0 & 0xFF),
                (uint8_t)((dec_v1 >> 24) & 0xFF),
                (uint8_t)((dec_v1 >> 16) & 0xFF),
            };
            uint16_t crc16 = psa_calculate_crc16_bf2(crc_buffer, 6);
            uint16_t expected_crc = (((dec_v1 >> 16) & 0xFF) << 8) | (dec_v1 & 0xFF);
            
            if(crc16 == expected_crc) {
                psa_extract_fields_mode36(buffer, instance);
				instance->decrypted_seed = counter; // bf2 found key
                return true;
            }
        }
    }
    return false;
}

static bool psa_direct_xor_decrypt(SubGhzProtocolDecoderPSA* instance, uint8_t* buffer) {
    psa_calculate_checksum(buffer);
    uint8_t checksum = buffer[11];
    uint8_t key2_high = buffer[8];

    uint8_t validation_result = (checksum ^ key2_high) & 0xF0;
    if(validation_result == 0) {
        buffer[13] = buffer[9] ^ buffer[8];
        psa_second_stage_xor_decrypt(buffer);
        psa_extract_fields_mode23(buffer, instance);
        return true;
    }
    return false;
}

// Fast decrypt: only tries mode23 XOR (no brute force, safe for UI thread)
static void psa_decrypt_fast(SubGhzProtocolDecoderPSA* instance) {
    uint8_t buffer[48] = {0};
    psa_setup_byte_buffer(buffer, instance->key1_low, instance->key1_high, instance->key2_low);
    if(psa_direct_xor_decrypt(instance, buffer)) {
        instance->mode_serialize = 0x23;
        instance->decrypted = 0x50;
    } else {
        instance->decrypted = 0x00;
        instance->mode_serialize = 0x36;
    }
}

// Full decrypt: tries mode23 first, then brute force mode36
// WARNING: can take ~30 seconds, only call from manual context
static void psa_decrypt_full(SubGhzProtocolDecoderPSA* instance, PsaDecryptProgressCallback progress_cb, void* progress_ctx) {
    uint8_t buffer[48] = {0};
    psa_setup_byte_buffer(buffer, instance->key1_low, instance->key1_high, instance->key2_low);

    uint8_t mode = instance->mode_serialize;

    if(mode == 1 || mode == 2) {
        if(psa_direct_xor_decrypt(instance, buffer)) {
            mode = 0x23;
        } else {
            mode = 0x36;
        }
    }

    if(mode == 0x23) {
        if(psa_direct_xor_decrypt(instance, buffer)) {
            instance->mode_serialize = 0x23;
            instance->decrypted = 0x50;
            return;
        }
    } else if(mode == 0x36) {
        uint32_t w0, w1;
        psa_prepare_tea_data(buffer, &w0, &w1);
        if(psa_brute_force_decrypt_bf1(instance, buffer, w0, w1, progress_cb, progress_ctx)) {
            instance->mode_serialize = 0x36;
            instance->decrypted = 0x50;
            return;
        }
        if(psa_brute_force_decrypt_bf2(instance, buffer, w0, w1, progress_cb, progress_ctx)) {
            instance->mode_serialize = 0x36;
            instance->decrypted = 0x50;
            return;
        }
    } else {
        if(psa_direct_xor_decrypt(instance, buffer)) {
            instance->mode_serialize = 0x23;
            instance->decrypted = 0x50;
            return;
        }
        uint32_t w0, w1;
        psa_prepare_tea_data(buffer, &w0, &w1);
        if(psa_brute_force_decrypt_bf1(instance, buffer, w0, w1, progress_cb, progress_ctx)) {
            instance->mode_serialize = 0x36;
            instance->decrypted = 0x50;
            return;
        }
        if(psa_brute_force_decrypt_bf2(instance, buffer, w0, w1, progress_cb, progress_ctx)) {
            instance->mode_serialize = 0x36;
            instance->decrypted = 0x50;
            return;
        }
    }
    instance->decrypted = 0x00;
}

void* subghz_protocol_decoder_psa_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderPSA* instance = malloc(sizeof(SubGhzProtocolDecoderPSA));
    if(instance) {
        memset(instance, 0, sizeof(SubGhzProtocolDecoderPSA));
        instance->base.protocol = &subghz_protocol_psa;
        instance->generic.protocol_name = instance->base.protocol->name;
        instance->manchester_state = ManchesterStateMid1;
    }
    return instance;
}

void subghz_protocol_decoder_psa_free(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderPSA* instance = context;
    free(instance);
}

void subghz_protocol_decoder_psa_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderPSA* instance = context;

    instance->state = 0;
    instance->status_flag = 0;
    instance->mode_serialize = 0;
    instance->key1_low = 0;
    instance->key1_high = 0;
    instance->key2_low = 0;
    instance->key2_high = 0;
    instance->validation_field = 0;
    instance->decode_data_low = 0;
    instance->decode_data_high = 0;
    instance->decode_count_bit = 0;
    instance->pattern_counter = 0;
    instance->manchester_state = ManchesterStateMid1;
    instance->prev_duration = 0;
    instance->decrypted = 0;

    instance->decrypted_button = 0;
    instance->decrypted_serial = 0;
    instance->decrypted_counter = 0;
    instance->decrypted_crc = 0;
    instance->decrypted_seed = 0;
    instance->decrypted_type = 0;
}

#define PSA_FIRE_CALLBACK_IF_NEW(instance)                                          \
    do {                                                                             \
        bool _is_dup = ((instance)->key1_low  == (instance)->last_key1_low &&       \
                        (instance)->key1_high == (instance)->last_key1_high);       \
        if(!_is_dup) {                                                               \
            (instance)->last_key1_low  = (instance)->key1_low;                      \
            (instance)->last_key1_high = (instance)->key1_high;                     \
            if((instance)->base.callback) {                                          \
                (instance)->base.callback(&(instance)->base, (instance)->base.context); \
            }                                                                        \
        }                                                                            \
    } while(0)

void subghz_protocol_decoder_psa_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderPSA* instance = context;

    uint32_t tolerance;
    uint32_t new_state = instance->state;
    uint32_t prev_dur = instance->prev_duration;
    uint32_t te_short = subghz_protocol_psa_const.te_short;
    uint32_t te_long = subghz_protocol_psa_const.te_long;

    switch(instance->state) {
    case PSADecoderState0:
        if(!level) {
            return;
        }

        if(duration < te_short) {
            tolerance = te_short - duration;
            if(tolerance > PSA_TOLERANCE_99) {
                if(duration < PSA_TE_SHORT_125) {
                    tolerance = PSA_TE_SHORT_125 - duration;
                } else {
                    tolerance = duration - PSA_TE_SHORT_125;
                }
                if(tolerance > PSA_TOLERANCE_49) {
                    return;
                }
                new_state = PSADecoderState3;
            } else {
                new_state = PSADecoderState1;
            }
        } else {
            tolerance = duration - te_short;
            if(tolerance > PSA_TOLERANCE_99) {
                return;
            }
            new_state = PSADecoderState1;
        }

        instance->decode_data_low = 0;
        instance->decode_data_high = 0;
        instance->pattern_counter = 0;
        instance->decode_count_bit = 0;
        instance->mode_serialize = 0;
        instance->te_sum = duration;
        instance->te_count = 1;
        instance->te_detected = 0;
        instance->prev_duration = duration;
        manchester_advance(instance->manchester_state, ManchesterEventReset,
                         &instance->manchester_state, NULL);
        break;

    case PSADecoderState1:
        if(level) {
            return;
        }

        if(duration < te_short) {
            tolerance = te_short - duration;
            if(tolerance < PSA_TOLERANCE_100) {
                uint32_t prev_diff = psa_abs_diff(prev_dur, te_short);
                if(prev_diff <= PSA_TOLERANCE_99) {
                    instance->pattern_counter++;
                }
                instance->prev_duration = duration;
                return;
            }
        } else {
            tolerance = duration - te_short;
            if(tolerance < PSA_TOLERANCE_100) {
                uint32_t prev_diff = psa_abs_diff(prev_dur, te_short);
                if(prev_diff <= PSA_TOLERANCE_99) {
                    instance->pattern_counter++;
                }
                instance->prev_duration = duration;
                return;
            } else {
                uint32_t long_diff;
                if(duration < te_long) {
                    long_diff = te_long - duration;
                } else {
                    long_diff = duration - te_long;
                }
                if(long_diff < 100) {
                    if(instance->pattern_counter > PSA_PATTERN_THRESHOLD_1) {
                        new_state = PSADecoderState2;
                        instance->decode_data_low = 0;
                        instance->decode_data_high = 0;
                        instance->decode_count_bit = 0;
                        manchester_advance(instance->manchester_state, ManchesterEventReset,
                                         &instance->manchester_state, NULL);
                        instance->state = new_state;
                    }
                    instance->pattern_counter = 0;
                    instance->prev_duration = duration;
                    return;
                }
            }
        }

        new_state = PSADecoderState0;
        instance->pattern_counter = 0;
        break;

    case PSADecoderState2:
        if(instance->decode_count_bit >= PSA_MAX_BITS) {
            new_state = PSADecoderState0;
            break;
        }

        if(level && instance->decode_count_bit == PSA_KEY2_BITS) {
            if(duration >= 800) {
                uint32_t end_diff;
                if(duration < PSA_TE_END_1000) {
                    end_diff = PSA_TE_END_1000 - duration;
                } else {
                    end_diff = duration - PSA_TE_END_1000;
                }
                if(end_diff <= 199) {
                    instance->validation_field = (uint16_t)(instance->decode_data_low & 0xFFFF);
                    instance->key2_low = instance->decode_data_low;
                    instance->key2_high = instance->decode_data_high;
                    instance->mode_serialize = 1;
                    instance->status_flag = 0x80;

                    uint8_t buffer[48] = {0};
                    psa_setup_byte_buffer(buffer, instance->key1_low, instance->key1_high, instance->key2_low);
                    if(psa_direct_xor_decrypt(instance, buffer)) {
                        instance->mode_serialize = 0x23;
                        instance->decrypted = 0x50;
                    } else {
                        instance->decrypted = 0x00;
                        instance->mode_serialize = 0x36;
                    }

                    // Only fire callback if decrypted or validation nibble matches
                    if(instance->decrypted != 0x50 &&
                       (instance->validation_field & 0xf) != 0xa) {
                        instance->decode_data_low = 0;
                        instance->decode_data_high = 0;
                        instance->decode_count_bit = 0;
                        new_state = PSADecoderState0;
                        instance->state = new_state;
                        return;
                    }

                    instance->generic.data = ((uint64_t)instance->key1_high << 32) | instance->key1_low;
                    instance->generic.data_count_bit = 64;
                    instance->decoder.decode_data = instance->generic.data;
                    instance->decoder.decode_count_bit = 64;

                    PSA_FIRE_CALLBACK_IF_NEW(instance);

                    instance->decode_data_low = 0;
                    instance->decode_data_high = 0;
                    instance->decode_count_bit = 0;
                    new_state = PSADecoderState0;
                    instance->state = new_state;
                    return;
                }
            }
        }

        uint8_t manchester_input = 0;
        bool should_process = false;

        if(duration < te_short) {
            tolerance = te_short - duration;
            if(tolerance >= PSA_TOLERANCE_100) {
                return;
            }
            manchester_input = ((level ^ 1) & 0x7f) << 1;
            should_process = true;
        } else {
            tolerance = duration - te_short;
            if(tolerance < PSA_TOLERANCE_100) {
                manchester_input = ((level ^ 1) & 0x7f) << 1;
                should_process = true;
            } else if(duration < te_long) {
                uint32_t diff_from_250 = duration - te_short;
                uint32_t diff_from_500 = te_long - duration;

                if(diff_from_500 < 150 || diff_from_250 > diff_from_500) {
                    if(level == 0) {
                        manchester_input = 6;
                    } else {
                        manchester_input = 4;
                    }
                    should_process = true;
                } else if(diff_from_250 < 150) {
                    manchester_input = ((level ^ 1) & 0x7f) << 1;
                    should_process = true;
                } else {
                    if(duration > 10000) {
                        new_state = PSADecoderState0;
                        instance->pattern_counter = 0;
                        return;
                    }
                    if(duration >= 350 && duration <= 400) {
                        if(level == 0) {
                            manchester_input = 6;
                        } else {
                            manchester_input = 4;
                        }
                        should_process = true;
                    } else {
                        return;
                    }
                }
            } else {
                uint32_t long_diff = duration - te_long;
                if(long_diff < 100) {
                    if(level == 0) {
                        manchester_input = 6;
                    } else {
                        manchester_input = 4;
                    }
                    should_process = true;
                } else {
                    if(!level) {
                        if(duration > 10000) {
                            new_state = PSADecoderState0;
                            instance->pattern_counter = 0;
                            return;
                        }
                        return;
                    }
                    should_process = false;
                }
            }
        }

        if(should_process && instance->decode_count_bit < PSA_KEY2_BITS) {
            bool decoded_bit = false;

            if(manchester_advance(instance->manchester_state,
                               (ManchesterEvent)manchester_input,
                               &instance->manchester_state,
                               &decoded_bit)) {
                uint32_t carry = (instance->decode_data_low >> 31) & 1;
                instance->decode_data_low = (instance->decode_data_low << 1) | (decoded_bit ? 1 : 0);
                instance->decode_data_high = (instance->decode_data_high << 1) | carry;
                instance->decode_count_bit++;

                if(instance->decode_count_bit == PSA_KEY1_BITS) {
                    instance->key1_low = instance->decode_data_low;
                    instance->key1_high = instance->decode_data_high;
                    instance->decode_data_low = 0;
                    instance->decode_data_high = 0;
                }
            }
        }

        if(!level) {
            return;
        }

        if(!should_process) {
            uint32_t end_diff;
            if(duration < PSA_TE_END_1000) {
                end_diff = PSA_TE_END_1000 - duration;
            } else {
                end_diff = duration - PSA_TE_END_1000;
            }
            if(end_diff <= 199) {
                if(instance->decode_count_bit != PSA_KEY2_BITS) {
                    return;
                }

                instance->validation_field = (uint16_t)(instance->decode_data_low & 0xFFFF);

                if((instance->validation_field & 0xf) == 0xa) {
                    instance->key2_low = instance->decode_data_low;
                    instance->key2_high = instance->decode_data_high;
                    instance->mode_serialize = 1;
                    instance->status_flag = 0x80;

                    uint8_t buffer[48] = {0};
                    psa_setup_byte_buffer(buffer, instance->key1_low, instance->key1_high, instance->key2_low);
                    if(psa_direct_xor_decrypt(instance, buffer)) {
                        instance->mode_serialize = 0x23;
                        instance->decrypted = 0x50;
                    } else {
                        instance->decrypted = 0x00;
                        instance->mode_serialize = 0x36;
                    }

                    instance->generic.data = ((uint64_t)instance->key1_high << 32) | instance->key1_low;
                    instance->generic.data_count_bit = 64;
                    instance->decoder.decode_data = instance->generic.data;
                    instance->decoder.decode_count_bit = 64;

                    PSA_FIRE_CALLBACK_IF_NEW(instance);

                    instance->decode_data_low = 0;
                    instance->decode_data_high = 0;
                    instance->decode_count_bit = 0;
                    new_state = PSADecoderState0;
                } else {
                    return;
                }
            } else {
                return;
            }
        }
        break;

    case PSADecoderState3:
        if(level) {
            return;
        }

        // Adaptive AM preamble: accept 76-174us, average to detect actual TE
        if(duration >= 76 && duration <= 174) {
            if(prev_dur >= 76 && prev_dur <= 174) {
                instance->pattern_counter++;
                instance->te_sum += duration;
                instance->te_count++;
            } else {
                instance->pattern_counter = 0;
                instance->te_sum = duration;
                instance->te_count = 1;
            }
            instance->prev_duration = duration;
            return;
        } else {
            // Check if this is the preamble-to-data transition (2x detected TE)
            uint32_t te_avg = (instance->te_count > 0) ?
                (instance->te_sum / instance->te_count) : PSA_TE_SHORT_125;
            uint32_t te_long_expected = te_avg * 2;
            uint32_t long_diff = psa_abs_diff(duration, te_long_expected);

            if(long_diff <= te_avg && instance->pattern_counter > PSA_PATTERN_THRESHOLD_2) {
                instance->te_detected = te_avg;
                new_state = PSADecoderState4;
                instance->decode_data_low = 0;
                instance->decode_data_high = 0;
                instance->decode_count_bit = 0;
                manchester_advance(instance->manchester_state, ManchesterEventReset,
                                 &instance->manchester_state, NULL);
                instance->state = new_state;
                instance->pattern_counter = 0;
                instance->prev_duration = duration;
                return;
            }
        }

        new_state = PSADecoderState0;
        instance->pattern_counter = 0;
        break;

    case PSADecoderState4: {
        if(instance->decode_count_bit >= PSA_MAX_BITS) {
            new_state = PSADecoderState0;
            break;
        }

        uint32_t te_s = instance->te_detected ? instance->te_detected : PSA_TE_SHORT_125;
        uint32_t te_l = te_s * 2;
        uint32_t te_tol = te_s / 2;
        uint32_t midpoint = (te_s + te_l) / 2;

        // End marker check: HIGH pulse beyond long range at 80 bits
        if(level && instance->decode_count_bit == PSA_KEY2_BITS && duration > midpoint) {
            uint32_t end_expected = te_s * 4;
            uint32_t end_diff = psa_abs_diff(duration, end_expected);
            if(end_diff <= te_s * 2) {
                instance->validation_field = (uint16_t)(instance->decode_data_low & 0xFFFF);
                instance->key2_low = instance->decode_data_low;
                instance->key2_high = instance->decode_data_high;
                instance->mode_serialize = 2;
                instance->status_flag = 0x80;

                uint8_t buffer[48] = {0};
                psa_setup_byte_buffer(buffer, instance->key1_low, instance->key1_high, instance->key2_low);
                if(psa_direct_xor_decrypt(instance, buffer)) {
                    instance->mode_serialize = 0x23;
                    instance->decrypted = 0x50;
                } else {
                    instance->decrypted = 0x00;
                    instance->mode_serialize = 0x36;
                }

                if(instance->decrypted != 0x50 &&
                   (instance->validation_field & 0xf) != 0xa) {
                    instance->decode_data_low = 0;
                    instance->decode_data_high = 0;
                    instance->decode_count_bit = 0;
                    new_state = PSADecoderState0;
                    instance->state = new_state;
                    return;
                }

                instance->generic.data = ((uint64_t)instance->key1_high << 32) | instance->key1_low;
                instance->generic.data_count_bit = 64;
                instance->decoder.decode_data = instance->generic.data;
                instance->decoder.decode_count_bit = 64;

                PSA_FIRE_CALLBACK_IF_NEW(instance);

                instance->decode_data_low = 0;
                instance->decode_data_high = 0;
                instance->decode_count_bit = 0;
                new_state = PSADecoderState0;
                instance->state = new_state;
                return;
            }
        }

        // Manchester decode: process BOTH high and low pulses (unlike original AM path)
        if(duration > te_l + te_tol) {
            if(duration > 10000) {
                new_state = PSADecoderState0;
                break;
            }
            return;
        }

        uint8_t manchester_input;
        bool decoded_bit = false;

        if(duration <= midpoint) {
            if(psa_abs_diff(duration, te_s) > te_tol) {
                return;
            }
            manchester_input = level ? ManchesterEventShortLow : ManchesterEventShortHigh;
        } else {
            if(psa_abs_diff(duration, te_l) > te_tol) {
                return;
            }
            manchester_input = level ? ManchesterEventLongLow : ManchesterEventLongHigh;
        }

        if(instance->decode_count_bit < PSA_KEY2_BITS) {
            if(manchester_advance(instance->manchester_state,
                                 (ManchesterEvent)manchester_input,
                                 &instance->manchester_state,
                                 &decoded_bit)) {
                uint32_t carry = (instance->decode_data_low >> 31) & 1;
                // PSA AM uses inverted Manchester convention
                decoded_bit = !decoded_bit;
                instance->decode_data_low = (instance->decode_data_low << 1) | (decoded_bit ? 1 : 0);
                instance->decode_data_high = (instance->decode_data_high << 1) | carry;
                instance->decode_count_bit++;

                if(instance->decode_count_bit == PSA_KEY1_BITS) {
                    instance->key1_low = instance->decode_data_low;
                    instance->key1_high = instance->decode_data_high;
                    instance->decode_data_low = 0;
                    instance->decode_data_high = 0;
                }
            }
        }
        break;
    }
    }

    instance->state = new_state;
    instance->prev_duration = duration;
}

uint8_t subghz_protocol_decoder_psa_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderPSA* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_psa_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderPSA* instance = context;

    if(instance->decrypted != 0x50 && instance->status_flag == 0x80) {
        psa_decrypt_fast(instance);
        
        if(instance->decrypted == 0x50) {
            instance->generic.cnt = instance->decrypted_counter;
            instance->generic.serial = instance->decrypted_serial;
            instance->generic.btn = instance->decrypted_button;
        }
    }

    instance->generic.data_count_bit = 128;

    SubGhzProtocolStatus ret = subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    if(ret != SubGhzProtocolStatusOk) {
        return ret;
    }

    do {
        char key2_str[32];
        snprintf(key2_str, sizeof(key2_str), 
                 "%02X %02X %02X %02X %02X %02X %02X %02X",
                 (unsigned int)((instance->key2_high >> 24) & 0xFF),
                 (unsigned int)((instance->key2_high >> 16) & 0xFF),
                 (unsigned int)((instance->key2_high >> 8) & 0xFF),
                 (unsigned int)(instance->key2_high & 0xFF),
                 (unsigned int)((instance->key2_low >> 24) & 0xFF),
                 (unsigned int)((instance->key2_low >> 16) & 0xFF),
                 (unsigned int)((instance->key2_low >> 8) & 0xFF),
                 (unsigned int)(instance->key2_low & 0xFF));
        if(!flipper_format_write_string_cstr(flipper_format, "Key_2", key2_str)) {
            ret = SubGhzProtocolStatusError;
            break;
        }

        if(instance->decrypted == 0x50 && instance->decrypted_type != 0x00) {
            char serial_str[16];
            snprintf(serial_str, sizeof(serial_str), "%02X %02X %02X",
                     (unsigned int)((instance->decrypted_serial >> 16) & 0xFF),
                     (unsigned int)((instance->decrypted_serial >> 8) & 0xFF),
                     (unsigned int)(instance->decrypted_serial & 0xFF));
            if(!flipper_format_write_string_cstr(flipper_format, "Serial", serial_str)) {
                ret = SubGhzProtocolStatusError;
                break;
            }

            char cnt_str[24];
            if(instance->decrypted_type == 0x23) {
                snprintf(cnt_str, sizeof(cnt_str), "%02X %02X",
                         (unsigned int)((instance->decrypted_counter >> 8) & 0xFF),
                         (unsigned int)(instance->decrypted_counter & 0xFF));
            } else {
                snprintf(cnt_str, sizeof(cnt_str), "%02X %02X %02X %02X",
                         (unsigned int)((instance->decrypted_counter >> 24) & 0x0F),
                         (unsigned int)((instance->decrypted_counter >> 16) & 0xFF),
                         (unsigned int)((instance->decrypted_counter >> 8) & 0xFF),
                         (unsigned int)(instance->decrypted_counter & 0xFF));
            }
            if(!flipper_format_write_string_cstr(flipper_format, "Cnt", cnt_str)) {
                ret = SubGhzProtocolStatusError;
                break;
            }

            char btn_str[8];
            snprintf(btn_str, sizeof(btn_str), "%02X", (unsigned int)instance->decrypted_button);
            if(!flipper_format_write_string_cstr(flipper_format, "Btn", btn_str)) {
                ret = SubGhzProtocolStatusError;
                break;
            }

            char type_str[8];
            snprintf(type_str, sizeof(type_str), "%02X", (unsigned int)instance->decrypted_type);
            if(!flipper_format_write_string_cstr(flipper_format, "Type", type_str)) {
                ret = SubGhzProtocolStatusError;
                break;
            }

            char crc_str[12];
            if(instance->decrypted_type == 0x23) {
                snprintf(crc_str, sizeof(crc_str), "%02X", (unsigned int)(instance->decrypted_crc & 0xFF));
            } else {
                snprintf(crc_str, sizeof(crc_str), "%02X %02X",
                         (unsigned int)((instance->decrypted_crc >> 8) & 0xFF),
                         (unsigned int)(instance->decrypted_crc & 0xFF));
            }
            if(!flipper_format_write_string_cstr(flipper_format, "CRC", crc_str)) {
                ret = SubGhzProtocolStatusError;
                break;
            }

            char seed_str[16];
            snprintf(seed_str, sizeof(seed_str), "%02X %02X %02X",
                     (unsigned int)((instance->decrypted_seed >> 16) & 0xFF),
                     (unsigned int)((instance->decrypted_seed >> 8) & 0xFF),
                     (unsigned int)(instance->decrypted_seed & 0xFF));
            if(!flipper_format_write_string_cstr(flipper_format, "Seed", seed_str)) {
                ret = SubGhzProtocolStatusError;
                break;
            }
        }

        ret = SubGhzProtocolStatusOk;
    } while(false);

    return ret;
}

SubGhzProtocolStatus subghz_protocol_decoder_psa_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderPSA* instance = context;

    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;
    FuriString* temp_str = furi_string_alloc();

    do {
        ret = subghz_block_generic_deserialize(&instance->generic, flipper_format);
        if(ret != SubGhzProtocolStatusOk) {
            break;
        }
        
        uint64_t key1 = instance->generic.data;
        instance->key1_low = (uint32_t)(key1 & 0xFFFFFFFF);
        instance->key1_high = (uint32_t)((key1 >> 32) & 0xFFFFFFFF);

        if(!flipper_format_read_string(flipper_format, "Key_2", temp_str)) {
            break;
        }

        const char* key2_str = furi_string_get_cstr(temp_str);
        uint64_t key2 = 0;
        for(size_t i = 0; i < strlen(key2_str); i++) {
            char c = key2_str[i];
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
            key2 = (key2 << 4) | nibble;
        }
        instance->key2_low = (uint32_t)(key2 & 0xFFFFFFFF);
        instance->key2_high = (uint32_t)((key2 >> 32) & 0xFFFFFFFF);

        instance->generic.data = ((uint64_t)instance->key1_high << 32) | instance->key1_low;
        instance->generic.data_count_bit = 128;
        instance->status_flag = 0x80;

        uint32_t serial = 0;
        uint32_t counter = 0;
        uint8_t button = 0;
        uint8_t type = 0;
        uint16_t crc = 0;
        uint32_t seed = 0;
        
        bool has_decrypted_data = true;
        
        if(flipper_format_read_string(flipper_format, "Serial", temp_str)) {
            const char* serial_str = furi_string_get_cstr(temp_str);
            for(size_t i = 0; i < strlen(serial_str); i++) {
                char c = serial_str[i];
                if(c == ' ') continue;
                uint8_t nibble = (c >= '0' && c <= '9') ? c - '0' : 
                               (c >= 'A' && c <= 'F') ? c - 'A' + 10 : 
                               (c >= 'a' && c <= 'f') ? c - 'a' + 10 : 0;
                serial = (serial << 4) | nibble;
            }
        } else {
            has_decrypted_data = false;
        }

        if(flipper_format_read_string(flipper_format, "Cnt", temp_str)) {
            const char* cnt_str = furi_string_get_cstr(temp_str);
            for(size_t i = 0; i < strlen(cnt_str); i++) {
                char c = cnt_str[i];
                if(c == ' ') continue;
                uint8_t nibble = (c >= '0' && c <= '9') ? c - '0' : 
                               (c >= 'A' && c <= 'F') ? c - 'A' + 10 : 
                               (c >= 'a' && c <= 'f') ? c - 'a' + 10 : 0;
                counter = (counter << 4) | nibble;
            }
        } else {
            has_decrypted_data = false;
        }

        if(flipper_format_read_string(flipper_format, "Btn", temp_str)) {
            const char* btn_str = furi_string_get_cstr(temp_str);
            uint32_t btn_val = 0;
            for(size_t i = 0; i < strlen(btn_str); i++) {
                char c = btn_str[i];
                if(c == ' ') continue;
                uint8_t nibble = (c >= '0' && c <= '9') ? c - '0' : 
                               (c >= 'A' && c <= 'F') ? c - 'A' + 10 : 
                               (c >= 'a' && c <= 'f') ? c - 'a' + 10 : 0;
                btn_val = (btn_val << 4) | nibble;
            }
            button = (uint8_t)(btn_val & 0xFF);
        } else {
            has_decrypted_data = false;
        }

        if(flipper_format_read_string(flipper_format, "Type", temp_str)) {
            const char* type_str = furi_string_get_cstr(temp_str);
            uint32_t type_val = 0;
            for(size_t i = 0; i < strlen(type_str); i++) {
                char c = type_str[i];
                if(c == ' ') continue;
                uint8_t nibble = (c >= '0' && c <= '9') ? c - '0' : 
                               (c >= 'A' && c <= 'F') ? c - 'A' + 10 : 
                               (c >= 'a' && c <= 'f') ? c - 'a' + 10 : 0;
                type_val = (type_val << 4) | nibble;
            }
            type = (uint8_t)(type_val & 0xFF);
        } else {
            has_decrypted_data = false;
        }

        if(flipper_format_read_string(flipper_format, "CRC", temp_str)) {
            const char* crc_str = furi_string_get_cstr(temp_str);
            uint32_t crc_val = 0;
            for(size_t i = 0; i < strlen(crc_str); i++) {
                char c = crc_str[i];
                if(c == ' ') continue;
                uint8_t nibble = (c >= '0' && c <= '9') ? c - '0' : 
                               (c >= 'A' && c <= 'F') ? c - 'A' + 10 : 
                               (c >= 'a' && c <= 'f') ? c - 'a' + 10 : 0;
                crc_val = (crc_val << 4) | nibble;
            }
            crc = (uint16_t)(crc_val & 0xFFFF);
        } else {
            has_decrypted_data = false;
        }

        if(flipper_format_read_string(flipper_format, "Seed", temp_str)) {
            const char* seed_str = furi_string_get_cstr(temp_str);
            for(size_t i = 0; i < strlen(seed_str); i++) {
                char c = seed_str[i];
                if(c == ' ') continue;
                uint8_t nibble = (c >= '0' && c <= '9') ? c - '0' : 
                               (c >= 'A' && c <= 'F') ? c - 'A' + 10 : 
                               (c >= 'a' && c <= 'f') ? c - 'a' + 10 : 0;
                seed = (seed << 4) | nibble;
            }
        }

        if(has_decrypted_data && type != 0x00) {
            instance->decrypted_serial = serial;
            instance->decrypted_counter = counter;
            instance->decrypted_button = button;
            instance->decrypted_type = type;
            instance->decrypted_crc = crc;
            instance->decrypted_seed = seed;
            instance->decrypted = 0x50;
            
            instance->generic.cnt = counter;
            instance->generic.serial = serial;
            instance->generic.btn = button;
        } else {
            psa_decrypt_fast(instance);
            
            instance->generic.cnt = instance->decrypted_counter;
            instance->generic.serial = instance->decrypted_serial;
            instance->generic.btn = instance->decrypted_button;
        }

        ret = SubGhzProtocolStatusOk;
    } while(false);

    furi_string_free(temp_str);
    return ret;
}

void subghz_protocol_decoder_psa_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderPSA* instance = context;


    uint16_t key2_value = (uint16_t)(instance->key2_low & 0xFFFF);

    if(instance->decrypted == 0x50 && instance->decrypted_type != 0) {
        // Always update original button when loading a new file
        subghz_custom_btn_set_original(instance->generic.btn == 0 ? 0xFF : instance->generic.btn);
        subghz_custom_btn_set_max(4);
        uint8_t display_btn = psa_get_btn_code();
        if(instance->decrypted_type == 0x23) {
            furi_string_printf(
                output,
                "%s %dbit\r\n"
                "Key1:%08lX%08lX\r\n"
                "Key2:%04X Ser:%06lX\r\n"
                "Btn:[%s] Cnt:%04lX\r\n"
                "Type:%02X Sd:%06lX CRC:%02X",
                instance->base.protocol->name,
                128,
                instance->key1_high,
                instance->key1_low,
                key2_value,
                instance->generic.serial,
                psa_button_name(display_btn),
                instance->generic.cnt,
                instance->decrypted_type,
                instance->decrypted_seed,
                instance->decrypted_crc);
        } else if(instance->decrypted_type == 0x36) {
            furi_string_printf(
                output,
                "%s %dbit\r\n"
                "Key1:%08lX%08lX\r\n"
                "Key2:%04X Ser:%06lX\r\n"
                "Btn:[%s] Cnt:%08lX\r\n"
                "Type:%02X Sd:%06lX CRC:%02X",
                instance->base.protocol->name,
                128,
                instance->key1_high,
                instance->key1_low,
                key2_value,
                instance->generic.serial,
                psa_button_name(display_btn),
                instance->generic.cnt,
                instance->decrypted_type,
                instance->decrypted_seed,
                instance->decrypted_crc);
        }
    } else {
        furi_string_printf(
            output,
            "%s %dbit\r\n"
            "Key1:%08lX%08lX\r\n"
            "Key2:%X",
            instance->base.protocol->name,
            128,
            instance->key1_high,
            instance->key1_low,
            key2_value);
    }
}


static void psa_build_buffer_mode23(SubGhzProtocolEncoderPSA* instance, uint8_t* buffer, uint8_t* preserve_buffer01) {
    memset(buffer, 0, 48);
    
    buffer[2] = (uint8_t)((instance->serial >> 16) & 0xFF);
    buffer[3] = (uint8_t)((instance->serial >> 8) & 0xFF);
    buffer[4] = (uint8_t)(instance->serial & 0xFF);
    buffer[5] = (uint8_t)((instance->counter >> 8) & 0xFF);
    buffer[6] = (uint8_t)(instance->counter & 0xFF);
    buffer[7] = (uint8_t)(instance->crc & 0xFF);
    buffer[8] = (uint8_t)(instance->button & 0xF);
    
    uint8_t original_buffer9 = 0;
    bool has_original_key2 = (instance->key2_low != 0);
    if(has_original_key2) {
        original_buffer9 = (uint8_t)(instance->key2_low & 0xFF);
        buffer[9] = original_buffer9;
    }
    
    uint8_t initial_plaintext[6];
    initial_plaintext[0] = buffer[2];
    initial_plaintext[1] = buffer[3];
    initial_plaintext[2] = buffer[4];
    initial_plaintext[3] = buffer[5];
    initial_plaintext[4] = buffer[6];
    initial_plaintext[5] = buffer[7];
    uint8_t initial_button = buffer[8] & 0xF;
    
    bool found = false;
    uint8_t buffer9_to_use = has_original_key2 ? original_buffer9 : 0;
    uint8_t buffer9_end = has_original_key2 ? original_buffer9 + 1 : 255;
    
    for(uint8_t buffer9_try = buffer9_to_use; buffer9_try < buffer9_end && !found; buffer9_try++) {
        for(uint8_t buffer8_high_try = 0; buffer8_high_try < 16 && !found; buffer8_high_try++) {
            buffer[2] = initial_plaintext[0];
            buffer[3] = initial_plaintext[1];
            buffer[4] = initial_plaintext[2];
            buffer[5] = initial_plaintext[3];
            buffer[6] = initial_plaintext[4];
            buffer[7] = initial_plaintext[5];
            buffer[8] = initial_button | (buffer8_high_try << 4);
            buffer[9] = buffer9_try;
            
            psa_second_stage_xor_encrypt(buffer);
            psa_calculate_checksum(buffer);
            uint8_t checksum_after = buffer[11];
            uint8_t key2_high_after = checksum_after & 0xF0;
            
            uint8_t validation = (checksum_after ^ buffer[8]) & 0xF0;
            if(validation == 0) {
                buffer[8] = (buffer[8] & 0x0F) | key2_high_after;
                buffer[13] = buffer[9] ^ buffer[8];
                found = true;
                break;
            }
        }
    }
    
    if(!found) {
        buffer[2] = initial_plaintext[0];
        buffer[3] = initial_plaintext[1];
        buffer[4] = initial_plaintext[2];
        buffer[5] = initial_plaintext[3];
        buffer[6] = initial_plaintext[4];
        buffer[7] = initial_plaintext[5];
        buffer[8] = initial_button;
        buffer[9] = has_original_key2 ? original_buffer9 : 0x23;
        
        psa_second_stage_xor_encrypt(buffer);
        psa_calculate_checksum(buffer);
        uint8_t checksum_after = buffer[11];
        uint8_t key2_high_after = checksum_after & 0xF0;
        buffer[8] = (buffer[8] & 0x0F) | key2_high_after;
        buffer[13] = buffer[9] ^ buffer[8];
    }
    
    if(preserve_buffer01 != NULL) {
        buffer[0] = preserve_buffer01[0];
        buffer[1] = preserve_buffer01[1];
    } else {
        buffer[0] = buffer[2] ^ buffer[6];
        buffer[1] = buffer[3] ^ buffer[7];
    }
}

static void psa_build_buffer_mode36(SubGhzProtocolEncoderPSA* instance, uint8_t* buffer, uint8_t* preserve_buffer01) {
    memset(buffer, 0, 48);
    
    uint32_t v0 = ((instance->serial & 0xFFFFFF) << 8) |
                   ((instance->button & 0xF) << 4) |
                   ((instance->counter >> 24) & 0x0F);
    uint32_t v1 = ((instance->counter & 0xFFFFFF) << 8) |
                   (instance->crc & 0xFF);
    
    uint8_t crc = psa_calculate_tea_crc(v0, v1);
    v1 = (v1 & 0xFFFFFF00) | crc;
    
    uint32_t bf_counter = PSA_BF1_START | (instance->serial & 0xFFFFFF);
    
    uint32_t working_key[4];
    
    uint32_t wk2 = PSA_BF1_CONST_U4;
    uint32_t wk3 = bf_counter;
    psa_tea_encrypt(&wk2, &wk3, PSA_BF1_KEY_SCHEDULE);
    
    uint32_t wk0 = (bf_counter << 8) | 0x0E;
    uint32_t wk1 = PSA_BF1_CONST_U5;
    psa_tea_encrypt(&wk0, &wk1, PSA_BF1_KEY_SCHEDULE);
    
    working_key[0] = wk0;
    working_key[1] = wk1;
    working_key[2] = wk2;
    working_key[3] = wk3;
    
    psa_tea_encrypt(&v0, &v1, working_key);
    
    psa_unpack_tea_result_to_buffer(buffer, v0, v1);
    
    if(preserve_buffer01 != NULL) {
        buffer[0] = preserve_buffer01[0];
        buffer[1] = preserve_buffer01[1];
    } else {
        buffer[0] = buffer[2] ^ buffer[6];
        buffer[1] = buffer[3] ^ buffer[7];
    }
}

static void psa_encoder_build_upload(SubGhzProtocolEncoderPSA* instance) {
    furi_assert(instance);
    
    uint8_t buffer[48] = {0};
    
    uint8_t preserve_buffer01[2] = {0};
    uint8_t* preserve_ptr = NULL;
    
    if(instance->key1_low != 0 || instance->key1_high != 0) {
        uint8_t orig_buffer[48] = {0};
        psa_setup_byte_buffer(orig_buffer, instance->key1_low, instance->key1_high, instance->key2_low);
        preserve_buffer01[0] = orig_buffer[0];
        preserve_buffer01[1] = orig_buffer[1];
        preserve_ptr = preserve_buffer01;
    }
    
    if(instance->mode == 0x23) {
        psa_build_buffer_mode23(instance, buffer, preserve_ptr);
    } else if(instance->mode == 0x36) {
        psa_build_buffer_mode36(instance, buffer, preserve_ptr);
    } else {
        return;
    }
    
    uint32_t key1_high = ((uint32_t)buffer[0] << 24) | ((uint32_t)buffer[1] << 16) |
                         ((uint32_t)buffer[2] << 8) | (uint32_t)buffer[3];
    uint32_t key1_low = ((uint32_t)buffer[4] << 24) | ((uint32_t)buffer[5] << 16) |
                        ((uint32_t)buffer[6] << 8) | (uint32_t)buffer[7];
    uint16_t validation_field = ((uint16_t)buffer[8] << 8) | (uint16_t)buffer[9];
    
    size_t index = 0;
    uint32_t te = PSA_TE_LONG_250;
    
    for(int i = 0; i < 80; i++) {
        if(index >= instance->encoder.size_upload - 2) break;
        instance->encoder.upload[index++] = level_duration_make(true, te);
        instance->encoder.upload[index++] = level_duration_make(false, te);
    }
    
    uint32_t te_long_transition = subghz_protocol_psa_const.te_long;
    if(index < instance->encoder.size_upload - 3) {
        instance->encoder.upload[index++] = level_duration_make(false, te);
        instance->encoder.upload[index++] = level_duration_make(true, te_long_transition);
        instance->encoder.upload[index++] = level_duration_make(false, te);
    }
    
    uint64_t key1_data = ((uint64_t)key1_high << 32) | key1_low;
    for(int bit = 63; bit >= 0; bit--) {
        if(index >= instance->encoder.size_upload - 2) break;
        bool bit_value = (key1_data >> bit) & 1;
        if(bit_value) {
            instance->encoder.upload[index++] = level_duration_make(true, te);
            instance->encoder.upload[index++] = level_duration_make(false, te);
        } else {
            instance->encoder.upload[index++] = level_duration_make(false, te);
            instance->encoder.upload[index++] = level_duration_make(true, te);
        }
    }
    
    for(int bit = 15; bit >= 0; bit--) {
        if(index >= instance->encoder.size_upload - 2) break;
        bool bit_value = (validation_field >> bit) & 1;
        if(bit_value) {
            instance->encoder.upload[index++] = level_duration_make(true, te);
            instance->encoder.upload[index++] = level_duration_make(false, te);
        } else {
            instance->encoder.upload[index++] = level_duration_make(false, te);
            instance->encoder.upload[index++] = level_duration_make(true, te);
        }
    }
    
    uint32_t end_duration = PSA_TE_END_1000;
    if(index < instance->encoder.size_upload - 1) {
        instance->encoder.upload[index++] = level_duration_make(true, end_duration);
        instance->encoder.upload[index++] = level_duration_make(false, end_duration);
    }
    
    instance->encoder.size_upload = index;
    instance->encoder.front = 0;
    instance->encoder.repeat = 10;
    
    instance->key1_high = key1_high;
    instance->key1_low = key1_low;
    instance->key2_low = validation_field;
}

void* subghz_protocol_encoder_psa_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderPSA* instance = malloc(sizeof(SubGhzProtocolEncoderPSA));
    if(instance) {
        memset(instance, 0, sizeof(SubGhzProtocolEncoderPSA));
        instance->base.protocol = &subghz_protocol_psa;
        instance->generic.protocol_name = instance->base.protocol->name;
        instance->encoder.size_upload = 600;
        instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
        instance->encoder.repeat = 10;
        instance->encoder.front = 0;
        instance->encoder.is_running = false;
        instance->is_running = false;
    }
    return instance;
}

void subghz_protocol_encoder_psa_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderPSA* instance = context;
    if(instance->encoder.upload) free(instance->encoder.upload);
    free(instance);
}

SubGhzProtocolStatus subghz_protocol_encoder_psa_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolEncoderPSA* instance = context;
    
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;
    FuriString* temp_str = furi_string_alloc();
    
    do {
        flipper_format_rewind(flipper_format);
        ret = subghz_block_generic_deserialize(&instance->generic, flipper_format);
        if(ret != SubGhzProtocolStatusOk) {
            break;
        }
        
        uint64_t key1 = instance->generic.data;
        instance->key1_low = (uint32_t)(key1 & 0xFFFFFFFF);
        instance->key1_high = (uint32_t)((key1 >> 32) & 0xFFFFFFFF);
        
        flipper_format_rewind(flipper_format);
        if(flipper_format_read_string(flipper_format, "Key_2", temp_str)) {
            const char* key2_str = furi_string_get_cstr(temp_str);
            uint64_t key2 = 0;
            for(size_t i = 0; i < strlen(key2_str); i++) {
                char c = key2_str[i];
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
                key2 = (key2 << 4) | nibble;
            }
            instance->key2_low = (uint32_t)(key2 & 0xFFFFFFFF);
            instance->validation_field = (uint16_t)(key2 & 0xFFFF);
        } else {
            break;
        }
        
        uint32_t serial = 0;
        uint32_t counter = 0;
        uint8_t button = 0;
        uint8_t type = 0;
        uint16_t crc = 0;
        uint32_t seed = 0;
        
        bool has_decrypted_data = true;
        
        flipper_format_rewind(flipper_format);
        if(flipper_format_read_string(flipper_format, "Serial", temp_str)) {
            const char* serial_str = furi_string_get_cstr(temp_str);
            for(size_t i = 0; i < strlen(serial_str); i++) {
                char c = serial_str[i];
                if(c == ' ') continue;
                uint8_t nibble = (c >= '0' && c <= '9') ? c - '0' : 
                               (c >= 'A' && c <= 'F') ? c - 'A' + 10 : 
                               (c >= 'a' && c <= 'f') ? c - 'a' + 10 : 0;
                serial = (serial << 4) | nibble;
            }
        } else {
            has_decrypted_data = false;
        }

        flipper_format_rewind(flipper_format);
        if(flipper_format_read_string(flipper_format, "Cnt", temp_str)) {
            const char* cnt_str = furi_string_get_cstr(temp_str);
            for(size_t i = 0; i < strlen(cnt_str); i++) {
                char c = cnt_str[i];
                if(c == ' ') continue;
                uint8_t nibble = (c >= '0' && c <= '9') ? c - '0' : 
                               (c >= 'A' && c <= 'F') ? c - 'A' + 10 : 
                               (c >= 'a' && c <= 'f') ? c - 'a' + 10 : 0;
                counter = (counter << 4) | nibble;
            }
        } else {
            has_decrypted_data = false;
        }

        flipper_format_rewind(flipper_format);
        if(flipper_format_read_string(flipper_format, "Btn", temp_str)) {
            const char* btn_str = furi_string_get_cstr(temp_str);
            uint32_t btn_val = 0;
            for(size_t i = 0; i < strlen(btn_str); i++) {
                char c = btn_str[i];
                if(c == ' ') continue;
                uint8_t nibble = (c >= '0' && c <= '9') ? c - '0' : 
                               (c >= 'A' && c <= 'F') ? c - 'A' + 10 : 
                               (c >= 'a' && c <= 'f') ? c - 'a' + 10 : 0;
                btn_val = (btn_val << 4) | nibble;
            }
            button = (uint8_t)(btn_val & 0xFF);
        } else {
            has_decrypted_data = false;
        }

        flipper_format_rewind(flipper_format);
        if(flipper_format_read_string(flipper_format, "Type", temp_str)) {
            const char* type_str = furi_string_get_cstr(temp_str);
            uint32_t type_val = 0;
            for(size_t i = 0; i < strlen(type_str); i++) {
                char c = type_str[i];
                if(c == ' ') continue;
                uint8_t nibble = (c >= '0' && c <= '9') ? c - '0' : 
                               (c >= 'A' && c <= 'F') ? c - 'A' + 10 : 
                               (c >= 'a' && c <= 'f') ? c - 'a' + 10 : 0;
                type_val = (type_val << 4) | nibble;
            }
            type = (uint8_t)(type_val & 0xFF);
        } else {
            has_decrypted_data = false;
        }

        flipper_format_rewind(flipper_format);
        if(flipper_format_read_string(flipper_format, "CRC", temp_str)) {
            const char* crc_str = furi_string_get_cstr(temp_str);
            uint32_t crc_val = 0;
            for(size_t i = 0; i < strlen(crc_str); i++) {
                char c = crc_str[i];
                if(c == ' ') continue;
                uint8_t nibble = (c >= '0' && c <= '9') ? c - '0' : 
                               (c >= 'A' && c <= 'F') ? c - 'A' + 10 : 
                               (c >= 'a' && c <= 'f') ? c - 'a' + 10 : 0;
                crc_val = (crc_val << 4) | nibble;
            }
            crc = (uint16_t)(crc_val & 0xFFFF);
        } else {
            has_decrypted_data = false;
        }

        flipper_format_rewind(flipper_format);
        if(flipper_format_read_string(flipper_format, "Seed", temp_str)) {
            const char* seed_str = furi_string_get_cstr(temp_str);
            for(size_t i = 0; i < strlen(seed_str); i++) {
                char c = seed_str[i];
                if(c == ' ') continue;
                uint8_t nibble = (c >= '0' && c <= '9') ? c - '0' : 
                               (c >= 'A' && c <= 'F') ? c - 'A' + 10 : 
                               (c >= 'a' && c <= 'f') ? c - 'a' + 10 : 0;
                seed = (seed << 4) | nibble;
            }
        }

        if(has_decrypted_data && type != 0x00) {
            instance->serial = serial;
            instance->counter = counter;
            instance->button = button;
            instance->type = type;
            instance->crc = crc;
            instance->seed = (uint8_t)(seed & 0xFF);
            
            instance->mode = instance->type;
            if(instance->mode == 0x23 || instance->mode == 0) {
                instance->mode = 0x23;
            } else if(instance->mode == 0x36) {
                instance->mode = 0x36;
            } else {
                instance->mode = 0x23;
            }
            
            // Setup custom button system
            // Save original button from FILE before any d-pad remapping
            uint8_t file_btn = button; // 'button' was read directly from file above
            subghz_custom_btn_set_original(file_btn == 0 ? 0xFF : file_btn);
            subghz_custom_btn_set_max(4);
            instance->button = psa_get_btn_code();
            FURI_LOG_I("PSA_ENC", "file_btn=%02X custom=%02X result=%02X orig=%02X", file_btn, subghz_custom_btn_get(), instance->button, subghz_custom_btn_get_original());
            
            // Increment counter
            uint32_t mult = furi_hal_subghz_get_rolling_counter_mult();
            instance->counter = (instance->counter + mult) & 0xFFFFFFFF;
                        
            psa_encoder_build_upload(instance);
            
            instance->generic.data = ((uint64_t)instance->key1_high << 32) | instance->key1_low;
            instance->generic.cnt = instance->counter;
            instance->generic.serial = instance->serial;
            instance->generic.btn = instance->button;
            
            flipper_format_rewind(flipper_format);
            
            char cnt_str[24];
            if(instance->type == 0x23) {
                snprintf(cnt_str, sizeof(cnt_str), "%02X %02X",
                        (unsigned int)((instance->counter >> 8) & 0xFF),
                        (unsigned int)(instance->counter & 0xFF));
            } else {
                snprintf(cnt_str, sizeof(cnt_str), "%02X %02X %02X %02X",
                        (unsigned int)((instance->counter >> 24) & 0x0F),
                        (unsigned int)((instance->counter >> 16) & 0xFF),
                        (unsigned int)((instance->counter >> 8) & 0xFF),
                        (unsigned int)(instance->counter & 0xFF));
            }
            flipper_format_update_string_cstr(flipper_format, "Cnt", cnt_str);
            
            char key_str[32];
            uint64_t key1 = ((uint64_t)instance->key1_high << 32) | instance->key1_low;
            snprintf(key_str, sizeof(key_str), "%02X %02X %02X %02X %02X %02X %02X %02X",
                    (unsigned int)((key1 >> 56) & 0xFF),
                    (unsigned int)((key1 >> 48) & 0xFF),
                    (unsigned int)((key1 >> 40) & 0xFF),
                    (unsigned int)((key1 >> 32) & 0xFF),
                    (unsigned int)((key1 >> 24) & 0xFF),
                    (unsigned int)((key1 >> 16) & 0xFF),
                    (unsigned int)((key1 >> 8) & 0xFF),
                    (unsigned int)(key1 & 0xFF));
            flipper_format_update_string_cstr(flipper_format, "Key", key_str);
            
            char key2_str[32];
            snprintf(key2_str, sizeof(key2_str), "%02X %02X %02X %02X %02X %02X %02X %02X",
                    0, 0, 0, 0, 0, 0,
                    (unsigned int)((instance->key2_low >> 8) & 0xFF),
                    (unsigned int)(instance->key2_low & 0xFF));
            flipper_format_update_string_cstr(flipper_format, "Key_2", key2_str);
            
            
            instance->is_running = true;
            ret = SubGhzProtocolStatusOk;
        } else {
            ret = SubGhzProtocolStatusErrorParserOthers;
        }
        
    } while(false);
    
    furi_string_free(temp_str);
    return ret;
}

void subghz_protocol_encoder_psa_stop(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderPSA* instance = context;
    instance->is_running = false;
    instance->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_psa_yield(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderPSA* instance = context;
    if(!instance->is_running || instance->encoder.size_upload == 0) {
        instance->is_running = false;
        return level_duration_reset();
    }
    LevelDuration ret = instance->encoder.upload[instance->encoder.front];
    instance->encoder.front++;
    if(instance->encoder.front >= instance->encoder.size_upload) {
        instance->encoder.front = 0;
        instance->encoder.repeat--;
        if(instance->encoder.repeat <= 0) instance->is_running = false;
    }
    return ret;
}

bool subghz_protocol_psa_decrypt_file(FlipperFormat* flipper_format, FuriString* result_str, PsaDecryptProgressCallback progress_cb, void* progress_ctx) {
    SubGhzProtocolDecoderPSA instance = {0};

    // Read Key (key1)
    uint8_t key1_bytes[8] = {0};
    flipper_format_rewind(flipper_format);
    if(!flipper_format_read_hex(flipper_format, "Key", key1_bytes, 8)) return false;
    instance.key1_high = ((uint32_t)key1_bytes[0] << 24) | ((uint32_t)key1_bytes[1] << 16) |
                         ((uint32_t)key1_bytes[2] << 8) | key1_bytes[3];
    instance.key1_low  = ((uint32_t)key1_bytes[4] << 24) | ((uint32_t)key1_bytes[5] << 16) |
                         ((uint32_t)key1_bytes[6] << 8) | key1_bytes[7];

    // Read Key_2
    FuriString* temp = furi_string_alloc();
    flipper_format_rewind(flipper_format);
    if(!flipper_format_read_string(flipper_format, "Key_2", temp)) {
        furi_string_free(temp);
        return false;
    }
    const char* k2 = furi_string_get_cstr(temp);
    uint64_t key2 = 0;
    for(size_t i = 0; i < strlen(k2); i++) {
        char c = k2[i]; if(c == ' ') continue;
        uint8_t n = (c>='0'&&c<='9') ? c-'0' : (c>='A'&&c<='F') ? c-'A'+10 : (c>='a'&&c<='f') ? c-'a'+10 : 0;
        key2 = (key2 << 4) | n;
    }
    furi_string_free(temp);
    instance.key2_low  = (uint32_t)(key2 & 0xFFFFFFFF);
    instance.key2_high = (uint32_t)(key2 >> 32);
    instance.status_flag = 0x80;
    instance.mode_serialize = 0;

    // Run full decrypt router (includes brute force)
    psa_decrypt_full(&instance, progress_cb, progress_ctx);

    if(instance.decrypted != 0x50 || instance.decrypted_type == 0) return false;

    // Write results back to file
    flipper_format_rewind(flipper_format);
    char serial_str[16];
    snprintf(serial_str, sizeof(serial_str), "%02X %02X %02X",
        (unsigned int)((instance.decrypted_serial >> 16) & 0xFF),
        (unsigned int)((instance.decrypted_serial >> 8) & 0xFF),
        (unsigned int)(instance.decrypted_serial & 0xFF));
    flipper_format_insert_or_update_string_cstr(flipper_format, "Serial", serial_str);

    flipper_format_rewind(flipper_format);
    char cnt_str[24];
    if(instance.decrypted_type == 0x23) {
        snprintf(cnt_str, sizeof(cnt_str), "%02X %02X",
            (unsigned int)((instance.decrypted_counter >> 8) & 0xFF),
            (unsigned int)(instance.decrypted_counter & 0xFF));
    } else {
        snprintf(cnt_str, sizeof(cnt_str), "%02X %02X %02X %02X",
            (unsigned int)((instance.decrypted_counter >> 24) & 0x0F),
            (unsigned int)((instance.decrypted_counter >> 16) & 0xFF),
            (unsigned int)((instance.decrypted_counter >> 8) & 0xFF),
            (unsigned int)(instance.decrypted_counter & 0xFF));
    }
    flipper_format_insert_or_update_string_cstr(flipper_format, "Cnt", cnt_str);

    flipper_format_rewind(flipper_format);
    char btn_str[8];
    snprintf(btn_str, sizeof(btn_str), "%02X", (unsigned int)instance.decrypted_button);
    flipper_format_insert_or_update_string_cstr(flipper_format, "Btn", btn_str);

    flipper_format_rewind(flipper_format);
    char type_str[8];
    snprintf(type_str, sizeof(type_str), "%02X", (unsigned int)instance.decrypted_type);
    flipper_format_insert_or_update_string_cstr(flipper_format, "Type", type_str);

    flipper_format_rewind(flipper_format);
    char crc_str[12];
    if(instance.decrypted_type == 0x23) {
        snprintf(crc_str, sizeof(crc_str), "%02X", (unsigned int)(instance.decrypted_crc & 0xFF));
    } else {
        snprintf(crc_str, sizeof(crc_str), "%02X %02X",
            (unsigned int)((instance.decrypted_crc >> 8) & 0xFF),
            (unsigned int)(instance.decrypted_crc & 0xFF));
    }
    flipper_format_insert_or_update_string_cstr(flipper_format, "CRC", crc_str);

    flipper_format_rewind(flipper_format);
    char seed_str[16];
    snprintf(seed_str, sizeof(seed_str), "%02X %02X %02X",
        (unsigned int)((instance.decrypted_serial >> 16) & 0xFF),
        (unsigned int)((instance.decrypted_serial >> 8) & 0xFF),
        (unsigned int)(instance.decrypted_serial & 0xFF));
    flipper_format_insert_or_update_string_cstr(flipper_format, "Seed", seed_str);

    if(result_str != NULL) {
        furi_string_printf(result_str,
            "Type: %02X\nSeed: %08lX",
            instance.decrypted_type,
            instance.decrypted_seed);
    }

    return true;
}

bool subghz_protocol_psa_get_bf_params(
    FlipperFormat* flipper_format,
    uint32_t* w0,
    uint32_t* w1) {
    SubGhzProtocolDecoderPSA instance = {0};

    uint8_t key1_bytes[8] = {0};
    flipper_format_rewind(flipper_format);
    if(!flipper_format_read_hex(flipper_format, "Key", key1_bytes, 8)) return false;
    instance.key1_high = ((uint32_t)key1_bytes[0] << 24) | ((uint32_t)key1_bytes[1] << 16) |
                         ((uint32_t)key1_bytes[2] << 8) | key1_bytes[3];
    instance.key1_low  = ((uint32_t)key1_bytes[4] << 24) | ((uint32_t)key1_bytes[5] << 16) |
                         ((uint32_t)key1_bytes[6] << 8) | key1_bytes[7];

    FuriString* temp = furi_string_alloc();
    flipper_format_rewind(flipper_format);
    if(!flipper_format_read_string(flipper_format, "Key_2", temp)) {
        furi_string_free(temp);
        return false;
    }
    const char* k2 = furi_string_get_cstr(temp);
    uint64_t key2 = 0;
    for(size_t i = 0; i < strlen(k2); i++) {
        char c = k2[i]; if(c == ' ') continue;
        uint8_t n = (c>='0'&&c<='9') ? c-'0' : (c>='A'&&c<='F') ? c-'A'+10 : (c>='a'&&c<='f') ? c-'a'+10 : 0;
        key2 = (key2 << 4) | n;
    }
    furi_string_free(temp);
    instance.key2_low = (uint32_t)(key2 & 0xFFFFFFFF);

    // Check if XOR decrypt works (mode23) — if so, no BF needed
    uint8_t buffer[48] = {0};
    psa_setup_byte_buffer(buffer, instance.key1_low, instance.key1_high, instance.key2_low);
    if(psa_direct_xor_decrypt(&instance, buffer)) return false;

    // Needs TEA BF — extract w0/w1
    psa_prepare_tea_data(buffer, w0, w1);
    return true;
}

bool subghz_protocol_psa_apply_bf_result(
    FlipperFormat* flipper_format,
    FuriString* result_str,
    uint32_t counter,
    uint32_t dec_v0,
    uint32_t dec_v1,
    int bf_type) {
    UNUSED(bf_type);

    SubGhzProtocolDecoderPSA instance = {0};

    uint8_t buffer[48] = {0};
    psa_unpack_tea_result_to_buffer(buffer, dec_v0, dec_v1);
    psa_extract_fields_mode36(buffer, &instance);
    instance.decrypted_seed = counter;
    instance.decrypted_type = 0x36;

    // Write results to flipper format
    flipper_format_rewind(flipper_format);
    char serial_str[16];
    snprintf(serial_str, sizeof(serial_str), "%02X %02X %02X",
        (unsigned int)((instance.decrypted_serial >> 16) & 0xFF),
        (unsigned int)((instance.decrypted_serial >> 8) & 0xFF),
        (unsigned int)(instance.decrypted_serial & 0xFF));
    flipper_format_insert_or_update_string_cstr(flipper_format, "Serial", serial_str);

    flipper_format_rewind(flipper_format);
    char cnt_str[24];
    snprintf(cnt_str, sizeof(cnt_str), "%02X %02X %02X %02X",
        (unsigned int)((instance.decrypted_counter >> 24) & 0x0F),
        (unsigned int)((instance.decrypted_counter >> 16) & 0xFF),
        (unsigned int)((instance.decrypted_counter >> 8) & 0xFF),
        (unsigned int)(instance.decrypted_counter & 0xFF));
    flipper_format_insert_or_update_string_cstr(flipper_format, "Cnt", cnt_str);

    flipper_format_rewind(flipper_format);
    char btn_str[8];
    snprintf(btn_str, sizeof(btn_str), "%02X", (unsigned int)instance.decrypted_button);
    flipper_format_insert_or_update_string_cstr(flipper_format, "Btn", btn_str);

    flipper_format_rewind(flipper_format);
    flipper_format_insert_or_update_string_cstr(flipper_format, "Type", "36");

    flipper_format_rewind(flipper_format);
    char crc_str[12];
    snprintf(crc_str, sizeof(crc_str), "%02X %02X",
        (unsigned int)((instance.decrypted_crc >> 8) & 0xFF),
        (unsigned int)(instance.decrypted_crc & 0xFF));
    flipper_format_insert_or_update_string_cstr(flipper_format, "CRC", crc_str);

    flipper_format_rewind(flipper_format);
    char seed_str[16];
    snprintf(seed_str, sizeof(seed_str), "%02X %02X %02X",
        (unsigned int)((instance.decrypted_serial >> 16) & 0xFF),
        (unsigned int)((instance.decrypted_serial >> 8) & 0xFF),
        (unsigned int)(instance.decrypted_serial & 0xFF));
    flipper_format_insert_or_update_string_cstr(flipper_format, "Seed", seed_str);

    if(result_str != NULL) {
        furi_string_printf(result_str,
            "Decrypted (ext)!\nType: 36\nKey: %08lX",
            counter);
    }
    return true;
}
