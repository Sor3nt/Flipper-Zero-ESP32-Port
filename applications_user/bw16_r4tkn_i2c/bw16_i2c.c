#include "bw16_i2c.h"
#include "bw16_i2c_hal.h"
#include <furi.h>
#include <furi_hal_random.h>
#include <esp_timer.h>
#include <string.h>

static uint32_t clock_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static esp_err_t response(Bw16I2c* io, const uint8_t raw[BWI_FRAME_SIZE]) {
    BwiFrame frame;
    if(!bwi_decode(raw, BWI_FRAME_SIZE, &frame) || frame.session != io->session ||
       (frame.op & ~(BWI_FLAG_READY | BWI_FLAG_FAULT)) ||
       (!frame.length && frame.sequence) || (frame.length && !frame.sequence)) {
        ++io->protocol_errors;
        return ESP_ERR_INVALID_RESPONSE;
    }
    io->ready = (frame.op & BWI_FLAG_READY) != 0;
    if(frame.op & BWI_FLAG_FAULT) {
        ++io->protocol_errors;
        return ESP_FAIL;
    }
    if(frame.ack != io->tx_sequence) {
        if(io->pending && frame.ack == io->pending_sequence) {
            io->tx_sequence = frame.ack;
            io->pending = false;
        } else {
            ++io->protocol_errors;
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    io->remote_empty = !frame.length;
    if(frame.length) {
        if(frame.sequence == io->rx_sequence) {
            ++io->duplicates;
        } else if(io->rx_sequence != UINT32_MAX && frame.sequence == io->rx_sequence + 1U) {
            if(frame.length > sizeof(io->rx) - io->rx_count) {
                ++io->protocol_errors;
                io->stage = "I2C RX buffer full";
                return ESP_ERR_NO_MEM;
            }
            for(unsigned i = 0; i < frame.length; ++i)
                io->rx[(io->rx_head + io->rx_count + i) % sizeof(io->rx)] = frame.payload[i];
            io->rx_count += frame.length;
            io->rx_sequence = frame.sequence; /* ACK only after durable copy into our buffer. */
            io->bytes_received += frame.length;
        } else {
            ++io->protocol_errors;
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    ++io->frames_received;
    return ESP_OK;
}

/* No repeated START: the firmware exports each operation as its own transaction. */
static esp_err_t exchange(Bw16I2c* io, const uint8_t request[BWI_FRAME_SIZE], bool* written) {
    if(written) *written = false;
    esp_err_t error = furi_hal_bw16_i2c_write(request, BWI_FRAME_SIZE);
    if(error != ESP_OK) { ++io->bus_errors; return error; }
    if(written) *written = true;
    uint8_t incoming[BWI_FRAME_SIZE];
    error = furi_hal_bw16_i2c_read(incoming, sizeof(incoming));
    if(error != ESP_OK) { ++io->bus_errors; return error; }
    return response(io, incoming);
}

static esp_err_t emergency_stop(Bw16I2c* io) {
    BwiFrame stop = {.op = BWI_OP_STOP, .session = io->session};
    uint8_t request[BWI_FRAME_SIZE];
    if(!bwi_encode(request, &stop)) return ESP_ERR_INVALID_ARG;
    for(unsigned attempt = 0; attempt < 3; ++attempt) {
        bool written = false;
        esp_err_t error = exchange(io, request, &written);
        if(error == ESP_OK || written) {
            /* The command state machine still waits for the normal DONE marker.
             * A failed read does not prevent the out-of-band STOP reaching BW16. */
            return ESP_OK;
        }
        ++io->retries;
        /* A short/aborted slave read may need its next task tick to rearm. */
        furi_delay_ms(5);
    }
    return ESP_FAIL;
}

bool bw16_i2c_open(Bw16I2c* io) {
    if(!io || io->installed) return false;
    memset(io, 0, sizeof(*io));
    io->stage = "Use BW16 Plus I2C firmware";
    io->error = ESP_ERR_NOT_SUPPORTED;
    if(!furi_hal_bw16_i2c_available()) return false;
    /* Each connection is a fresh epoch; identical OPEN retries preserve that epoch. */
    static uint32_t generation;
    io->session = furi_hal_random_get() ^ (++generation * UINT32_C(0x9e3779b9));
    if(!io->session) io->session = generation ? generation : 1;
    BwiFrame open = {.op = BWI_OP_OPEN, .session = io->session};
    uint8_t request[BWI_FRAME_SIZE];
    if(!bwi_encode(request, &open)) return false;
    io->stage = "I2C peer not ready";
    uint32_t start = clock_ms();
    for(unsigned attempt = 0; attempt < 64 && (uint32_t)(clock_ms() - start) < BW16_I2C_OPEN_MS; ++attempt) {
        io->error = exchange(io, request, NULL);
        if(io->error == ESP_OK && io->ready) {
            io->installed = true;
            io->stage = "I2C ready";
            return true;
        }
        ++io->retries;
        furi_delay_ms(10);
    }
    io->error = ESP_ERR_TIMEOUT;
    return false;
}

esp_err_t bw16_i2c_send(Bw16I2c* io, const char* line, size_t size) {
    if(!io || !io->installed) return ESP_ERR_INVALID_STATE;
    if(line && size == 5 && !memcmp(line, "STOP\n", 5)) {
        io->error = emergency_stop(io);
        return io->error;
    }
    if(io->tx_failed || io->pending || !io->ready) return ESP_ERR_INVALID_STATE;
    if(!line || size < 2 || size > BWI_PAYLOAD_SIZE || line[size - 1] != '\n' ||
       io->tx_sequence == UINT32_MAX) return ESP_ERR_INVALID_ARG;
    for(size_t i = 0; i + 1 < size; ++i)
        if((unsigned char)line[i] < 32 || (unsigned char)line[i] > 126) return ESP_ERR_INVALID_ARG;
    BwiFrame command = {.op = BWI_OP_COMMAND, .session = io->session,
                        .sequence = io->tx_sequence + 1U, .ack = io->rx_sequence,
                        .length = (uint8_t)size};
    memcpy(command.payload, line, size);
    uint8_t request[BWI_FRAME_SIZE];
    if(!bwi_encode(request, &command)) return ESP_ERR_INVALID_ARG;
    io->pending = true;
    io->pending_sequence = command.sequence;
    for(unsigned attempt = 0; attempt < BW16_I2C_RETRIES; ++attempt) {
        io->error = exchange(io, request, NULL);
        if(io->error == ESP_OK && !io->pending) {
            io->stage = "I2C ready";
            io->failures = 0;
            return ESP_OK;
        }
        ++io->retries;
        furi_delay_ms(5);
    }
    io->tx_failed = true;
    (void)emergency_stop(io); /* Uncertain command can never restart after a retry. */
    io->stage = "I2C command ACK timeout";
    io->error = ESP_ERR_TIMEOUT;
    return io->error;
}

int bw16_i2c_read(Bw16I2c* io, void* output, size_t capacity) {
    if(!io || !io->installed || !output || !capacity) return -1;
    if(!io->rx_count) {
        BwiFrame poll = {.op = BWI_OP_POLL, .session = io->session, .ack = io->rx_sequence};
        uint8_t request[BWI_FRAME_SIZE];
        if(!bwi_encode(request, &poll)) return -1;
        io->error = exchange(io, request, NULL);
        if(io->error != ESP_OK) {
            ++io->retries;
            /* Address NACKs can complete faster than the BW16's 1 ms rearm.
             * Space retries so three quick NACKs cannot look like link loss. */
            furi_delay_ms(5);
            if(++io->failures >= 3 || io->error == ESP_ERR_NO_MEM) {
                io->tx_failed = true;
                if(io->error != ESP_ERR_NO_MEM) io->stage = "I2C link error; reset BW16";
                return -1;
            }
            return 0;
        }
        io->failures = 0;
    }
    size_t count = io->rx_count < capacity ? io->rx_count : capacity;
    for(size_t i = 0; i < count; ++i)
        ((uint8_t*)output)[i] = io->rx[(io->rx_head + i) % sizeof(io->rx)];
    io->rx_head = (io->rx_head + count) % sizeof(io->rx);
    io->rx_count -= count;
    return (int)count;
}

esp_err_t bw16_i2c_buffered(Bw16I2c* io, size_t* size) {
    if(!io || !io->installed || !size) return ESP_ERR_INVALID_STATE;
    *size = io->rx_count + (io->remote_empty ? 0U : 1U);
    return ESP_OK;
}

esp_err_t bw16_i2c_close(Bw16I2c* io) {
    if(!io) return ESP_ERR_INVALID_ARG;
    /* The app's operation lifecycle confirms STOP before normal exit.
     * The shared bus belongs to the firmware, so closing changes only our state. */
    io->installed = io->ready = io->pending = false;
    io->rx_count = io->rx_head = 0;
    return ESP_OK;
}
