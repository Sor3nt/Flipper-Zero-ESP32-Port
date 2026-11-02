#ifndef yo3gnd_fmtx_compat_h
#define yo3gnd_fmtx_compat_h

// ESP32 port: upstream targets a Cortex-M4 and uses the CMSIS barrier __DMB(). The ESP32-S3 is
// Xtensa: __sync_synchronize() compiles to MEMW, which orders the ring-buffer head/tail
// updates between the decode thread and the RMT refill (possibly on the other core).
#ifndef __DMB
#define __DMB() __sync_synchronize()
#endif

#endif
