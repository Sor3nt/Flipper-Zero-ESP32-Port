#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// Case-insensitive Vergleich von n Bytes aus `a` gegen die ersten n Zeichen
// von `b`. `b` muss mind. n Zeichen lang sein (kein NUL-Check).
static inline bool wlan_ci_match(const uint8_t* a, const char* b, int n) {
    for(int i = 0; i < n; i++) {
        char x = (char)a[i];
        char y = b[i];
        if(x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if(y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if(x != y) return false;
    }
    return true;
}

// Case-insensitive memmem-Variante: Erstes Vorkommen von `needle` (NUL-
// terminiert) im Heuhaufen `hay`/`haylen`, oder NULL.
static inline const uint8_t* wlan_ci_mem_find(
    const uint8_t* hay, int haylen, const char* needle) {
    int nlen = (int)strlen(needle);
    if(nlen <= 0 || haylen < nlen) return NULL;
    for(int i = 0; i + nlen <= haylen; i++)
        if(wlan_ci_match(hay + i, needle, nlen)) return hay + i;
    return NULL;
}

#ifdef __cplusplus
}
#endif
