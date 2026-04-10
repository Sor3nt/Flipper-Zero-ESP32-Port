/**
 * Audi/VAG V1 SubGHz Protocol - 88-bit with Roundtrip Encoder
 *
 * 88-bit protocol: [Serial:32][Button:8][Hop:48]
 * - Serial: 32-bit, constant per remote
 * - Button byte: upper nibble = button ID (0xE=Lock, 0xB=Unlock)
 *                lower nibble bits {0,1} = counter bits {6,7}
 * - Hop: 48-bit weakly scrambled code, contains embedded counter bits
 *
 * Counter: 8-bit (0-255), shared across buttons, DECREMENTS per press
 *   counter[0:3] = hop bits {42,43,44,45}
 *   counter[4:5] = hop bits {46,47}
 *   counter[6:7] = btn_lo bits {0,1}
 *
 * HOP structure: Feistel cipher with proprietary permutations.
 *   HOP = L(24 upper) || R(24 lower)
 *   Each press applies one Feistel step: L' = sigma(L), R' = R ^ F(L)
 *   sigma and F are 256-entry lookup tables (NOT algebraic formulas).
 *
 * Roundtrip: precomputed lookup table for one epoch of one remote.
 *   new_hop = actual_hop XOR (predict[ctr_from] XOR predict[ctr_to])
 *   Only valid when the signal belongs to the same epoch as the table.
 *
 * Physical layer: Manchester, 260/520us, 3000us sync, 2000us header, ~15 preamble pairs
 */

#include "audi_v1.h"

#include <lib/toolbox/manchester_decoder.h>
#include <lib/toolbox/manchester_encoder.h>

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

#include "../blocks/custom_btn_i.h"

#include <furi.h>
#include <furi_hal.h>

#define TAG "SubGhzProtocolAudiV1"

// Timing constants not in SubGhzBlockConst
#define AUDI_V1_TE_SYNC         3000
#define AUDI_V1_TE_HEADER       2000
#define AUDI_V1_TE_SYNC_DELTA   800
#define AUDI_V1_TE_HEADER_DELTA 500
#define AUDI_V1_DATA_BITS       88
#define AUDI_V1_PREAMBLE_COUNT  20

// Button codes (upper nibble of button byte)
#define AUDI_V1_BTN_LOCK   0x0E
#define AUDI_V1_BTN_UNLOCK 0x0B

static const SubGhzBlockConst subghz_protocol_audi_v1_const = {
    .te_short = 260,
    .te_long = 520,
    .te_delta = 70,
    .min_count_bit_for_found = 88,
};

// ============================================================================
// ROUNDTRIP PREDICTION TABLE
// 256 entries x 48-bit HOP values for one complete epoch (256 presses).
// Captured from real remote (serial F96F98D4), verified from signal data.
//
// Each entry TABLE[ctr] = the exact HOP value at counter=ctr for this epoch.
// Roundtrip formula (within same epoch):
//   new_hop = actual_hop XOR (TABLE[ctr_from] XOR TABLE[ctr_to])
// ============================================================================

