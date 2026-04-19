#pragma once

#include "base.h"
#include "../blocks/math.h"

#define SUBGHZ_PROTOCOL_KIA_V3_V4_NAME "KIA/HYU V3/V4"

typedef struct SubGhzProtocolDecoderKiaV3V4 SubGhzProtocolDecoderKiaV3V4;
typedef struct SubGhzProtocolEncoderKiaV3V4 SubGhzProtocolEncoderKiaV3V4;

extern const SubGhzProtocol subghz_protocol_kia_v3_v4;

void* subghz_protocol_decoder_kia_v3_v4_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_kia_v3_v4_free(void* context);
void subghz_protocol_decoder_kia_v3_v4_reset(void* context);
void subghz_protocol_decoder_kia_v3_v4_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_kia_v3_v4_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_kia_v3_v4_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_kia_v3_v4_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_kia_v3_v4_get_string(void* context, FuriString* output);

void* subghz_protocol_encoder_kia_v3_v4_alloc(SubGhzEnvironment* environment);
void subghz_protocol_encoder_kia_v3_v4_free(void* context);
SubGhzProtocolStatus
    subghz_protocol_encoder_kia_v3_v4_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_encoder_kia_v3_v4_stop(void* context);
LevelDuration subghz_protocol_encoder_kia_v3_v4_yield(void* context);
