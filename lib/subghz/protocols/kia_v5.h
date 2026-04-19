#pragma once

#include "base.h"
#include "../blocks/math.h"

#define SUBGHZ_PROTOCOL_KIA_V5_NAME "KIA/HYU V5"

typedef struct SubGhzProtocolDecoderKiaV5 SubGhzProtocolDecoderKiaV5;
typedef struct SubGhzProtocolEncoderKiaV5 SubGhzProtocolEncoderKiaV5;

extern const SubGhzProtocol subghz_protocol_kia_v5;

void* subghz_protocol_decoder_kia_v5_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_kia_v5_free(void* context);
void subghz_protocol_decoder_kia_v5_reset(void* context);
void subghz_protocol_decoder_kia_v5_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_kia_v5_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_kia_v5_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_kia_v5_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_kia_v5_get_string(void* context, FuriString* output);

void* subghz_protocol_encoder_kia_v5_alloc(SubGhzEnvironment* environment);
void subghz_protocol_encoder_kia_v5_free(void* context);
SubGhzProtocolStatus
    subghz_protocol_encoder_kia_v5_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_encoder_kia_v5_stop(void* context);
LevelDuration subghz_protocol_encoder_kia_v5_yield(void* context);
void subghz_protocol_encoder_kia_v5_set_button(void* context, uint8_t button);
void subghz_protocol_encoder_kia_v5_set_counter(void* context, uint16_t counter);
void subghz_protocol_encoder_kia_v5_increment_counter(void* context);
uint16_t subghz_protocol_encoder_kia_v5_get_counter(void* context);
uint8_t subghz_protocol_encoder_kia_v5_get_button(void* context);