static const uint64_t AUDI_V1_HOP_PREDICT[256] = {
    0x0374560849F1ULL, // ctr=0
    0x05545A2AC979ULL, // ctr=1
    0x0B5ECBE721C9ULL, // ctr=2
    0x0D7EC2268187ULL, // ctr=3
    0x13544B1069E4ULL, // ctr=4
    0x1574C01AE5CFULL, // ctr=5
    0x1B7CDB6E0D4EULL, // ctr=6
    0x1D5C5F2D41E3ULL, // ctr=7
    0x23744E49A564ULL, // ctr=8
    0x2554406C61ADULL, // ctr=9
    0x2B5ED97B0543ULL, // ctr=10
    0x2D7ED87AF595ULL, // ctr=11
    0x33545CDA4D3DULL, // ctr=12
    0x3574D8DA4597ULL, // ctr=13
    0x3B74CC3C153BULL, // ctr=14
    0x3D546F2C79C2ULL, // ctr=15
    0x4354C81B0154ULL, // ctr=16
    0x45744A10956FULL, // ctr=17
    0x4B7C59CE0938ULL, // ctr=18
    0x4D5CDC84C911ULL, // ctr=19
    0x537442A9015CULL, // ctr=20
    0x5554EEA08D53ULL, // ctr=21
    0x5B5EDE4F0D08ULL, // ctr=22
    0x5D7E7726F112ULL, // ctr=23
    0x6354DA5A5573ULL, // ctr=24
    0x65747E52218DULL, // ctr=25
    0x6B7449866D2CULL, // ctr=26
    0x6D54CB96996CULL, // ctr=27
    0x737458CC9597ULL, // ctr=28
    0x7554D7EC31BAULL, // ctr=29
    0x7B5ECE5B0120ULL, // ctr=30
    0x7D7E4F72A906ULL, // ctr=31
    0x8375D71C05A9ULL, // ctr=32
    0x8575D22FE975ULL, // ctr=33
    0x8B5F5F85AD32ULL, // ctr=34
    0x8D5F7A240518ULL, // ctr=35
    0x93555D00F5FFULL, // ctr=36
    0x95555B4A6D2DULL, // ctr=37
    0x9B7DDC0CE542ULL, // ctr=38
    0x9D7DDC0EA506ULL, // ctr=39
    0xA355CE696DADULL, // ctr=40
    0xA555C97AFD2FULL, // ctr=41
    0xAB7E4F40115AULL, // ctr=42
    0xAD7E4A68CDADULL, // ctr=43
    0xB37546C2AD7EULL, // ctr=44
    0xB5756FC8798FULL, // ctr=45
    0xBB54CF4CA954ULL, // ctr=46
    0xBD54E01C61E5ULL, // ctr=47
    0xC355480AC580ULL, // ctr=48
    0xC555CE42810CULL, // ctr=49
    0xCB7DCAAC3D7CULL, // ctr=50
    0xCD7D4BA4A16FULL, // ctr=51
    0xD3755A8515FDULL, // ctr=52
    0xD57576B68170ULL, // ctr=53
    0xDB5FDE2E896CULL, // ctr=54
    0xDD5FD726698BULL, // ctr=55
    0xE375544AB9A0ULL, // ctr=56
    0xE575D44A45DAULL, // ctr=57
    0xEB54DCF6DDD9ULL, // ctr=58
    0xED5452A48957ULL, // ctr=59
    0xF3554AD8C96CULL, // ctr=60
    0xF5554CE021BCULL, // ctr=61
    0xFB7ECC6B9D2EULL, // ctr=62
    0xFD7ECD6A6DD8ULL, // ctr=63
    0x03155B91EDA1ULL, // ctr=64
    0x0535D0B9ED24ULL, // ctr=65
    0x0B3DDB2D1949ULL, // ctr=66
    0x0D1D5F2DFD0FULL, // ctr=67
    0x1375422C65FDULL, // ctr=68
    0x1555462DC972ULL, // ctr=69
    0x1B5FCF278DB1ULL, // ctr=70
    0x1D7FEE264973ULL, // ctr=71
    0x2315425A7DC3ULL, // ctr=72
    0x2535CE7299A5ULL, // ctr=73
    0x2B34CA7E053EULL, // ctr=74
    0x2D14412C5195ULL, // ctr=75
    0x33755A682DC9ULL, // ctr=76
    0x35555C6AB177ULL, // ctr=77
    0x3B1D4D016997ULL, // ctr=78
    0x3D1CC629E9B3ULL, // ctr=79
    0x4314480121ABULL, // ctr=80
    0x4514C26B6D01ULL, // ctr=81
    0x4B1CD8CD39EFULL, // ctr=82
    0x4D1C5DADF9CCULL, // ctr=83
    0x535450A82942ULL, // ctr=84
    0x55547CADA9EDULL, // ctr=85
    0x5B5CC6221D6AULL, // ctr=86
    0x5D5CEF2BD988ULL, // ctr=87
    0x631450523DDCULL, // ctr=88
    0x6514D476B1F6ULL, // ctr=89
    0x6B15CA8949E0ULL, // ctr=90
    0x6D1460ABE9CFULL, // ctr=91
    0x735448FC2522ULL, // ctr=92
    0x75544EFCB1B6ULL, // ctr=93
    0x7B5CDE6FC9CBULL, // ctr=94
    0x7D5CDF7F2D38ULL, // ctr=95
    0x835C5C359DBFULL, // ctr=96
    0x855C71250910ULL, // ctr=97
    0x8B54DD7A3DD1ULL, // ctr=98
    0x8D54DA32A104ULL, // ctr=99
    0x931D45890DDBULL, // ctr=100
    0x951CCBB1CDAAULL, // ctr=101
    0x9B14CF0DD900ULL, // ctr=102
    0x9D144727D9A4ULL, // ctr=103
    0xA35D4E680945ULL, // ctr=104
    0xA55C4061E9A5ULL, // ctr=105
    0xAB54CD7A01DDULL, // ctr=106
    0xAD54C8E3ADEBULL, // ctr=107
    0xB31C5C4BF9D5ULL, // ctr=108
    0xB51CFD6BB912ULL, // ctr=109
    0xBB1CDE4D8121ULL, // ctr=110
    0xBD1C79250960ULL, // ctr=111
    0xC31550138D8CULL, // ctr=112
    0xC51456299DA7ULL, // ctr=113
    0xCB1CD38DF1ECULL, // ctr=114
    0xCD1CD2AD556FULL, // ctr=115
    0xD354DBAFA9CEULL, // ctr=116
    0xD554D7BD7D02ULL, // ctr=117
    0xDB5C5E706D50ULL, // ctr=118
    0xDD5C7F39E1B2ULL, // ctr=119
    0xE3144E40BDD0ULL, // ctr=120
    0xE5144E6B3D7DULL, // ctr=121
    0xEB14CFCFE982ULL, // ctr=122
    0xED14C9AC4149ULL, // ctr=123
    0xF355CAEAADBFULL, // ctr=124
    0xF554C5E86D75ULL, // ctr=125
    0xFB5C4C6DC5D7ULL, // ctr=126
    0xFD5C4D7D6560ULL, // ctr=127
    0x035CD4CAE137ULL, // ctr=128
    0x055C5EE02151ULL, // ctr=129
    0x0B74484805EDULL, // ctr=130
    0x0D74C0412D42ULL, // ctr=131
    0x137CD6471D74ULL, // ctr=132
    0x157CF375D5ADULL, // ctr=133
    0x1B544A08D14BULL, // ctr=134
    0x1D54632131ADULL, // ctr=135
    0x237CC64D692AULL, // ctr=136
    0x257C4E4E4D83ULL, // ctr=137
    0x2B5D565A4DA0ULL, // ctr=138
    0x2D5CF14185D3ULL, // ctr=139
    0x335CCE1FD1F8ULL, // ctr=140
    0x355CC9245142ULL, // ctr=141
    0x3B74528CB5B3ULL, // ctr=142
    0x3D74533D15A5ULL, // ctr=143
    0x437C42DD7542ULL, // ctr=144
    0x457C46E8B9B9ULL, // ctr=145
    0x4B56DF22FD8AULL, // ctr=146
    0x4D56D623750CULL, // ctr=147
    0x535CC541B9A5ULL, // ctr=148
    0x555CC36A81B6ULL, // ctr=149
    0x5B7447C8DDEAULL, // ctr=150
    0x5D7446CA4D7FULL, // ctr=151
    0x635C528DDDAAULL, // ctr=152
    0x655C5DBD7937ULL, // ctr=153
    0x6B76C5369DE4ULL, // ctr=154
    0x6D76CC2F553AULL, // ctr=155
    0x737CC4464953ULL, // ctr=156
    0x757CED62E5F4ULL, // ctr=157
    0x7B5D54C8F5D8ULL, // ctr=158
    0x7D5C52DBC1FEULL, // ctr=159
    0x835D5EDE5198ULL, // ctr=160
    0x857CD8F01D5BULL, // ctr=161
    0x8B74D34A8D7CULL, // ctr=162
    0x8D545F417929ULL, // ctr=163
    0x937CCA421DEDULL, // ctr=164
    0x955C4E63F586ULL, // ctr=165
    0x9B5546683146ULL, // ctr=166
    0x9D74E7239923ULL, // ctr=167
    0xA35C4C5BE996ULL, // ctr=168
    0xA57CC4721DE5ULL, // ctr=169
    0xAB7DC15269BBULL, // ctr=170
    0xAD5C4A48D9BDULL, // ctr=171
    0xB37DDA0DF57BULL, // ctr=172
    0xB55C5426111DULL, // ctr=173
    0xBB545CAC7DF5ULL, // ctr=174
    0xBD74DD36DD68ULL, // ctr=175
    0xC37CC6C9B196ULL, // ctr=176
    0xC55CEAE8515DULL, // ctr=177
    0xCB574342D5DFULL, // ctr=178
    0xCD764A2A511DULL, // ctr=179
    0xD35DCD550180ULL, // ctr=180
    0xD57C4A5EB59FULL, // ctr=181
    0xDB7440C81DADULL, // ctr=182
    0xDD54CDC95943ULL, // ctr=183
    0xE37DDC8CF1A8ULL, // ctr=184
    0xE55CD2AD5D0CULL, // ctr=185
    0xEB565B1E5D32ULL, // ctr=186
    0xED765A2FF58CULL, // ctr=187
    0xF35CCE5A05A9ULL, // ctr=188
    0xF57C6A72696FULL, // ctr=189
    0xFB7D5DC835B1ULL, // ctr=190
    0xFD5CFFDB5547ULL, // ctr=191
    0x031CDC4BD5CBULL, // ctr=192
    0x051C52699125ULL, // ctr=193
    0x0B145E475DCDULL, // ctr=194
    0x0D14DE6D1925ULL, // ctr=195
    0x135DD87E651BULL, // ctr=196
    0x155CDD75F98EULL, // ctr=197
    0x1B5454641DF4ULL, // ctr=198
    0x1D5477AD8D88ULL, // ctr=199
    0x231DC4C1913CULL, // ctr=200
    0x251C6CE1D5F7ULL, // ctr=201
    0x2B1C4E5DF94BULL, // ctr=202
    0x2D1CE17DD5EEULL, // ctr=203
    0x335CCA2BB9A1ULL, // ctr=204
    0x355CCD716122ULL, // ctr=205
    0x3B544C2355C3ULL, // ctr=206
    0x3D544D30BD3FULL, // ctr=207
    0x435556F5D5F6ULL, // ctr=208
    0x45547AF53139ULL, // ctr=209
    0x4B5CD36EEDD1ULL, // ctr=210
    0x4D5CDA2E6516ULL, // ctr=211
    0x5314CD41B1ECULL, // ctr=212
    0x5514C7617908ULL, // ctr=213
    0x5B1C4BC5D58AULL, // ctr=214
    0x5D1C4AE75D27ULL, // ctr=215
    0x63544EB1EDB3ULL, // ctr=216
    0x655449E00907ULL, // ctr=217
    0x6B5CC9322D77ULL, // ctr=218
    0x6D5CC82291DCULL, // ctr=219
    0x7315C642B1C5ULL, // ctr=220
    0x7514C76299CBULL, // ctr=221
    0x7B145ED4F586ULL, // ctr=222
    0x7D1450F65900ULL, // ctr=223
    0x831C465D5143ULL, // ctr=224
    0x851CC87919A7ULL, // ctr=225
    0x8B14D94375E6ULL, // ctr=226
    0x8D145569F9CAULL, // ctr=227
    0x935CDE686D82ULL, // ctr=228
    0x955C5263E58BULL, // ctr=229
    0x9B54482189B9ULL, // ctr=230
    0x9D54CBAA152BULL, // ctr=231
    0xA31C5ED575D7ULL, // ctr=232
    0xA51CDAF5BDBDULL, // ctr=233
    0xAB1CCD4BF95EULL, // ctr=234
    0xAD1C6E69D9F9ULL, // ctr=235
    0xB35CCE2635A4ULL, // ctr=236
    0xB55C4027B1A9ULL, // ctr=237
    0xBB555A3C253BULL, // ctr=238
    0xBD54DB26C547ULL, // ctr=239
    0xC354C8E0B59CULL, // ctr=240
    0xC554E4E1292BULL, // ctr=241
    0xCB5C5F2B3540ULL, // ctr=242
    0xCD5C7E2AF5A6ULL, // ctr=243
    0xD314C55535F4ULL, // ctr=244
    0xD51442757D1DULL, // ctr=245
    0xDB1C50C135F5ULL, // ctr=246
    0xDD1CD5E37110ULL, // ctr=247
    0xE354D8A5E1BDULL, // ctr=248
    0xE554D6AE1D42ULL, // ctr=249
    0xEB5D4F26F5BCULL, // ctr=250
    0xED5C4E372D73ULL, // ctr=251
    0xF314CC52DD32ULL, // ctr=252
    0xF5146C73B9D1ULL, // ctr=253
    0xFB144BC035C7ULL, // ctr=254
    0xFD14C9E0194FULL, // ctr=255
};

