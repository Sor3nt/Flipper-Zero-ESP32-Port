#pragma once

#include "base.h"

#define SUBGHZ_PROTOCOL_KIA_V6_NAME "KIA/HYU V6"

typedef struct SubGhzProtocolDecoderKiaV6 SubGhzProtocolDecoderKiaV6;
typedef struct SubGhzProtocolEncoderKiaV6 SubGhzProtocolEncoderKiaV6;

extern const SubGhzProtocol subghz_protocol_kia_v6;

void* subghz_protocol_decoder_kia_v6_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_kia_v6_free(void* context);
void subghz_protocol_decoder_kia_v6_reset(void* context);
void subghz_protocol_decoder_kia_v6_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_kia_v6_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_kia_v6_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus subghz_protocol_decoder_kia_v6_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_kia_v6_get_string(void* context, FuriString* output);

void* subghz_protocol_encoder_kia_v6_alloc(SubGhzEnvironment* environment);
void subghz_protocol_encoder_kia_v6_free(void* context);
SubGhzProtocolStatus subghz_protocol_encoder_kia_v6_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_encoder_kia_v6_stop(void* context);
LevelDuration subghz_protocol_encoder_kia_v6_yield(void* context);
