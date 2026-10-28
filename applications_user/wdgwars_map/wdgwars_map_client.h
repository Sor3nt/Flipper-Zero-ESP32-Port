#pragma once
#include "wdgwars_map_model.h"
typedef bool (*WdgMapCancel)(void* context);
typedef enum { WdgMapServiceWdgWars, WdgMapServiceOverpass } WdgMapService;
typedef enum {
    WdgMapClientOk, WdgMapClientInvalid, WdgMapClientOffline, WdgMapClientBusy,
    WdgMapClientCancelled, WdgMapClientTimeout, WdgMapClientNetwork,
    WdgMapClientAuth, WdgMapClientRateLimit, WdgMapClientHttp,
    WdgMapClientTooLarge, WdgMapClientSchema, WdgMapClientMemory
} WdgMapClientStatus;
typedef struct {
    WdgMapService service;
    const char* path; /* Relative allowlisted route; never a URL. */
    const char* api_key; /* WDG only; rejected when service is Overpass. */
    char* body; /* Caller-owned capacity incl NUL, <= WDG_MAP_RESPONSE_MAX+1. */
    size_t capacity, length;
    WdgMapCancel cancelled;
    void* context;
    WdgMapClientStatus status;
    int http_status;
    uint32_t retry_after_s;
    char message[64];
} WdgMapHttpRequest;
/* Blocking off-UI call. Borrows existing WLAN worker/connection, TLS verified;
 * no radio lifecycle changes, redirects, uploads or automatic retries.
 * Overall deadline 60s; cancellation waits at most a blocked I/O call
 * (10s WDG, 25s Overpass, whose query timeout is 20s). */
bool wdg_map_http_get(WdgMapHttpRequest* request);
typedef struct {
    const char* api_key;
    WdgMapBounds bounds;
    uint8_t zoom;
    uint32_t downloaded_at; /* Caller supplies UTC epoch, 0 if unknown. */
    WdgMapCancel cancelled;
    void* context;
    WdgMapSnapshot* map;
    WdgMapAccount* account;
    WdgMapClientStatus status;
    int http_status;
    uint32_t retry_after_s;
    size_t bytes_received;
    char message[64];
} WdgMapFetch;
/* Each commits output only after a complete successful parse. */
bool wdg_map_fetch_territories(WdgMapFetch* fetch);
bool wdg_map_fetch_account(WdgMapFetch* fetch);
