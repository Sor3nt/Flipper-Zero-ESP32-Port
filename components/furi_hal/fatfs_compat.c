#include "../fatfs.h"

#include <string.h>
#include <esp_attr.h>

/* In PSRAM (plain .bss without PSRAM): the SD diskio stages every PSRAM
 * buffer through its internal DMA bounce buffer (sd_buf_is_dma_capable() in
 * furi_hal_sd.c), so the 4 KiB sector window never needs internal RAM. */
EXT_RAM_BSS_ATTR FATFS fatfs_object;

void fatfs_init(void) {
    memset(&fatfs_object, 0, sizeof(fatfs_object));
}

FRESULT f_getlabel(const TCHAR* path, TCHAR* label, DWORD* vsn) {
    (void)path;
    if(label != NULL) {
        label[0] = '\0';
    }
    if(vsn != NULL) {
        *vsn = 0;
    }
    return FR_OK;
}

FRESULT f_setlabel(const TCHAR* label) {
    (void)label;
    return FR_OK;
}
