#pragma once

#include "base.h"
#include <flipper_format/flipper_format.h>

#define BMW_CAS4_PROTOCOL_NAME "BMW CAS4"

typedef struct SubGhzProtocolDecoderBmwCas4 SubGhzProtocolDecoderBmwCas4;
typedef struct SubGhzProtocolEncoderBmwCas4 SubGhzProtocolEncoderBmwCas4;

extern const SubGhzProtocol subghz_protocol_bmw_cas4;

void* subghz_protocol_decoder_bmw_cas4_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_bmw_cas4_free(void* context);
void subghz_protocol_decoder_bmw_cas4_reset(void* context);
void subghz_protocol_decoder_bmw_cas4_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_bmw_cas4_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_bmw_cas4_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_bmw_cas4_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_bmw_cas4_get_string(void* context, FuriString* output);

void* subghz_protocol_encoder_bmw_cas4_alloc(SubGhzEnvironment* environment);
void subghz_protocol_encoder_bmw_cas4_free(void* context);
SubGhzProtocolStatus
    subghz_protocol_encoder_bmw_cas4_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_encoder_bmw_cas4_stop(void* context);
LevelDuration subghz_protocol_encoder_bmw_cas4_yield(void* context);
