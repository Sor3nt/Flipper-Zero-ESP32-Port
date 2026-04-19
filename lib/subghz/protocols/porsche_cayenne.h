#pragma once

#include <furi.h>
#include <lib/subghz/protocols/base.h>
#include <lib/subghz/types.h>
#include <lib/subghz/blocks/const.h>
#include <lib/subghz/blocks/decoder.h>
#include <lib/subghz/blocks/encoder.h>
#include <lib/subghz/blocks/generic.h>
#include <lib/subghz/blocks/math.h>
#include <flipper_format/flipper_format.h>

#define SUBGHZ_PROTOCOL_PORSCHE_CAYENNE_NAME "Porsche AG"

extern const SubGhzProtocol subghz_protocol_porsche_cayenne;

// Decoder
void* subghz_protocol_decoder_porsche_cayenne_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_porsche_cayenne_free(void* context);
void subghz_protocol_decoder_porsche_cayenne_reset(void* context);
void subghz_protocol_decoder_porsche_cayenne_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_porsche_cayenne_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_porsche_cayenne_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus subghz_protocol_decoder_porsche_cayenne_deserialize(
    void* context,
    FlipperFormat* flipper_format);
void subghz_protocol_decoder_porsche_cayenne_get_string(void* context, FuriString* output);

// Encoder
void* subghz_protocol_encoder_porsche_cayenne_alloc(SubGhzEnvironment* environment);
void subghz_protocol_encoder_porsche_cayenne_free(void* context);
SubGhzProtocolStatus subghz_protocol_encoder_porsche_cayenne_deserialize(
    void* context,
    FlipperFormat* flipper_format);
void subghz_protocol_encoder_porsche_cayenne_stop(void* context);
LevelDuration subghz_protocol_encoder_porsche_cayenne_yield(void* context);
