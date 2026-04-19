#pragma once

#include "base.h"
#include <flipper_format/flipper_format.h>

#define CHRYSLER_PROTOCOL_NAME "Chrysler"

typedef struct SubGhzProtocolDecoderChrysler SubGhzProtocolDecoderChrysler;
typedef struct SubGhzProtocolEncoderChrysler SubGhzProtocolEncoderChrysler;

extern const SubGhzProtocol subghz_protocol_chrysler;

void* subghz_protocol_decoder_chrysler_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_chrysler_free(void* context);
void subghz_protocol_decoder_chrysler_reset(void* context);
void subghz_protocol_decoder_chrysler_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_chrysler_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_chrysler_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_chrysler_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_chrysler_get_string(void* context, FuriString* output);

void* subghz_protocol_encoder_chrysler_alloc(SubGhzEnvironment* environment);
void subghz_protocol_encoder_chrysler_free(void* context);
SubGhzProtocolStatus
    subghz_protocol_encoder_chrysler_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_encoder_chrysler_stop(void* context);
LevelDuration subghz_protocol_encoder_chrysler_yield(void* context);
