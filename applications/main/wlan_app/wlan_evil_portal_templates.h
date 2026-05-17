#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WLAN_EP_LOGIN_TEMPLATE_DIR  "/ext/wifi/evil_portal/login_template"
#define WLAN_EP_ROUTER_TEMPLATE_DIR "/ext/wifi/evil_portal/router_template"
#define WLAN_EP_TEMPLATE_NAME_MAX   24
#define WLAN_EP_TEMPLATE_MAX        12

typedef struct {
    char name[WLAN_EP_TEMPLATE_NAME_MAX]; // basename ohne .html
    bool is_router;                       // true = aus router_template/ (verify_creds)
} WlanEvilPortalTemplateEntry;

typedef struct {
    WlanEvilPortalTemplateEntry items[WLAN_EP_TEMPLATE_MAX];
    uint8_t count;
} WlanEvilPortalTemplateList;

/** Scannt login_template/ und router_template/ nach *.html und befüllt list
 *  (login zuerst, dann router). count == 0 wenn beide Verzeichnisse fehlen
 *  oder leer sind. Idempotent. */
void wlan_evil_portal_templates_scan(WlanEvilPortalTemplateList* list);

/** Lädt den Inhalt der zu entry gehörenden HTML-Datei in einen frisch per
 *  malloc allokierten Buffer (NUL-terminiert). *out_len = Länge ohne NUL.
 *  Caller muss free()n. Liefert NULL bei Fehler / leerer Datei. */
char* wlan_evil_portal_templates_load(
    const WlanEvilPortalTemplateEntry* entry, size_t* out_len);

#ifdef __cplusplus
}
#endif