// ============================================================================
// DATA STRUCTURES
// ============================================================================

typedef enum {
    AudiV1DecoderStepReset = 0,
    AudiV1DecoderStepWaitHeader,
    AudiV1DecoderStepPreamble,
    AudiV1DecoderStepData,
} AudiV1DecoderStep;

struct SubGhzProtocolDecoderAudiV1 {
    SubGhzProtocolDecoderBase base;

    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    uint16_t preamble_count;
    ManchesterState manchester_saved_state;
    uint64_t hop;
    uint8_t btn_byte;
};

struct SubGhzProtocolEncoderAudiV1 {
    SubGhzProtocolEncoderBase base;

    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;

    uint64_t hop;
    uint8_t btn_byte;
    uint32_t serial;
};

// ============================================================================
// PROTOCOL DEFINITION
// ============================================================================

static SubGhzBlockGeneric* subghz_protocol_decoder_audi_v1_get_generic(void* context) {
    SubGhzProtocolDecoderAudiV1* instance = context;
    return &instance->generic;
}

const SubGhzProtocolDecoder subghz_protocol_audi_v1_decoder = {
    .alloc = subghz_protocol_decoder_audi_v1_alloc,
    .free = subghz_protocol_decoder_audi_v1_free,

    .feed = subghz_protocol_decoder_audi_v1_feed,
    .reset = subghz_protocol_decoder_audi_v1_reset,

    .get_hash_data = subghz_protocol_decoder_audi_v1_get_hash_data,
    .serialize = subghz_protocol_decoder_audi_v1_serialize,
    .deserialize = subghz_protocol_decoder_audi_v1_deserialize,
    .get_string = subghz_protocol_decoder_audi_v1_get_string,
    .get_generic = subghz_protocol_decoder_audi_v1_get_generic,
};

