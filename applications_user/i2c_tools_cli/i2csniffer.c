#include "i2csniffer.h"

#include <string.h>

void clear_sniffer_buffers(i2cSniffer* i2c_sniffer) {
    furi_assert(i2c_sniffer);
    memset(i2c_sniffer->frames, 0, sizeof(i2c_sniffer->frames));
    i2c_sniffer->frame_index = 0;
    i2c_sniffer->state = I2C_BUS_FREE;
    i2c_sniffer->first = true;
    i2c_sniffer->full = false;
}

GpioPin i2c_sniff_scl_pin = {.port = NULL, .pin = 0};
GpioPin i2c_sniff_sda_pin = {.port = NULL, .pin = 0};

// Owner name of the two pads while the sniffer holds them (shown to other features that find the
// pad reserved). The IR HAL takes the same kind of lease on its external pad, so the sniffer and
// an IR transmission can never drive or listen on one pad at the same time.
static const char i2c_sniffer_pin_owner[] = "I2C Sniffer";
static bool i2c_sniffer_pins_held = false;

static bool i2c_sniffer_take_pad(const GpioPin* pin) {
    if(!furi_hal_gpio_reserve(pin, i2c_sniffer_pin_owner)) return false;
    // Somebody (the GPIO app, a script) may have made it an output: never listen on a pad that
    // is being driven.
    if(furi_hal_gpio_is_output(pin)) {
        furi_hal_gpio_unreserve(pin, i2c_sniffer_pin_owner);
        return false;
    }
    return true;
}

bool start_interrupts(i2cSniffer* i2c_sniffer) {
    furi_assert(i2c_sniffer);
    i2c_sniffer->error = NULL;
    if(i2c_sniffer_pins_held) return true;
    // A new capture starts empty (here, in task context: the ISRs only ever write into it).
    clear_sniffer_buffers(i2c_sniffer);

    uint8_t sda = 0;
    uint8_t scl = 0;
    if(!furi_hal_i2c_get_sniffer_pins(&sda, &scl)) {
        // The I2C sniffer needs two free header pads that are not the board's own I2C bus (tapping
        // that bus would break NFC / the power ICs). Only Paradoxt exposes such pads, so the sniffer
        // is a Paradoxt-only feature; every other board lands here. Say so plainly instead of the old
        // "no free pads" wording, which read as if the board might work with pads freed up.
        i2c_sniffer->error = "Not supported here\nUse Paradoxt board";
        return false;
    }
    i2c_sniff_sda_pin = (GpioPin){.port = NULL, .pin = sda};
    i2c_sniff_scl_pin = (GpioPin){.port = NULL, .pin = scl};

    if(!i2c_sniffer_take_pad(pinSDA)) {
        i2c_sniffer->error = "Pad in use:\nclose IR / GPIO app";
        return false;
    }
    if(!i2c_sniffer_take_pad(pinSCL)) {
        furi_hal_gpio_unreserve(pinSDA, i2c_sniffer_pin_owner);
        i2c_sniffer->error = "Pad in use:\nclose IR / GPIO app";
        return false;
    }
    i2c_sniffer_pins_held = true;

    // Weak pull-ups instead of GpioPullNo. The header pins have no pull of their own, so with
    // nothing wired (or a disconnected bus) they float and the edge interrupts storm on noise. A
    // real I2C bus idles high anyway; ~45 kOhm is negligible next to its pull-ups. The pads are
    // reserved, so they are configured with the owner variant of the init call.
    furi_hal_gpio_init_owned(
        pinSCL, GpioModeInterruptRise, GpioPullUp, GpioSpeedHigh, i2c_sniffer_pin_owner);
    furi_hal_gpio_add_int_callback(pinSCL, SCLcallback, i2c_sniffer);

    // Add Rise and Fall Interrupt on SDA pin
    furi_hal_gpio_init_owned(
        pinSDA, GpioModeInterruptRiseFall, GpioPullUp, GpioSpeedHigh, i2c_sniffer_pin_owner);
    furi_hal_gpio_add_int_callback(pinSDA, SDAcallback, i2c_sniffer);
    return true;
}

