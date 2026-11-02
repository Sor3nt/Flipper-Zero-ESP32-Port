#pragma once

/*
 * Servo Tester state machine (upstream ThunderFly v1.3 logic, no hardware access), kept apart
 * from the app so it can be checked on a host. Takes normalized input (servo_input.h): one
 * Short per click, Long for an OK hold, Repeat while an arrow is held.
 */

#include <input/input.h>
#include <stdbool.h>
#include <stdint.h>

#define SERVOTESTER_MIN_WIDTH     1000
#define SERVOTESTER_DEFAULT_WIDTH 1500
#define SERVOTESTER_MAX_WIDTH     2000

#define SERVOTESTER_STEP_SMALL 1
#define SERVOTESTER_STEP_BIG   10
#define SERVOTESTER_STEP_AUTO  5

typedef enum {
    ServoTesterModeCenter,
    ServoTesterModeManual,
    ServoTesterModeAuto,
    ServoTesterModeCount,
} ServoTesterMode;

typedef enum {
    ServoTesterActionNone,
    ServoTesterActionExit,
    ServoTesterActionNextPin,
} ServoTesterAction;

typedef struct {
    uint16_t width; // pulse width, us
    ServoTesterMode mode;
    bool dir_up; // Auto sweep direction
} ServoTesterState;

static inline void servotester_state_init(ServoTesterState* s) {
    s->width = SERVOTESTER_DEFAULT_WIDTH;
    s->mode = ServoTesterModeManual;
    s->dir_up = true;
}

static inline uint16_t servotester_clamp(int32_t width) {
    if(width < SERVOTESTER_MIN_WIDTH) return SERVOTESTER_MIN_WIDTH;
    if(width > SERVOTESTER_MAX_WIDTH) return SERVOTESTER_MAX_WIDTH;
    return (uint16_t)width;
}

/** Handle one normalized key event. */
static inline ServoTesterAction
    servotester_on_key(ServoTesterState* s, InputKey key, InputType type) {
    if(key == InputKeyBack) {
        return (type == InputTypeShort || type == InputTypeLong) ? ServoTesterActionExit :
                                                                   ServoTesterActionNone;
    }

    if(key == InputKeyOk) {
        if(type == InputTypeLong) return ServoTesterActionNextPin;
        if(type != InputTypeShort) return ServoTesterActionNone;

        s->mode = (s->mode + 1) % ServoTesterModeCount;
        if(s->mode == ServoTesterModeCenter) s->width = SERVOTESTER_DEFAULT_WIDTH;
        if(s->mode == ServoTesterModeAuto) s->dir_up = true; // deterministic direction
        return ServoTesterActionNone;
    }

    if(s->mode != ServoTesterModeManual) return ServoTesterActionNone;
    if(type != InputTypeShort && type != InputTypeRepeat) return ServoTesterActionNone;

    int32_t width = s->width;
    switch(key) {
    case InputKeyLeft:
        width -= SERVOTESTER_STEP_SMALL; // fine step for precise setting
        break;
    case InputKeyRight:
        width += SERVOTESTER_STEP_SMALL;
        break;
    case InputKeyDown:
        width -= SERVOTESTER_STEP_BIG; // big step, repeats while held for fast scrolling
        break;
    case InputKeyUp:
        width += SERVOTESTER_STEP_BIG;
        break;
    default:
        break;
    }
    s->width = servotester_clamp(width);
    return ServoTesterActionNone;
}

/** Advance the Auto sweep by one tick. @return true when the width changed. */
static inline bool servotester_on_tick(ServoTesterState* s) {
    if(s->mode != ServoTesterModeAuto) return false;

    int32_t width = s->width;
    if(s->dir_up) {
        width += SERVOTESTER_STEP_AUTO;
        if(width >= SERVOTESTER_MAX_WIDTH) s->dir_up = false;
    } else {
        width -= SERVOTESTER_STEP_AUTO;
        if(width <= SERVOTESTER_MIN_WIDTH) s->dir_up = true;
    }

    const uint16_t clamped = servotester_clamp(width);
    const bool changed = clamped != s->width;
    s->width = clamped;
    return changed;
}
