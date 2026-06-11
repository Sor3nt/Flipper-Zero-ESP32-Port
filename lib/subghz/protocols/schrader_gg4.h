#pragma once

#include "base.h"
#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "tpms_generic.h"
#include "../blocks/math.h"

#define TPMS_PROTOCOL_SCHRADER_GG4_NAME "Schrader GG4"

typedef struct TPMSProtocolDecoderSchraderGG4 TPMSProtocolDecoderSchraderGG4;
typedef struct TPMSProtocolEncoderSchraderGG4 TPMSProtocolEncoderSchraderGG4;

extern const SubGhzProtocolDecoder tpms_protocol_schrader_gg4_decoder;
extern const SubGhzProtocolEncoder tpms_protocol_schrader_gg4_encoder;
extern const SubGhzProtocol tpms_protocol_schrader_gg4;

void* tpms_protocol_decoder_schrader_gg4_alloc(SubGhzEnvironment* environment);
void tpms_protocol_decoder_schrader_gg4_free(void* context);
void tpms_protocol_decoder_schrader_gg4_reset(void* context);
void tpms_protocol_decoder_schrader_gg4_feed(void* context, bool level, uint32_t duration);
uint8_t tpms_protocol_decoder_schrader_gg4_get_hash_data(void* context);

SubGhzProtocolStatus tpms_protocol_decoder_schrader_gg4_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);

SubGhzProtocolStatus
    tpms_protocol_decoder_schrader_gg4_deserialize(void* context, FlipperFormat* flipper_format);

void tpms_protocol_decoder_schrader_gg4_get_string(void* context, FuriString* output);

void* tpms_protocol_encoder_schrader_gg4_alloc(SubGhzEnvironment* environment);
void tpms_protocol_encoder_schrader_gg4_free(void* context);
void tpms_protocol_encoder_schrader_gg4_stop(void* context);
LevelDuration tpms_protocol_encoder_schrader_gg4_yield(void* context);

SubGhzProtocolStatus
    tpms_protocol_encoder_schrader_gg4_deserialize(void* context, FlipperFormat* flipper_format);

/**
 * Re-pack the Schrader GG4 64-bit data word from edited generic fields
 * (id, pressure, temperature). Preserves the Status byte from generic->data
 * and recomputes the trailing CRC8 (poly 0x7, init 0x0).
 *
 * @param generic TPMSBlockGeneric whose id/pressure/temperature are read and
 *                whose data field gets the freshly packed payload written back.
 */
void tpms_protocol_schrader_gg4_pack(TPMSBlockGeneric* generic);
