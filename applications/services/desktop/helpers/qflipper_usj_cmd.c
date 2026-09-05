#include "qflipper_usj_cmd.h"

#include <furi_hal_usb_tinyusb_composite.h>

#include <string.h>

#define USJ_LINE_MAX 32

static char s_line[USJ_LINE_MAX];
static size_t s_len = 0;

bool qflipper_usj_cmd_poll(void) {
    uint8_t buf[64];
    size_t n = furi_hal_usb_serial_jtag_read(buf, sizeof(buf));
    if(n == 0) return false;

    bool hit = false;
    for(size_t i = 0; i < n; i++) {
        char c = (char)buf[i];
        if(c == '\r' || c == '\n') {
            s_line[s_len] = '\0';
            if(s_len > 0 && strcmp(s_line, QFLIPPER_USJ_COMMAND) == 0) {
                hit = true;
            }
            s_len = 0;
        } else if(s_len < sizeof(s_line) - 1) {
            s_line[s_len++] = c;
        } else {
            /* Zeile zu lang (Muell/Fremddaten) — verwerfen. */
            s_len = 0;
        }
    }

    if(hit) {
        static const char ack[] = "qflipper: starting bridge\r\n";
        furi_hal_usb_serial_jtag_write((const uint8_t*)ack, sizeof(ack) - 1);
    }
    return hit;
}
