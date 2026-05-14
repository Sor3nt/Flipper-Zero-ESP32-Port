#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WLAN_MITM_PAYLOADS_DIR "/ext/wifi/mitm/payloads"
#define WLAN_MITM_PAYLOAD_NAME_MAX 24
#define WLAN_MITM_PAYLOAD_MAX 12

typedef struct {
    char name[WLAN_MITM_PAYLOAD_NAME_MAX]; // basename ohne .js
} WlanMitmPayloadEntry;

typedef struct {
    WlanMitmPayloadEntry items[WLAN_MITM_PAYLOAD_MAX];
    uint8_t count;
} WlanMitmPayloadList;

/** Scannt WLAN_MITM_PAYLOADS_DIR nach *.js und befüllt list. count == 0 wenn
 *  Verzeichnis fehlt oder leer ist. Idempotent. */
void wlan_mitm_payloads_scan(WlanMitmPayloadList* list);

/** Lädt den Inhalt von <DIR>/<name>.js in out (NUL-terminiert, ohne trailing
 *  Whitespace/CR/LF). Liefert true bei Erfolg, false wenn Datei fehlt oder
 *  leer / out_max zu klein. */
bool wlan_mitm_payloads_load(const char* name, char* out, size_t out_max);

#ifdef __cplusplus
}
#endif
