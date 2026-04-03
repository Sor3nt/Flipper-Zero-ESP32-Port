#include "../fatfs.h"

#include <string.h>

FATFS fatfs_object;

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
