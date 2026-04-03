#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <furi.h>

typedef void (*PropertyValueCallback)(const char* key, const char* value, bool last, void* context);

typedef struct {
    FuriString* key;
    FuriString* value;
    PropertyValueCallback out;
    char sep;
    bool last;
    void* context;
} PropertyValueContext;

void property_value_out(PropertyValueContext* ctx, const char* fmt, unsigned int nparts, ...);

#ifdef __cplusplus
}
#endif
