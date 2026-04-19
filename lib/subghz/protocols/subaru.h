#pragma once

#include "base.h"
#include "../blocks/math.h"

#define SUBGHZ_PROTOCOL_SUBARU_NAME "SUBARU"

typedef struct SubGhzProtocolDecoderSubaru SubGhzProtocolDecoderSubaru;
typedef struct SubGhzProtocolEncoderSubaru SubGhzProtocolEncoderSubaru;

extern const SubGhzProtocol subghz_protocol_subaru;

// Decoder functions
void* subghz_protocol_decoder_subaru_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_subaru_free(void* context);
void subghz_protocol_decoder_subaru_reset(void* context);
void subghz_protocol_decoder_subaru_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_subaru_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_subaru_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus subghz_protocol_decoder_subaru_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_subaru_get_string(void* context, FuriString* output);

// Encoder functions
void* subghz_protocol_encoder_subaru_alloc(SubGhzEnvironment* environment);
void subghz_protocol_encoder_subaru_free(void* context);
void subghz_protocol_encoder_subaru_stop(void* context);
LevelDuration subghz_protocol_encoder_subaru_yield(void* context);
SubGhzProtocolStatus subghz_protocol_encoder_subaru_deserialize(
    void* context,
    FlipperFormat* flipper_format);