void stop_interrupts() {
    if(!i2c_sniffer_pins_held) return;
    furi_hal_gpio_remove_int_callback(pinSCL);
    furi_hal_gpio_remove_int_callback(pinSDA);
    // Back to a plain, undriven input, then hand the pads back.
    furi_hal_gpio_init_owned(pinSCL, GpioModeInput, GpioPullNo, GpioSpeedLow, i2c_sniffer_pin_owner);
    furi_hal_gpio_init_owned(pinSDA, GpioModeInput, GpioPullNo, GpioSpeedLow, i2c_sniffer_pin_owner);
    furi_hal_gpio_unreserve(pinSCL, i2c_sniffer_pin_owner);
    furi_hal_gpio_unreserve(pinSDA, i2c_sniffer_pin_owner);
    i2c_sniffer_pins_held = false;
}

// Called on Fallin/Rising SDA
// Used to monitor i2c bus state
void SDAcallback(void* _i2c_sniffer) {
    i2cSniffer* i2c_sniffer = _i2c_sniffer;
    if(i2c_sniffer->full) return;
    // SCL is low maybe cclock strecching
    if(furi_hal_gpio_read(pinSCL) == false) {
        return;
    }
    // Check for stop condition: SDA rising while SCL is High
    if(i2c_sniffer->state == I2C_BUS_STARTED) {
        if(furi_hal_gpio_read(pinSDA) == true) {
            i2c_sniffer->state = I2C_BUS_FREE;
        }
    }
    // Check for start condition: SDA falling while SCL is high
    else if(furi_hal_gpio_read(pinSDA) == false) {
        i2c_sniffer->state = I2C_BUS_STARTED;
        if(i2c_sniffer->first) {
            i2c_sniffer->first = false;
            return;
        }
        if(i2c_sniffer->frame_index + 1U >= MAX_RECORDS) {
            // Table full. Wiping it from here (a ~33 KB memset inside the ISR) would lose edges,
            // the transaction in flight and every frame captured so far: stop and say so.
            i2c_sniffer->full = true;
            i2c_sniffer->state = I2C_BUS_FREE;
        } else {
            i2c_sniffer->frame_index++;
        }
        memset(&i2c_sniffer->frames[i2c_sniffer->frame_index], 0, sizeof(i2cFrame));
    }
    return;
}

// Called on Rising SCL
// Used to read bus datas
void SCLcallback(void* _i2c_sniffer) {
    i2cSniffer* i2c_sniffer = _i2c_sniffer;
    if(i2c_sniffer->full || i2c_sniffer->state == I2C_BUS_FREE) {
        return;
    }
    uint8_t frame = i2c_sniffer->frame_index;
    uint8_t bit = i2c_sniffer->frames[frame].bit_index;
    uint8_t data_idx = i2c_sniffer->frames[frame].data_index;
    // A transaction longer than the frame (or a bus left "started" by noise) must not run past
    // data[] / ack[]: the counter is a uint8_t and the arrays hold MAX_MESSAGE_SIZE entries.
    if(data_idx >= MAX_MESSAGE_SIZE) {
        return;
    }
    if(bit < 8) {
        i2c_sniffer->frames[frame].data[data_idx] <<= 1;
        i2c_sniffer->frames[frame].data[data_idx] |= (int)furi_hal_gpio_read(pinSDA);
        i2c_sniffer->frames[frame].bit_index++;
    } else {
        i2c_sniffer->frames[frame].ack[data_idx] = !furi_hal_gpio_read(pinSDA);
        i2c_sniffer->frames[frame].data_index++;
        i2c_sniffer->frames[frame].bit_index = 0;
    }
}

i2cSniffer* i2c_sniffer_alloc() {
    i2cSniffer* i2c_sniffer = malloc(sizeof(i2cSniffer));
    i2c_sniffer->error = NULL;
    i2c_sniffer->started = false;
    i2c_sniffer->row_index = 0;
    i2c_sniffer->menu_index = 0;
    clear_sniffer_buffers(i2c_sniffer);
    return i2c_sniffer;
}

void i2c_sniffer_free(i2cSniffer* i2c_sniffer) {
    furi_assert(i2c_sniffer);
    if(i2c_sniffer->started) {
        stop_interrupts();
    }
    free(i2c_sniffer);
}
