/**
 * @file furi_hal_encoder.c
 * @brief Rotary encoder driver for LilyGo T-Embed CC1101
 *
 * Uses GPIO interrupts on encoder A/B pins to detect rotation.
 * Publishes InputKeyUp/InputKeyDown events to the input pubsub.
 */

#include "furi_hal_encoder.h"
#include "boards/board.h"
#include <furi.h>
#include <furi_hal_gpio.h>
#include <furi_hal_resources.h>
#include <input/input.h>

#define TAG "Encoder"

#ifdef BOARD_HAS_ENCODER

#define ENCODER_DEBOUNCE_US 1000

typedef struct {
    volatile int8_t last_a;
    volatile int8_t last_b;
    volatile int32_t count;
    FuriPubSub* input_pubsub;
    FuriTimer* debounce_timer;
} EncoderState;

static EncoderState* encoder_state = NULL;

static void encoder_isr(void* ctx) {
    UNUSED(ctx);
    if(encoder_state) {
        encoder_state->count++;
    }
}

static void encoder_debounce_callback(void* ctx) {
    UNUSED(ctx);
    if(!encoder_state) return;

    int8_t a = furi_hal_gpio_read(&gpio_encoder_a);
    int8_t b = furi_hal_gpio_read(&gpio_encoder_b);

    if(a != encoder_state->last_a) {
        InputEvent event;
        event.sequence_source = INPUT_SEQUENCE_SOURCE_HARDWARE;
        event.sequence_counter = 0;

        if(a && !encoder_state->last_a) {
            // Rising edge on A
            if(b) {
                // B is high = CCW
                event.key = InputKeyUp;
            } else {
                // B is low = CW
                event.key = InputKeyDown;
            }
            event.type = InputTypeShort;
            furi_pubsub_publish(encoder_state->input_pubsub, &event);
        }
    }

    encoder_state->last_a = a;
    encoder_state->last_b = b;
}

void furi_hal_encoder_init(void) {
    furi_assert(!encoder_state);

    FURI_LOG_I(TAG, "Initializing encoder on A=%d B=%d",
        BOARD_PIN_ENCODER_A, BOARD_PIN_ENCODER_B);

    encoder_state = malloc(sizeof(EncoderState));
    encoder_state->last_a = 0;
    encoder_state->last_b = 0;
    encoder_state->count = 0;

    // Configure encoder GPIO pins with interrupt mode for rotation detection
    furi_hal_gpio_init(&gpio_encoder_a, GpioModeInterruptRiseFall, GpioPullUp, GpioSpeedLow);
    furi_hal_gpio_init(&gpio_encoder_b, GpioModeInput, GpioPullUp, GpioSpeedLow);

    // Enable interrupt on both edges of pin A
    furi_hal_gpio_add_int_callback(&gpio_encoder_a, encoder_isr, NULL);

    // Create debounce timer
    encoder_state->debounce_timer = furi_timer_alloc(
        encoder_debounce_callback, FuriTimerTypePeriodic, NULL);
    furi_timer_start(encoder_state->debounce_timer, 5); // 5ms debounce

    // Open input events pubsub
    encoder_state->input_pubsub = furi_record_open(RECORD_INPUT_EVENTS);

    FURI_LOG_I(TAG, "Encoder initialized");
}

void furi_hal_encoder_deinit(void) {
    if(!encoder_state) return;

    furi_timer_stop(encoder_state->debounce_timer);
    furi_timer_free(encoder_state->debounce_timer);

    furi_hal_gpio_remove_int_callback(&gpio_encoder_a);

    furi_record_close(RECORD_INPUT_EVENTS);

    free(encoder_state);
    encoder_state = NULL;
}

#endif // BOARD_HAS_ENCODER
