#pragma once

#include <furi.h>
#include <furi_hal.h>
#include "i2c_compat.h"

// The two pads the sniffer listens on. They are chosen by the firmware (furi_hal_i2c_get_sniffer_pins:
// free header pads of the board, never the bus pins and never the external IR pad) and filled in
// by start_interrupts(). Boards without such pads cannot sniff.
extern GpioPin i2c_sniff_scl_pin;
extern GpioPin i2c_sniff_sda_pin;
#define pinSCL (&i2c_sniff_scl_pin)
#define pinSDA (&i2c_sniff_sda_pin)

// Bus States
typedef enum { I2C_BUS_FREE, I2C_BUS_STARTED } i2cBusStates;

// Max read size of i2c frame by message
// Arbitraly defined
// They're not real limit to maximum frames send
#define MAX_MESSAGE_SIZE 128

// Nb of records
#define MAX_RECORDS 128

/// @brief Struct used to store our reads
typedef struct {
    uint8_t data[MAX_MESSAGE_SIZE];
    bool ack[MAX_MESSAGE_SIZE];
    uint8_t bit_index;
    uint8_t data_index;
} i2cFrame;

typedef struct {
    bool started;
    bool first;
    i2cBusStates state;
    i2cFrame frames[MAX_RECORDS];
    uint8_t frame_index;
    uint8_t menu_index;
    uint8_t row_index;
    const char* error; // why start_interrupts() refused (NULL: no error)
    bool full; // the record table is full: capturing stops until the next start
} i2cSniffer;

void clear_sniffer_buffers(i2cSniffer* i2c_sniffer);
bool start_interrupts(i2cSniffer* i2c_sniffer);
void stop_interrupts();
void SDAcallback(void* _i2c_sniffer);
void SCLcallback(void* _i2c_sniffer);

i2cSniffer* i2c_sniffer_alloc();
void i2c_sniffer_free(i2cSniffer* i2c_sniffer);