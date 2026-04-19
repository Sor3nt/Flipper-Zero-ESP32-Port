/**
 * @file target_input.h
 * Abstract input driver interface — each target provides its own implementation.
 *
 * Waveshare boards:  CST816S touch gestures + BOOT button
 * LilyGo T-Embed:   Rotary encoder + key buttons
 */

#pragma once

#include <furi.h>
#include <input.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize target-specific input hardware.
 * Called once at input service startup.
 */
void target_input_init(void);

/**
 * Poll target-specific input hardware and publish events.
 * Called every INPUT_POLL_MS from the input service main loop.
 *
 * @param pubsub           Input event pubsub channel
 * @param sequence_counter Monotonic sequence counter (caller owns, callee increments)
 */
void target_input_poll(FuriPubSub* pubsub, uint32_t* sequence_counter);

#ifdef __cplusplus
}
#endif
