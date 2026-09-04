#pragma once

#include "base.h"

#define SUBGHZ_PROTOCOL_HOLTEK_HT6P20B_NAME "HT6P20B"

typedef struct SubGhzProtocolDecoderHoltek_HT6P20B SubGhzProtocolDecoderHoltek_HT6P20B;
typedef struct SubGhzProtocolEncoderHoltek_HT6P20B SubGhzProtocolEncoderHoltek_HT6P20B;

extern const SubGhzProtocolDecoder subghz_protocol_holtek_ht6p20b_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_holtek_ht6p20b_encoder;
extern const SubGhzProtocol subghz_protocol_holtek_ht6p20b;

/**
 * Allocate SubGhzProtocolEncoderHoltek_HT6P20B.
 * @param environment Pointer to a SubGhzEnvironment instance
 * @return SubGhzProtocolEncoderHoltek_HT6P20B* pointer to a SubGhzProtocolEncoderHoltek_HT6P20B instance
 */
void* subghz_protocol_encoder_holtek_ht6p20b_alloc(SubGhzEnvironment* environment);

/**
 * Free SubGhzProtocolEncoderHoltek_HT6P20B.
 * @param context Pointer to a SubGhzProtocolEncoderHoltek_HT6P20B instance
 */
void subghz_protocol_encoder_holtek_ht6p20b_free(void* context);

/**
 * Deserialize and generating an upload to send.
 * @param context Pointer to a SubGhzProtocolEncoderHoltek_HT6P20B instance
 * @param flipper_format Pointer to a FlipperFormat instance
 * @return status
 */
SubGhzProtocolStatus
    subghz_protocol_encoder_holtek_ht6p20b_deserialize(void* context, FlipperFormat* flipper_format);

/**
 * Forced transmission stop.
 * @param context Pointer to a SubGhzProtocolEncoderHoltek_HT6P20B instance
 */
void subghz_protocol_encoder_holtek_ht6p20b_stop(void* context);

/**
 * Getting the level and duration of the upload to be loaded into DMA.
 * @param context Pointer to a SubGhzProtocolEncoderHoltek_HT6P20B instance
 * @return LevelDuration 
 */
LevelDuration subghz_protocol_encoder_holtek_ht6p20b_yield(void* context);

/**
 * Allocate SubGhzProtocolDecoderHoltek_HT6P20B.
 * @param environment Pointer to a SubGhzEnvironment instance
 * @return SubGhzProtocolDecoderHoltek_HT6P20B* pointer to a SubGhzProtocolDecoderHoltek_HT6P20B instance
 */
void* subghz_protocol_decoder_holtek_ht6p20b_alloc(SubGhzEnvironment* environment);

/**
 * Free SubGhzProtocolDecoderHoltek_HT6P20B.
 * @param context Pointer to a SubGhzProtocolDecoderHoltek_HT6P20B instance
 */
void subghz_protocol_decoder_holtek_ht6p20b_free(void* context);

/**
 * Reset decoder SubGhzProtocolDecoderHoltek_HT6P20B.
 * @param context Pointer to a SubGhzProtocolDecoderHoltek_HT6P20B instance
 */
void subghz_protocol_decoder_holtek_ht6p20b_reset(void* context);

/**
 * Parse a raw sequence of levels and durations received from the air.
 * @param context Pointer to a SubGhzProtocolDecoderHoltek_HT6P20B instance
 * @param level Signal level true-high false-low
 * @param duration Duration of this level in, us
 */
void subghz_protocol_decoder_holtek_ht6p20b_feed(void* context, bool level, uint32_t duration);

/**
 * Getting the hash sum of the last randomly received parcel.
 * @param context Pointer to a SubGhzProtocolDecoderHoltek_HT6P20B instance
 * @return hash Hash sum
 */
uint8_t subghz_protocol_decoder_holtek_ht6p20b_get_hash_data(void* context);

/**
 * Serialize data SubGhzProtocolDecoderHoltek_HT6P20B.
 * @param context Pointer to a SubGhzProtocolDecoderHoltek_HT6P20B instance
 * @param flipper_format Pointer to a FlipperFormat instance
 * @param preset The modulation on which the signal was received, SubGhzRadioPreset
 * @return status
 */
SubGhzProtocolStatus subghz_protocol_decoder_holtek_ht6p20b_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);

/**
 * Deserialize data SubGhzProtocolDecoderHoltek_HT6P20B.
 * @param context Pointer to a SubGhzProtocolDecoderHoltek_HT6P20B instance
 * @param flipper_format Pointer to a FlipperFormat instance
 * @return status
 */
SubGhzProtocolStatus
    subghz_protocol_decoder_holtek_ht6p20b_deserialize(void* context, FlipperFormat* flipper_format);

/**
 * Getting a textual representation of the received data.
 * @param context Pointer to a SubGhzProtocolDecoderHoltek_HT6P20B instance
 * @param output Resulting text
 */
void subghz_protocol_decoder_holtek_ht6p20b_get_string(void* context, FuriString* output);

