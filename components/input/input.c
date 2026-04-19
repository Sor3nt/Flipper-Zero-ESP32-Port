/**
 * @file input.c
 * Input service — delegates to target-specific input driver.
 *
 * Each target provides its own target_input.c with hardware-specific
 * input handling (touch, encoder, buttons, etc.).
 */

#include "input.h"
#include "target_input.h"

#include <furi.h>

#define TAG "Input"
#define INPUT_POLL_MS 20U

const char* input_get_key_name(InputKey key) {
    switch(key) {
    case InputKeyUp:
        return "Up";
    case InputKeyDown:
        return "Down";
    case InputKeyRight:
        return "Right";
    case InputKeyLeft:
        return "Left";
    case InputKeyOk:
        return "Ok";
    case InputKeyBack:
        return "Back";
    default:
        return "Unknown";
    }
}

const char* input_get_type_name(InputType type) {
    switch(type) {
    case InputTypePress:
        return "Press";
    case InputTypeRelease:
        return "Release";
    case InputTypeShort:
        return "Short";
    case InputTypeLong:
        return "Long";
    case InputTypeRepeat:
        return "Repeat";
    default:
        return "Unknown";
    }
}

int32_t input_srv(void* p) {
    UNUSED(p);

    FuriPubSub* event_pubsub = furi_pubsub_alloc();
    furi_record_create(RECORD_INPUT_EVENTS, event_pubsub);

    target_input_init();

    FURI_LOG_I(TAG, "Input service started");

    uint32_t sequence_counter = 0;

    while(true) {
        furi_delay_ms(INPUT_POLL_MS);
        target_input_poll(event_pubsub, &sequence_counter);
    }

    return 0;
}
