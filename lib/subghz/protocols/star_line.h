#pragma once
#include <furi.h>
#include <lib/subghz/protocols/base.h>
#include <lib/subghz/types.h>
#include <lib/subghz/blocks/const.h>
#include <lib/subghz/blocks/decoder.h>
#include <lib/subghz/blocks/encoder.h>
#include <lib/subghz/blocks/generic.h>
#include <lib/subghz/blocks/math.h>
#include <lib/toolbox/manchester_decoder.h>
#include <flipper_format/flipper_format.h>

#define SUBGHZ_PROTOCOL_STAR_LINE_NAME "Star Line"

typedef struct SubGhzProtocolDecoderStarLine SubGhzProtocolDecoderStarLine;
typedef struct SubGhzProtocolEncoderStarLine SubGhzProtocolEncoderStarLine;

extern const SubGhzProtocolDecoder subghz_protocol_star_line_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_star_line_encoder;
extern const SubGhzProtocol subghz_protocol_star_line;

void* subghz_protocol_encoder_star_line_alloc(SubGhzEnvironment* environment);
void subghz_protocol_encoder_star_line_free(void* context);

SubGhzProtocolStatus
    subghz_protocol_encoder_star_line_deserialize(void* context, FlipperFormat* flipper_format);

void subghz_protocol_encoder_star_line_stop(void* context);

LevelDuration subghz_protocol_encoder_star_line_yield(void* context);

void* subghz_protocol_decoder_star_line_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_star_line_free(void* context);
void subghz_protocol_decoder_star_line_reset(void* context);
void subghz_protocol_decoder_star_line_feed(void* context, bool level, uint32_t duration);

uint8_t subghz_protocol_decoder_star_line_get_hash_data(void* context);

SubGhzProtocolStatus subghz_protocol_decoder_star_line_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);

SubGhzProtocolStatus
    subghz_protocol_decoder_star_line_deserialize(void* context, FlipperFormat* flipper_format);

void subghz_protocol_decoder_star_line_get_string(void* context, FuriString* output);