const SubGhzProtocolEncoder subghz_protocol_audi_v1_encoder = {
    .alloc = subghz_protocol_encoder_audi_v1_alloc,
    .free = subghz_protocol_encoder_audi_v1_free,

    .deserialize = subghz_protocol_encoder_audi_v1_deserialize,
    .stop = subghz_protocol_encoder_audi_v1_stop,
    .yield = subghz_protocol_encoder_audi_v1_yield,
};

const SubGhzProtocol subghz_protocol_audi_v1 = {
    .name = SUBGHZ_PROTOCOL_AUDI_V1_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,

    .decoder = &subghz_protocol_audi_v1_decoder,
    .encoder = &subghz_protocol_audi_v1_encoder,
};

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/**
 * Extract full 8-bit counter from hop code + button byte lower nibble.
 */
static uint8_t audi_v1_extract_counter(uint64_t hop, uint8_t btn_byte) {
    uint8_t hop_bits = (hop >> 42) & 0x3F;
    uint8_t btn_lo = btn_byte & 0x03;
    return hop_bits | (btn_lo << 6);
}

static const char* audi_v1_get_button_name(uint8_t btn_id) {
    switch(btn_id) {
    case AUDI_V1_BTN_LOCK:
        return "Lock";
    case AUDI_V1_BTN_UNLOCK:
        return "Unlock";
    default:
        return "Unknown";
    }
}

/**
 * Get button code based on D-Pad selection (custom button support).
 * OK=original, UP=Unlock(0xB), DOWN=Lock(0xE)
 */
static uint8_t audi_v1_get_btn_code(uint8_t original_btn_id) {
    uint8_t custom_btn = subghz_custom_btn_get();

    switch(custom_btn) {
    case SUBGHZ_CUSTOM_BTN_OK:
        return original_btn_id;
    case SUBGHZ_CUSTOM_BTN_UP:
        return AUDI_V1_BTN_UNLOCK;
    case SUBGHZ_CUSTOM_BTN_DOWN:
        return AUDI_V1_BTN_LOCK;
    default:
        return original_btn_id;
    }
}

/**
 * Roundtrip: compute new hop for a different counter value.
 * new_hop = actual_hop XOR TABLE[ctr_from] XOR TABLE[ctr_to]
 * Only valid within the same 256-press epoch as the predict table.
 */
