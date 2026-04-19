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

#define SUBGHZ_PROTOCOL_SHERIFF_CFM_NAME "Sheriff CFM"

typedef struct SubGhzProtocolDecoderSheriffCfm SubGhzProtocolDecoderSheriffCfm;
typedef struct SubGhzProtocolEncoderSheriffCfm SubGhzProtocolEncoderSheriffCfm;

extern const SubGhzProtocolDecoder subghz_protocol_sheriff_cfm_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_sheriff_cfm_encoder;
extern const SubGhzProtocol subghz_protocol_sheriff_cfm;

void* subghz_protocol_decoder_sheriff_cfm_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_sheriff_cfm_free(void* context);
void subghz_protocol_decoder_sheriff_cfm_reset(void* context);
void subghz_protocol_decoder_sheriff_cfm_feed(void* context, bool level, uint32_t duration);

uint8_t subghz_protocol_decoder_sheriff_cfm_get_hash_data(void* context);

SubGhzProtocolStatus subghz_protocol_decoder_sheriff_cfm_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);

SubGhzProtocolStatus
    subghz_protocol_decoder_sheriff_cfm_deserialize(void* context, FlipperFormat* flipper_format);

void subghz_protocol_decoder_sheriff_cfm_get_string(void* context, FuriString* output);

void* subghz_protocol_encoder_sheriff_cfm_alloc(SubGhzEnvironment* environment);
void subghz_protocol_encoder_sheriff_cfm_free(void* context);
SubGhzProtocolStatus
    subghz_protocol_encoder_sheriff_cfm_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_encoder_sheriff_cfm_stop(void* context);
LevelDuration subghz_protocol_encoder_sheriff_cfm_yield(void* context);
