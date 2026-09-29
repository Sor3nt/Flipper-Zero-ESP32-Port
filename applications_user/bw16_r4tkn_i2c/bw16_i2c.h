#pragma once
#include <esp_err.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../../firmware/bw16_i2c_link/bw16_i2c_link.h"

#define BW16_I2C_RX_SIZE 8192U
#define BW16_I2C_OPEN_MS 2000U
#define BW16_I2C_RETRIES 6U
typedef struct {
    bool installed, ready, tx_failed, pending, remote_empty;
    uint32_t session, tx_sequence, rx_sequence, pending_sequence;
    uint32_t bus_errors, protocol_errors, retries, duplicates, bytes_received, frames_received;
    unsigned failures;
    esp_err_t error;
    const char* stage;
    size_t rx_head, rx_count;
    uint8_t rx[BW16_I2C_RX_SIZE];
} Bw16I2c;

bool bw16_i2c_open(Bw16I2c* io);
esp_err_t bw16_i2c_send(Bw16I2c* io, const char* line, size_t size);
int bw16_i2c_read(Bw16I2c* io, void* output, size_t capacity);
esp_err_t bw16_i2c_buffered(Bw16I2c* io, size_t* size);
esp_err_t bw16_i2c_close(Bw16I2c* io);
