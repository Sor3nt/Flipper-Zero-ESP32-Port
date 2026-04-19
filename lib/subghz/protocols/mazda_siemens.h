#pragma once

#include "base.h"

#define SUBGHZ_PROTOCOL_MAZDA_SIEMENS_NAME "MazdaSiemens"

typedef struct SubGhzProtocolDecoderMazdaSiemens SubGhzProtocolDecoderMazdaSiemens;
typedef struct SubGhzProtocolEncoderMazdaSiemens SubGhzProtocolEncoderMazdaSiemens;

extern const SubGhzProtocolDecoder subghz_protocol_mazda_siemens_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_mazda_siemens_encoder;
extern const SubGhzProtocol subghz_protocol_mazda_siemens;

void* subghz_protocol_encoder_mazda_siemens_alloc(SubGhzEnvironment* environment);
void subghz_protocol_encoder_mazda_siemens_free(void* context);
SubGhzProtocolStatus
    subghz_protocol_encoder_mazda_siemens_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_encoder_mazda_siemens_stop(void* context);
LevelDuration subghz_protocol_encoder_mazda_siemens_yield(void* context);

void* subghz_protocol_decoder_mazda_siemens_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_mazda_siemens_free(void* context);
void subghz_protocol_decoder_mazda_siemens_reset(void* context);
void subghz_protocol_decoder_mazda_siemens_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_mazda_siemens_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_mazda_siemens_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_mazda_siemens_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_mazda_siemens_get_string(void* context, FuriString* output);
