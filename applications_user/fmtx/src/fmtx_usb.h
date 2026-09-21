#ifndef yo3gnd_fmtx_usb_d129
#define yo3gnd_fmtx_usb_d129

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef void (*FmtxUsbRx)(const int16_t* samples, size_t count, void* ctx);

bool fmtx_usb_start(FmtxUsbRx callback, void* ctx);
bool fmtx_usb_stop(void);
bool fmtx_usb_connected(void);

#endif