static uint64_t audi_v1_roundtrip(uint64_t actual_hop, uint8_t ctr_from, uint8_t ctr_to) {
    uint64_t mask = AUDI_V1_HOP_PREDICT[ctr_from] ^ AUDI_V1_HOP_PREDICT[ctr_to];
    return (actual_hop ^ mask) & 0xFFFFFFFFFFFFULL;
}

/**
 * Populate generic fields from hop and btn_byte.
 */
static void audi_v1_extract_fields(SubGhzBlockGeneric* generic, uint64_t hop, uint8_t btn_byte) {
    generic->serial = (uint32_t)(generic->data >> 32);
    generic->btn = (btn_byte >> 4) & 0x0F;
    generic->cnt = audi_v1_extract_counter(hop, btn_byte);
}

// ============================================================================
// DECODER IMPLEMENTATION
// ============================================================================

void* subghz_protocol_decoder_audi_v1_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderAudiV1* instance = malloc(sizeof(SubGhzProtocolDecoderAudiV1));
    instance->base.protocol = &subghz_protocol_audi_v1;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_audi_v1_free(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderAudiV1* instance = context;
    free(instance);
}

void subghz_protocol_decoder_audi_v1_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderAudiV1* instance = context;
    instance->decoder.parser_step = AudiV1DecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->preamble_count = 0;
    manchester_advance(
        instance->manchester_saved_state,
        ManchesterEventReset,
        &instance->manchester_saved_state,
        NULL);
}

/**
 * Complete signal processing when we have 88 bits.
 */
static void audi_v1_decoder_check_complete(SubGhzProtocolDecoderAudiV1* instance) {
    if(instance->decoder.decode_count_bit < AUDI_V1_DATA_BITS) return;

    // generic.data = first 64 bits (saved at bit 64)
    // decode_data = remaining 24 bits (bits 65-88)
    uint32_t remaining_24 = instance->decoder.decode_data & 0xFFFFFF;

    instance->btn_byte = (instance->generic.data >> 24) & 0xFF;
    instance->hop = ((instance->generic.data & 0xFFFFFFULL) << 24) | remaining_24;

    instance->generic.data_count_bit = AUDI_V1_DATA_BITS;
    audi_v1_extract_fields(&instance->generic, instance->hop, instance->btn_byte);

    uint8_t btn_id = (instance->btn_byte >> 4) & 0x0F;
    subghz_block_generic_global.current_btn = btn_id;
    subghz_block_generic_global.btn_length_bit = 4;
    FURI_LOG_I(
        TAG,
        "Decoded 88-bit: Sn=%08lX Btn=%s(0x%02X) Cnt=%lu Hop=%012llX",
        (unsigned long)instance->generic.serial,
        audi_v1_get_button_name(btn_id),
        instance->btn_byte,
        (unsigned long)instance->generic.cnt,
        (unsigned long long)instance->hop);

    if(instance->base.callback) {
        instance->base.callback(&instance->base, instance->base.context);
    }

    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->decoder.parser_step = AudiV1DecoderStepReset;
}

void subghz_protocol_decoder_audi_v1_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderAudiV1* instance = context;

    switch(instance->decoder.parser_step) {
    case AudiV1DecoderStepReset:
        // Wait for SYNC: ~3000us LOW pulse
        if(!level && DURATION_DIFF(duration, AUDI_V1_TE_SYNC) <= AUDI_V1_TE_SYNC_DELTA) {
            instance->decoder.parser_step = AudiV1DecoderStepWaitHeader;
        }
        break;

    case AudiV1DecoderStepWaitHeader:
        // Expect HEADER: ~2000us HIGH pulse
        if(level && DURATION_DIFF(duration, AUDI_V1_TE_HEADER) <= AUDI_V1_TE_HEADER_DELTA) {
            instance->decoder.parser_step = AudiV1DecoderStepPreamble;
            instance->preamble_count = 0;
        } else {
            instance->decoder.parser_step = AudiV1DecoderStepReset;
        }
        break;

    case AudiV1DecoderStepPreamble:
        // Skip SHORT preamble pulses, transition to DATA on first LONG
        if(DURATION_DIFF(duration, subghz_protocol_audi_v1_const.te_short) <=
           subghz_protocol_audi_v1_const.te_delta) {
            instance->preamble_count++;
        } else if(
            DURATION_DIFF(duration, subghz_protocol_audi_v1_const.te_long) <=
                subghz_protocol_audi_v1_const.te_delta &&
            instance->preamble_count >= AUDI_V1_PREAMBLE_COUNT) {
            instance->decoder.parser_step = AudiV1DecoderStepData;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->generic.data = 0;

            manchester_advance(
                instance->manchester_saved_state,
                ManchesterEventReset,
                &instance->manchester_saved_state,
                NULL);

            // Process trigger LONG as separator (advance state, don't count as data bit)
            ManchesterEvent event = level ? ManchesterEventLongHigh : ManchesterEventLongLow;
            bool data;
            manchester_advance(
                instance->manchester_saved_state, event, &instance->manchester_saved_state, &data);
        } else {
            instance->decoder.parser_step = AudiV1DecoderStepReset;
        }
        break;

    case AudiV1DecoderStepData: {
        ManchesterEvent event = ManchesterEventReset;

        if(DURATION_DIFF(duration, subghz_protocol_audi_v1_const.te_short) <=
           subghz_protocol_audi_v1_const.te_delta) {
            event = level ? ManchesterEventShortHigh : ManchesterEventShortLow;
        } else if(
            DURATION_DIFF(duration, subghz_protocol_audi_v1_const.te_long) <=
            subghz_protocol_audi_v1_const.te_delta) {
            event = level ? ManchesterEventLongHigh : ManchesterEventLongLow;
        }

        if(event != ManchesterEventReset) {
            bool data;
            bool data_ok = manchester_advance(
                instance->manchester_saved_state, event, &instance->manchester_saved_state, &data);

            if(data_ok) {
                instance->decoder.decode_data = (instance->decoder.decode_data << 1) | data;
                instance->decoder.decode_count_bit++;

                // At 64 bits, save key and reset decode_data for remaining bits
                if(instance->decoder.decode_count_bit == 64) {
                    instance->generic.data = instance->decoder.decode_data;
                    instance->decoder.decode_data = 0;
                }
            }
        }

        // Frame completed at 88 bits
        if(instance->decoder.decode_count_bit >= AUDI_V1_DATA_BITS) {
            audi_v1_decoder_check_complete(instance);
            break;
        }

        // Invalid timing ends data phase
        if(event == ManchesterEventReset) {
            if(instance->decoder.decode_count_bit >= 64 && instance->generic.data != 0) {
                FURI_LOG_W(
                    TAG,
                    "Partial frame: %lu bits (need 88)",
                    (unsigned long)instance->decoder.decode_count_bit);
            }
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->decoder.parser_step = AudiV1DecoderStepReset;
        }
    } break;

    default:
        instance->decoder.parser_step = AudiV1DecoderStepReset;
        break;
    }
}

