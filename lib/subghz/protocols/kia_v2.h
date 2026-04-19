#pragma once

#include "base.h"
#include "../blocks/math.h"
#include <lib/toolbox/manchester_decoder.h>

#define SUBGHZ_PROTOCOL_KIA_V2_NAME "KIA/HYU V2"

typedef struct SubGhzProtocolDecoderKiaV2 SubGhzProtocolDecoderKiaV2;
typedef struct SubGhzProtocolEncoderKiaV2 SubGhzProtocolEncoderKiaV2;

extern const SubGhzProtocol subghz_protocol_kia_v2;

void* subghz_protocol_decoder_kia_v2_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_kia_v2_free(void* context);
void subghz_protocol_decoder_kia_v2_reset(void* context);
void subghz_protocol_decoder_kia_v2_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_kia_v2_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_kia_v2_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus subghz_protocol_decoder_kia_v2_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_kia_v2_get_string(void* context, FuriString* output);

void* subghz_protocol_encoder_kia_v2_alloc(SubGhzEnvironment* environment);
void subghz_protocol_encoder_kia_v2_free(void* context);
SubGhzProtocolStatus subghz_protocol_encoder_kia_v2_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_encoder_kia_v2_stop(void* context);
LevelDuration subghz_protocol_encoder_kia_v2_yield(void* context);
