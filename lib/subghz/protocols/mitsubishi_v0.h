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

#define MITSUBISHI_PROTOCOL_V0_NAME "Mitsubishi V0"

typedef struct SubGhzProtocolDecoderMitsubishiV0 SubGhzProtocolDecoderMitsubishiV0;
typedef struct SubGhzProtocolEncoderMitsubishiV0 SubGhzProtocolEncoderMitsubishiV0;

extern const SubGhzProtocol subghz_protocol_mitsubishi_v0;

/**
 * Allocate Mitsubishi V0 decoder.
 * @param environment SubGhzEnvironment
 * @return SubGhzProtocolDecoderMitsubishiV0*
 */
void* subghz_protocol_decoder_mitsubishi_v0_alloc(SubGhzEnvironment* environment);

/**
 * Free Mitsubishi V0 decoder.
 * @param context Pointer to SubGhzProtocolDecoderMitsubishiV0
 */
void subghz_protocol_decoder_mitsubishi_v0_free(void* context);

/**
 * Reset Mitsubishi V0 decoder.
 * @param context Pointer to SubGhzProtocolDecoderMitsubishiV0
 */
void subghz_protocol_decoder_mitsubishi_v0_reset(void* context);

/**
 * Feed level and duration to Mitsubishi V0 decoder.
 * @param context Pointer to SubGhzProtocolDecoderMitsubishiV0
 * @param level Level
 * @param duration Duration
 */
void subghz_protocol_decoder_mitsubishi_v0_feed(void* context, bool level, uint32_t duration);

/**
 * Get hash data from Mitsubishi V0 decoder.
 * @param context Pointer to SubGhzProtocolDecoderMitsubishiV0
 * @return uint8_t
 */
uint8_t subghz_protocol_decoder_mitsubishi_v0_get_hash_data(void* context);

/**
 * Serialize Mitsubishi V0 decoder data.
 * @param context Pointer to SubGhzProtocolDecoderMitsubishiV0
 * @param flipper_format FlipperFormat
 * @param preset SubGhzRadioPreset
 * @return SubGhzProtocolStatus
 */
SubGhzProtocolStatus subghz_protocol_decoder_mitsubishi_v0_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);

/**
 * Deserialize Mitsubishi V0 decoder data.
 * @param context Pointer to SubGhzProtocolDecoderMitsubishiV0
 * @param flipper_format FlipperFormat
 * @return SubGhzProtocolStatus
 */
SubGhzProtocolStatus
    subghz_protocol_decoder_mitsubishi_v0_deserialize(void* context, FlipperFormat* flipper_format);

/**
 * Get string representation of Mitsubishi V0 decoder data.
 * @param context Pointer to SubGhzProtocolDecoderMitsubishiV0
 * @param output FuriString*
 */
void subghz_protocol_decoder_mitsubishi_v0_get_string(void* context, FuriString* output);

/**
 * Allocate Mitsubishi V0 encoder.
 * @param environment SubGhzEnvironment
 * @return SubGhzProtocolEncoderMitsubishiV0*
 */
void* subghz_protocol_encoder_mitsubishi_v0_alloc(SubGhzEnvironment* environment);

/**
 * Free Mitsubishi V0 encoder.
 * @param context Pointer to SubGhzProtocolEncoderMitsubishiV0
 */
void subghz_protocol_encoder_mitsubishi_v0_free(void* context);

/**
 * Deserialize Mitsubishi V0 encoder data.
 * @param context Pointer to SubGhzProtocolEncoderMitsubishiV0
 * @param flipper_format FlipperFormat
 * @return SubGhzProtocolStatus
 */
SubGhzProtocolStatus
    subghz_protocol_encoder_mitsubishi_v0_deserialize(void* context, FlipperFormat* flipper_format);

/**
 * Stop Mitsubishi V0 encoder.
 * @param context Pointer to SubGhzProtocolEncoderMitsubishiV0
 */
void subghz_protocol_encoder_mitsubishi_v0_stop(void* context);

/**
 * Yield LevelDuration from Mitsubishi V0 encoder.
 * @param context Pointer to SubGhzProtocolEncoderMitsubishiV0
 * @return LevelDuration
 */
LevelDuration subghz_protocol_encoder_mitsubishi_v0_yield(void* context);