uint8_t subghz_protocol_decoder_audi_v1_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderAudiV1* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

// ============================================================================
// SERIALIZATION
// ============================================================================

SubGhzProtocolStatus subghz_protocol_decoder_audi_v1_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderAudiV1* instance = context;

    audi_v1_extract_fields(&instance->generic, instance->hop, instance->btn_byte);

    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);

    if(ret != SubGhzProtocolStatusOk) {
        FURI_LOG_E(TAG, "Failed to serialize base fields");
        return ret;
    }

    // Write full 48-bit hop as 6-byte hex field
    uint8_t hop_data[6];
    for(size_t i = 0; i < 6; i++) {
        hop_data[5 - i] = (instance->hop >> (i * 8)) & 0xFF;
    }
    if(!flipper_format_write_hex(flipper_format, "Hop", hop_data, 6)) {
        FURI_LOG_E(TAG, "Failed to write Hop");
        return SubGhzProtocolStatusError;
    }

    return SubGhzProtocolStatusOk;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_audi_v1_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderAudiV1* instance = context;

    SubGhzProtocolStatus ret =
        subghz_block_generic_deserialize(&instance->generic, flipper_format);

    if(ret != SubGhzProtocolStatusOk) {
        FURI_LOG_E(TAG, "Failed to deserialize base fields");
        return ret;
    }

    if(instance->generic.data_count_bit != AUDI_V1_DATA_BITS) {
        FURI_LOG_E(TAG, "Invalid bit count: %lu", (unsigned long)instance->generic.data_count_bit);
        return SubGhzProtocolStatusErrorValueBitCount;
    }

    instance->generic.protocol_name = instance->base.protocol->name;

    // Try to read full 48-bit Hop field
    uint8_t hop_data[6] = {0};
    if(flipper_format_read_hex(flipper_format, "Hop", hop_data, 6)) {
        instance->hop = 0;
        for(size_t i = 0; i < 6; i++) {
            instance->hop = (instance->hop << 8) | hop_data[i];
        }
    } else {
        // Backward compatible: reconstruct partial hop from Key's lower 24 bits
        FURI_LOG_W(TAG, "Hop field missing, reconstructing from Key");
        instance->hop = (instance->generic.data & 0xFFFFFFULL) << 24;
    }

    instance->btn_byte = (instance->generic.data >> 24) & 0xFF;
    audi_v1_extract_fields(&instance->generic, instance->hop, instance->btn_byte);

    // Set up custom button support
    uint8_t btn_id = (instance->btn_byte >> 4) & 0x0F;
    if(subghz_custom_btn_get_original() == 0) {
        subghz_custom_btn_set_original(btn_id);
    }
    subghz_custom_btn_set_max(2); // OK, UP(Unlock), DOWN(Lock)
    subghz_block_generic_global.btn_length_bit = 4;

    return SubGhzProtocolStatusOk;
}

// ============================================================================
// DISPLAY
// ============================================================================

void subghz_protocol_decoder_audi_v1_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderAudiV1* instance = context;

    audi_v1_extract_fields(&instance->generic, instance->hop, instance->btn_byte);

    uint8_t btn_id = (instance->btn_byte >> 4) & 0x0F;
    const char* btn_name = audi_v1_get_button_name(btn_id);

    uint32_t key_hi = (uint32_t)(instance->generic.data >> 32);
    uint32_t key_lo = (uint32_t)(instance->generic.data & 0xFFFFFFFF);
    uint32_t hop_hi = (uint32_t)(instance->hop >> 24);
    uint32_t hop_lo = (uint32_t)(instance->hop & 0xFFFFFF);

    furi_string_cat_printf(
        output,
        "%s 88bit\r\n"
        "Key:%08lX%08lX\r\n"
        "Sn:%08lX Btn:%s\r\n"
        "Cnt:%03lu Hop:%06lX%06lX\r\n",
        instance->generic.protocol_name,
        (unsigned long)key_hi,
        (unsigned long)key_lo,
        (unsigned long)instance->generic.serial,
        btn_name,
        (unsigned long)instance->generic.cnt,
        (unsigned long)hop_hi,
        (unsigned long)hop_lo);
}

