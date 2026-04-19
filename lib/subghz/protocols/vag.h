#pragma once

#include "base.h"
#include "../blocks/math.h"

#define VAG_PROTOCOL_NAME "VAG GROUP"

extern const SubGhzProtocol subghz_protocol_vag;

void* subghz_protocol_decoder_vag_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_vag_free(void* context);
void subghz_protocol_decoder_vag_reset(void* context);
void subghz_protocol_decoder_vag_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_vag_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_vag_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_vag_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_vag_get_string(void* context, FuriString* output);

void* subghz_protocol_encoder_vag_alloc(SubGhzEnvironment* environment);
void subghz_protocol_encoder_vag_free(void* context);
SubGhzProtocolStatus
    subghz_protocol_encoder_vag_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_encoder_vag_stop(void* context);
LevelDuration subghz_protocol_encoder_vag_yield(void* context);
