#pragma once

#include "base.h"
#include "../blocks/math.h"

#define SUBGHZ_PROTOCOL_KIA_V1_NAME "KIA/HYU V1"

typedef struct SubGhzProtocolDecoderKiaV1 SubGhzProtocolDecoderKiaV1;
typedef struct SubGhzProtocolEncoderKiaV1 SubGhzProtocolEncoderKiaV1;

extern const SubGhzProtocol subghz_protocol_kia_v1;

// Decoder functions
void* subghz_protocol_decoder_kia_v1_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_kia_v1_free(void* context);
void subghz_protocol_decoder_kia_v1_reset(void* context);
void subghz_protocol_decoder_kia_v1_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_kia_v1_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_kia_v1_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_kia_v1_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_kia_v1_get_string(void* context, FuriString* output);

// Encoder functions
void* subghz_protocol_encoder_kia_v1_alloc(SubGhzEnvironment* environment);
void subghz_protocol_encoder_kia_v1_free(void* context);
SubGhzProtocolStatus subghz_protocol_encoder_kia_v1_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_encoder_kia_v1_stop(void* context);
LevelDuration subghz_protocol_encoder_kia_v1_yield(void* context);

// Encoder helper functions for UI
void subghz_protocol_encoder_kia_v1_set_button(void* context, uint8_t button);
void subghz_protocol_encoder_kia_v1_set_counter(void* context, uint16_t counter);
void subghz_protocol_encoder_kia_v1_increment_counter(void* context);
uint16_t subghz_protocol_encoder_kia_v1_get_counter(void* context);
uint8_t subghz_protocol_encoder_kia_v1_get_button(void* context);
