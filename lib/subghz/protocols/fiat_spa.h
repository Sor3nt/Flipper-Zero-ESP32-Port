#pragma once

#include "base.h"

#define SUBGHZ_PROTOCOL_FIAT_SPA_NAME "FIAT SPA"

typedef struct SubGhzProtocolDecoderFiatSpa SubGhzProtocolDecoderFiatSpa;
typedef struct SubGhzProtocolEncoderFiatSpa SubGhzProtocolEncoderFiatSpa;

extern const SubGhzProtocol subghz_protocol_fiat_spa;

void* subghz_protocol_decoder_fiat_spa_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_fiat_spa_free(void* context);
void subghz_protocol_decoder_fiat_spa_reset(void* context);
void subghz_protocol_decoder_fiat_spa_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_fiat_spa_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_fiat_spa_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus subghz_protocol_decoder_fiat_spa_deserialize(
    void* context,
    FlipperFormat* flipper_format);
void subghz_protocol_decoder_fiat_spa_get_string(void* context, FuriString* output);

void* subghz_protocol_encoder_fiat_spa_alloc(SubGhzEnvironment* environment);
void subghz_protocol_encoder_fiat_spa_free(void* context);
SubGhzProtocolStatus subghz_protocol_encoder_fiat_spa_deserialize(
    void* context,
    FlipperFormat* flipper_format);
void subghz_protocol_encoder_fiat_spa_stop(void* context);
LevelDuration subghz_protocol_encoder_fiat_spa_yield(void* context);