// ============================================================================
// ENCODER IMPLEMENTATION
// ============================================================================

void* subghz_protocol_encoder_audi_v1_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderAudiV1* instance = malloc(sizeof(SubGhzProtocolEncoderAudiV1));

    instance->base.protocol = &subghz_protocol_audi_v1;
    instance->generic.protocol_name = instance->base.protocol->name;

    instance->encoder.repeat = 3;
    instance->encoder.size_upload = 256;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void subghz_protocol_encoder_audi_v1_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderAudiV1* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

/**
 * Build one complete frame into the upload buffer.
 * Signal: [SYNC -3000us][HEADER +2000us][15x SHORT pairs][TRIGGER LONG]
 *         [Manchester 88 data bits][END GAP]
 */
static void subghz_protocol_encoder_audi_v1_get_upload(SubGhzProtocolEncoderAudiV1* instance) {
    furi_assert(instance);
    size_t index = 0;

    // Button handling: global override first, then D-pad
    uint8_t original_btn_id = (instance->btn_byte >> 4) & 0x0F;
    uint8_t target_btn_id = original_btn_id;
    if(!subghz_block_generic_global_button_override_get(&target_btn_id)) {
        target_btn_id = audi_v1_get_btn_code(original_btn_id);
    }

    // Extract current counter
    uint8_t current_ctr = audi_v1_extract_counter(instance->hop, instance->btn_byte);

    // Counter handling: rolling_counter_mult or default decrement
    uint32_t cnt32 = current_ctr;
    if(furi_hal_subghz_get_rolling_counter_mult() != -0x7FFFFFFF) {
        if(!subghz_block_generic_global_counter_override_get(&cnt32)) {
            int32_t new_cnt = (int32_t)cnt32 + furi_hal_subghz_get_rolling_counter_mult();
            cnt32 = (uint32_t)(new_cnt & 0xFF);
        }
    } else {
        cnt32 = (cnt32 - 1) & 0xFF;
    }
    uint8_t next_ctr = cnt32 & 0xFF;

    // Roundtrip: compute new hop for next counter
    uint64_t new_hop = audi_v1_roundtrip(instance->hop, current_ctr, next_ctr);

    // Update btn_byte: upper nibble = button ID, lower nibble bits {0,1} = counter bits {6,7}
    uint8_t btn_lo_preserved = instance->btn_byte & 0x0C;
    uint8_t btn_lo_counter = (next_ctr >> 6) & 0x03;
    uint8_t new_btn_byte = (target_btn_id << 4) | ((btn_lo_preserved | btn_lo_counter) & 0x0F);

    FURI_LOG_I(TAG, "Roundtrip: ctr %d->%d, btn 0x%02X", current_ctr, next_ctr, new_btn_byte);

    // Update instance state
    instance->hop = new_hop;
    instance->btn_byte = new_btn_byte;
    instance->generic.cnt = next_ctr;
    instance->generic.btn = target_btn_id;
    instance->generic.serial = instance->serial;

    // Rebuild generic.data: serial[32] + btn_byte[8] + hop_upper24[24]
    instance->generic.data = ((uint64_t)instance->serial << 32) | ((uint64_t)new_btn_byte << 24) |
                             ((new_hop >> 24) & 0xFFFFFF);
    instance->generic.data_count_bit = AUDI_V1_DATA_BITS;

    // === BUILD UPLOAD ===

    // SYNC: LOW 3000us
    instance->encoder.upload[index++] = level_duration_make(false, (uint32_t)AUDI_V1_TE_SYNC);

    // HEADER: HIGH 2000us
    instance->encoder.upload[index++] = level_duration_make(true, (uint32_t)AUDI_V1_TE_HEADER);

    // PREAMBLE: 15 SHORT pairs (LOW-HIGH)
    for(int p = 0; p < 15; p++) {
        instance->encoder.upload[index++] =
            level_duration_make(false, (uint32_t)subghz_protocol_audi_v1_const.te_short);
        instance->encoder.upload[index++] =
            level_duration_make(true, (uint32_t)subghz_protocol_audi_v1_const.te_short);
    }

    // TRIGGER: LONG LOW (follows naturally after last preamble HIGH)
    instance->encoder.upload[index++] =
        level_duration_make(false, (uint32_t)subghz_protocol_audi_v1_const.te_long);

    // MANCHESTER DATA: 88 bits
    // After trigger LONG LOW, start Mid1 so first emission is HIGH (no merge with trigger LOW)
    uint8_t manchester_state = ManchesterStateMid1;

    for(int i = 87; i >= 0; i--) {
        bool b;
        if(i >= 24) {
            // Bits 87..24: from generic.data (bits 63..0)
            b = (instance->generic.data >> (i - 24)) & 1;
        } else {
            // Bits 23..0: from hop lower 24 bits
            b = (new_hop >> i) & 1;
        }

        switch(manchester_state) {
        case ManchesterStateMid1:
            if(b == 0) {
                instance->encoder.upload[index++] =
                    level_duration_make(true, (uint32_t)subghz_protocol_audi_v1_const.te_long);
                manchester_state = ManchesterStateMid0;
            } else {
                instance->encoder.upload[index++] =
                    level_duration_make(true, (uint32_t)subghz_protocol_audi_v1_const.te_short);
                instance->encoder.upload[index++] =
                    level_duration_make(false, (uint32_t)subghz_protocol_audi_v1_const.te_short);
                manchester_state = ManchesterStateMid1;
            }
            break;
        case ManchesterStateMid0:
            if(b == 0) {
                instance->encoder.upload[index++] =
                    level_duration_make(false, (uint32_t)subghz_protocol_audi_v1_const.te_short);
                instance->encoder.upload[index++] =
                    level_duration_make(true, (uint32_t)subghz_protocol_audi_v1_const.te_short);
                manchester_state = ManchesterStateMid0;
            } else {
                instance->encoder.upload[index++] =
                    level_duration_make(false, (uint32_t)subghz_protocol_audi_v1_const.te_long);
                manchester_state = ManchesterStateMid1;
            }
            break;
        default:
            manchester_state = ManchesterStateMid1;
            break;
        }
    }

    // END GAP - use opposite level to prevent merging with last Manchester emission
    // Mid0 → last emission was HIGH, so gap must be LOW
    // Mid1 → last emission was LOW, so gap must be HIGH
    bool end_level = (manchester_state == ManchesterStateMid1);
    instance->encoder.upload[index++] = level_duration_make(end_level, (uint32_t)6000);

    instance->encoder.size_upload = index;
}

