#pragma once

/*
 * Servo Tester state machine (upstream mhasbini/kaklik v0.2 logic, no hardware access), kept
 * apart from the app so it can be checked on a host. Takes normalized input (servo_input.h):
 * one Short per click, Long for an OK/Back hold, Repeat while an arrow is held.
 */

#include <input/input.h>
#include <stdbool.h>
#include <stdint.h>

#define MIN_ANGLE    0
#define MAX_ANGLE    180
#define CENTER_ANGLE 90
#define ANGLE_STEP   10

/* Upstream drove 3%..13% of the 20 ms frame (600..2600 us), which put "Center" (90 deg) at
 * 1600 us. RC servo neutral is 1500 us, so the range is 500..2500 us here (0/90/180 deg =
 * 500/1500/2500 us, the usual 180 degree servo mapping). */
#define PULSE_MIN_US 500
#define PULSE_MAX_US 2500

typedef enum {
    Manual = 1,
    Center = 2,
    Sweep = 3,
} Mode;

typedef enum {
    ServoAppActionNone,
    ServoAppActionExit,
    ServoAppActionNextPin,
} ServoAppAction;

typedef struct {
    Mode mode;
    uint8_t angle;
} ServoAppState;

static inline void servo_app_state_init(ServoAppState* s) {
    s->mode = Manual;
    s->angle = MIN_ANGLE;
}

static inline uint32_t angle_to_pulse_us(uint8_t angle) {
    if(angle > MAX_ANGLE) angle = MAX_ANGLE;
    return PULSE_MIN_US +
           ((uint32_t)angle * (PULSE_MAX_US - PULSE_MIN_US) + MAX_ANGLE / 2) / MAX_ANGLE;
}

static inline void servo_app_set_mode(ServoAppState* s, Mode mode) {
    s->mode = mode;
    if(mode == Center) s->angle = CENTER_ANGLE;
    if(mode == Sweep) s->angle = MIN_ANGLE;
}

/** Handle one normalized key event. */
static inline ServoAppAction servo_app_on_key(ServoAppState* s, InputKey key, InputType type) {
    if(key == InputKeyBack) {
        return (type == InputTypeShort || type == InputTypeLong) ? ServoAppActionExit :
                                                                   ServoAppActionNone;
    }

    if(key == InputKeyOk) {
        // Unused upstream: picks the next header pin here.
        return type == InputTypeShort ? ServoAppActionNextPin : ServoAppActionNone;
    }

    if(key == InputKeyUp && type == InputTypeShort) {
        servo_app_set_mode(s, s->mode == Manual ? Sweep : s->mode == Center ? Manual : Center);
    } else if(key == InputKeyDown && type == InputTypeShort) {
        servo_app_set_mode(s, s->mode == Manual ? Center : s->mode == Center ? Sweep : Manual);
    } else if(s->mode == Manual && (type == InputTypeShort || type == InputTypeRepeat)) {
        if(key == InputKeyRight && s->angle < MAX_ANGLE) {
            s->angle += ANGLE_STEP;
        } else if(key == InputKeyLeft && s->angle > MIN_ANGLE) {
            s->angle -= ANGLE_STEP;
        }
    }
    return ServoAppActionNone;
}

/** One Sweep tick: jump between the end stops. @return true when the angle changed. */
static inline bool servo_app_on_tick(ServoAppState* s) {
    if(s->mode != Sweep) return false;

    if(s->angle == MAX_ANGLE) {
        s->angle = MIN_ANGLE;
    } else {
        s->angle = MAX_ANGLE;
    }
    return true;
}
