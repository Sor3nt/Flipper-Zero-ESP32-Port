#pragma once

#include "base.h"
#include "../blocks/math.h"

#define SUBGHZ_PROTOCOL_SUZUKI_NAME "SUZUKI"

typedef struct SubGhzProtocolDecoderSuzuki SubGhzProtocolDecoderSuzuki;
typedef struct SubGhzProtocolEncoderSuzuki SubGhzProtocolEncoderSuzuki;

extern const SubGhzProtocol subghz_protocol_suzuki;

void* subghz_protocol_decoder_suzuki_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_suzuki_free(void* context);
void subghz_protocol_decoder_suzuki_reset(void* context);
void subghz_protocol_decoder_suzuki_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_suzuki_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_suzuki_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus subghz_protocol_decoder_suzuki_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_suzuki_get_string(void* context, FuriString* output);

void* subghz_protocol_encoder_suzuki_alloc(SubGhzEnvironment* environment);
void subghz_protocol_encoder_suzuki_free(void* context);
SubGhzProtocolStatus subghz_protocol_encoder_suzuki_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_encoder_suzuki_stop(void* context);
LevelDuration subghz_protocol_encoder_suzuki_yield(void* context);
