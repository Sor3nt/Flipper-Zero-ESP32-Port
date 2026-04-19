#pragma once

#include "base.h"

#define HONDA_STATIC_PROTOCOL_NAME "Honda Static"

typedef struct SubGhzProtocolDecoderHondaStatic SubGhzProtocolDecoderHondaStatic;
typedef struct SubGhzProtocolEncoderHondaStatic SubGhzProtocolEncoderHondaStatic;

extern const SubGhzProtocol subghz_protocol_honda_static;

void* subghz_protocol_decoder_honda_static_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_honda_static_free(void* context);
void subghz_protocol_decoder_honda_static_reset(void* context);
void subghz_protocol_decoder_honda_static_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_honda_static_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_honda_static_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_honda_static_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_honda_static_get_string(void* context, FuriString* output);

void* subghz_protocol_encoder_honda_static_alloc(SubGhzEnvironment* environment);
void subghz_protocol_encoder_honda_static_free(void* context);
SubGhzProtocolStatus
    subghz_protocol_encoder_honda_static_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_encoder_honda_static_stop(void* context);
LevelDuration subghz_protocol_encoder_honda_static_yield(void* context);
