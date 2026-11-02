#include "i2csniffer.h"

#include <string.h>

void clear_sniffer_buffers(i2cSniffer* i2c_sniffer) {
    furi_assert(i2c_sniffer);
    memset(i2c_sniffer->frames, 0, sizeof(i2c_sniffer->frames));
    i2c_sniffer->frame_index = 0;
    i2c_sniffer->state = I2C_BUS_FREE;
    i2c_sniffer->first = true;
}

void start_interrupts(i2cSniffer* i2c_sniffer) {
    furi_assert(i2c_sniffer);
    // Port: weak pull-ups instead of GpioPullNo. The header pins have no pull of their own, so
    // with nothing wired (or a disconnected bus) they float and the edge interrupts storm on
    // noise. A real I2C bus idles high anyway; ~45 kOhm is negligible next to its pull-ups.
    furi_hal_gpio_init(pinSCL, GpioModeInterruptRise, GpioPullUp, GpioSpeedHigh);
    furi_hal_gpio_add_int_callback(pinSCL, SCLcallback, i2c_sniffer);

    // Add Rise and Fall Interrupt on SDA pin
    furi_hal_gpio_init(pinSDA, GpioModeInterruptRiseFall, GpioPullUp, GpioSpeedHigh);
    furi_hal_gpio_add_int_callback(pinSDA, SDAcallback, i2c_sniffer);
}

void stop_interrupts() {
    furi_hal_gpio_remove_int_callback(pinSCL);
    furi_hal_gpio_remove_int_callback(pinSDA);
    // Reset GPIO pins to default state
    furi_hal_gpio_init(pinSCL, GpioModeAnalog, GpioPullNo, GpioSpeedLow);
    furi_hal_gpio_init(pinSDA, GpioModeAnalog, GpioPullNo, GpioSpeedLow);
}

// Called on Fallin/Rising SDA
// Used to monitor i2c bus state
void SDAcallback(void* _i2c_sniffer) {
    i2cSniffer* i2c_sniffer = _i2c_sniffer;
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
            clear_sniffer_buffers(i2c_sniffer);
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
    if(i2c_sniffer->state == I2C_BUS_FREE) {
        return;
    }
    uint8_t frame = i2c_sniffer->frame_index;
    uint8_t bit = i2c_sniffer->frames[frame].bit_index;
    uint8_t data_idx = i2c_sniffer->frames[frame].data_index;
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