SubGhzProtocolStatus
    subghz_protocol_encoder_audi_v1_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolEncoderAudiV1* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;
    do {
        if(subghz_block_generic_deserialize(&instance->generic, flipper_format) !=
           SubGhzProtocolStatusOk) {
            FURI_LOG_E(TAG, "Deserialize error");
            break;
        }

        if(instance->generic.data_count_bit != AUDI_V1_DATA_BITS) {
            FURI_LOG_E(
                TAG, "Invalid bit count: %lu", (unsigned long)instance->generic.data_count_bit);
            res = SubGhzProtocolStatusErrorValueBitCount;
            break;
        }

        // Read full 48-bit hop
        uint8_t hop_data[6] = {0};
        if(!flipper_format_rewind(flipper_format)) {
            FURI_LOG_E(TAG, "Rewind error");
            break;
        }
        if(flipper_format_read_hex(flipper_format, "Hop", hop_data, 6)) {
            instance->hop = 0;
            for(size_t i = 0; i < 6; i++) {
                instance->hop = (instance->hop << 8) | hop_data[i];
            }
        } else {
            // Backward compatible
            FURI_LOG_W(TAG, "Hop field missing, reconstructing from Key");
            instance->hop = (instance->generic.data & 0xFFFFFFULL) << 24;
        }

        instance->btn_byte = (instance->generic.data >> 24) & 0xFF;
        instance->serial = (uint32_t)(instance->generic.data >> 32);

        // Set up custom button support
        uint8_t original_btn_id = (instance->btn_byte >> 4) & 0x0F;
        if(subghz_custom_btn_get_original() == 0) {
            subghz_custom_btn_set_original(original_btn_id);
        }
        subghz_custom_btn_set_max(2);
        subghz_block_generic_global.btn_length_bit = 4;

        // Optional repeat parameter
        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        // Build upload (handles counter increment + roundtrip internally)
        subghz_protocol_encoder_audi_v1_get_upload(instance);

        // Write back updated Key
        if(!flipper_format_rewind(flipper_format)) {
            FURI_LOG_E(TAG, "Rewind error");
            break;
        }
        uint8_t key_data[sizeof(uint64_t)] = {0};
        for(size_t i = 0; i < sizeof(uint64_t); i++) {
            key_data[sizeof(uint64_t) - i - 1] = (instance->generic.data >> (i * 8)) & 0xFF;
        }
        if(!flipper_format_update_hex(flipper_format, "Key", key_data, sizeof(uint64_t))) {
            FURI_LOG_E(TAG, "Unable to update Key");
            break;
        }

        // Write back updated Hop
        uint8_t new_hop_data[6];
        for(size_t i = 0; i < 6; i++) {
            new_hop_data[5 - i] = (instance->hop >> (i * 8)) & 0xFF;
        }
        if(!flipper_format_update_hex(flipper_format, "Hop", new_hop_data, 6)) {
            FURI_LOG_W(TAG, "Unable to update Hop (may not exist yet)");
        }

        instance->encoder.is_running = true;
        res = SubGhzProtocolStatusOk;
    } while(false);

    return res;
}

void subghz_protocol_encoder_audi_v1_stop(void* context) {
    SubGhzProtocolEncoderAudiV1* instance = context;
    instance->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_audi_v1_yield(void* context) {
    SubGhzProtocolEncoderAudiV1* instance = context;

    if(instance->encoder.repeat == 0 || !instance->encoder.is_running) {
        instance->encoder.is_running = false;
        return level_duration_reset();
    }

    LevelDuration ret = instance->encoder.upload[instance->encoder.front];

    if(++instance->encoder.front == instance->encoder.size_upload) {
        if(!subghz_block_generic_global.endless_tx) instance->encoder.repeat--;
        instance->encoder.front = 0;
    }

    return ret;
}
