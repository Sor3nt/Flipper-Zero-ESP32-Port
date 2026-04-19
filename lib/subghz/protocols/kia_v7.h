#pragma once

#include "base.h"

#define SUBGHZ_PROTOCOL_KIA_V7_NAME "Kia V7"

typedef struct SubGhzProtocolDecoderKiaV7 SubGhzProtocolDecoderKiaV7;
typedef struct SubGhzProtocolEncoderKiaV7 SubGhzProtocolEncoderKiaV7;

extern const SubGhzProtocol subghz_protocol_kia_v7;

void* subghz_protocol_decoder_kia_v7_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_kia_v7_free(void* context);
void subghz_protocol_decoder_kia_v7_reset(void* context);
void subghz_protocol_decoder_kia_v7_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_kia_v7_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_kia_v7_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_kia_v7_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_kia_v7_get_string(void* context, FuriString* output);

void* subghz_protocol_encoder_kia_v7_alloc(SubGhzEnvironment* environment);
void subghz_protocol_encoder_kia_v7_free(void* context);
SubGhzProtocolStatus
    subghz_protocol_encoder_kia_v7_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_encoder_kia_v7_stop(void* context);
LevelDuration subghz_protocol_encoder_kia_v7_yield(void* context);
