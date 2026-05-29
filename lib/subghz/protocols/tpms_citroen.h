#pragma once

#include "base.h"
#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "tpms_generic.h"
#include "../blocks/math.h"

#define TPMS_PROTOCOL_CITROEN_NAME "Citroen TPMS"

extern const SubGhzProtocolDecoder tpms_protocol_citroen_decoder;
extern const SubGhzProtocolEncoder tpms_protocol_citroen_encoder;
extern const SubGhzProtocol tpms_protocol_citroen;

void* tpms_protocol_decoder_citroen_alloc(SubGhzEnvironment* environment);
void tpms_protocol_decoder_citroen_free(void* context);
void tpms_protocol_decoder_citroen_reset(void* context);
void tpms_protocol_decoder_citroen_feed(void* context, bool level, uint32_t duration);
uint8_t tpms_protocol_decoder_citroen_get_hash_data(void* context);

SubGhzProtocolStatus tpms_protocol_decoder_citroen_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);

SubGhzProtocolStatus
    tpms_protocol_decoder_citroen_deserialize(void* context, FlipperFormat* flipper_format);

void tpms_protocol_decoder_citroen_get_string(void* context, FuriString* output);
