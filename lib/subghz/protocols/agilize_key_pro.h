#pragma once

#include "base.h"

#define SUBGHZ_PROTOCOL_AGILIZE_KEY_PRO_NAME "Key Pro"

typedef struct SubGhzProtocolDecoderAgilize_Key_Pro SubGhzProtocolDecoderAgilize_Key_Pro;
typedef struct SubGhzProtocolEncoderAgilize_Key_Pro SubGhzProtocolEncoderAgilize_Key_Pro;

extern const SubGhzProtocolDecoder subghz_protocol_agilize_key_pro_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_agilize_key_pro_encoder;
extern const SubGhzProtocol subghz_protocol_agilize_key_pro;

/**
 * Allocate SubGhzProtocolEncoderAgilize_Key_Pro.
 * @param environment Pointer to a SubGhzEnvironment instance
 * @return SubGhzProtocolEncoderAgilize_Key_Pro* pointer to a SubGhzProtocolEncoderAgilize_Key_Pro instance
 */
void* subghz_protocol_encoder_agilize_key_pro_alloc(SubGhzEnvironment* environment);

/**
 * Free SubGhzProtocolEncoderAgilize_Key_Pro.
 * @param context Pointer to a SubGhzProtocolEncoderAgilize_Key_Pro instance
 */
void subghz_protocol_encoder_agilize_key_pro_free(void* context);

/**
 * Deserialize and generating an upload to send.
 * @param context Pointer to a SubGhzProtocolEncoderAgilize_Key_Pro instance
 * @param flipper_format Pointer to a FlipperFormat instance
 * @return status
 */
SubGhzProtocolStatus
    subghz_protocol_encoder_agilize_key_pro_deserialize(void* context, FlipperFormat* flipper_format);

/**
 * Forced transmission stop.
 * @param context Pointer to a SubGhzProtocolEncoderAgilize_Key_Pro instance
 */
void subghz_protocol_encoder_agilize_key_pro_stop(void* context);

/**
 * Getting the level and duration of the upload to be loaded into DMA.
 * @param context Pointer to a SubGhzProtocolEncoderAgilize_Key_Pro instance
 * @return LevelDuration 
 */
LevelDuration subghz_protocol_encoder_agilize_key_pro_yield(void* context);

/**
 * Allocate SubGhzProtocolDecoderAgilize_Key_Pro.
 * @param environment Pointer to a SubGhzEnvironment instance
 * @return SubGhzProtocolDecoderAgilize_Key_Pro* pointer to a SubGhzProtocolDecoderAgilize_Key_Pro instance
 */
void* subghz_protocol_decoder_agilize_key_pro_alloc(SubGhzEnvironment* environment);

/**
 * Free SubGhzProtocolDecoderAgilize_Key_Pro.
 * @param context Pointer to a SubGhzProtocolDecoderAgilize_Key_Pro instance
 */
void subghz_protocol_decoder_agilize_key_pro_free(void* context);

/**
 * Reset decoder SubGhzProtocolDecoderAgilize_Key_Pro.
 * @param context Pointer to a SubGhzProtocolDecoderAgilize_Key_Pro instance
 */
void subghz_protocol_decoder_agilize_key_pro_reset(void* context);

/**
 * Parse a raw sequence of levels and durations received from the air.
 * @param context Pointer to a SubGhzProtocolDecoderAgilize_Key_Pro instance
 * @param level Signal level true-high false-low
 * @param duration Duration of this level in, us
 */
void subghz_protocol_decoder_agilize_key_pro_feed(void* context, bool level, uint32_t duration);

/**
 * Getting the hash sum of the last randomly received parcel.
 * @param context Pointer to a SubGhzProtocolDecoderAgilize_Key_Pro instance
 * @return hash Hash sum
 */
uint8_t subghz_protocol_decoder_agilize_key_pro_get_hash_data(void* context);

/**
 * Serialize data SubGhzProtocolDecoderAgilize_Key_Pro.
 * @param context Pointer to a SubGhzProtocolDecoderAgilize_Key_Pro instance
 * @param flipper_format Pointer to a FlipperFormat instance
 * @param preset The modulation on which the signal was received, SubGhzRadioPreset
 * @return status
 */
SubGhzProtocolStatus subghz_protocol_decoder_agilize_key_pro_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);

/**
 * Deserialize data SubGhzProtocolDecoderAgilize_Key_Pro.
 * @param context Pointer to a SubGhzProtocolDecoderAgilize_Key_Pro instance
 * @param flipper_format Pointer to a FlipperFormat instance
 * @return status
 */
SubGhzProtocolStatus
    subghz_protocol_decoder_agilize_key_pro_deserialize(void* context, FlipperFormat* flipper_format);

/**
 * Getting a textual representation of the received data.
 * @param context Pointer to a SubGhzProtocolDecoderAgilize_Key_Pro instance
 * @param output Resulting text
 */
void subghz_protocol_decoder_agilize_key_pro_get_string(void* context, FuriString* output);

