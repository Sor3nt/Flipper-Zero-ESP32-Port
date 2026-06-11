#pragma once

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* rgbcmp compares two 3-byte RGB color values */
static inline int rgbcmp(const void* a, const void* b) {
    return memcmp(a, b, 3);
}

#ifdef __cplusplus
}
#endif
