#include "subghz_usb_export.h"
#include <stdio.h>

#define SUBGHZ_USB_EXPORT_VALUES_PER_LINE 64

void subghz_usb_export_start(uint32_t frequency, const char* preset_name) {
    printf("#SUBGHZ_RAW_START:%lu:%s\r\n", (unsigned long)frequency, preset_name);
}

void subghz_usb_export_send_data(const int32_t* data, uint16_t count) {
    uint16_t i = 0;
    while(i < count) {
        printf("#SUBGHZ_RAW_DATA:");
        uint16_t line_end = i + SUBGHZ_USB_EXPORT_VALUES_PER_LINE;
        if(line_end > count) line_end = count;
        for(; i < line_end; i++) {
            printf(" %ld", (long)data[i]);
        }
        printf("\r\n");
    }
}

void subghz_usb_export_stop(void) {
    printf("#SUBGHZ_RAW_STOP\r\n");
}
