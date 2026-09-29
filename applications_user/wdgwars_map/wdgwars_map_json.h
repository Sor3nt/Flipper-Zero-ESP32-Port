#pragma once
#include "wdgwars_map_model.h"
typedef struct {
    const char* data;
    size_t length, offset;
    unsigned depth;
    bool error;
    /* Diagnostics contain internal literals and an offset, never response text. */
    const char* field;
    const char* error_field;
    const char* reason;
    size_t error_offset;
} WdgJson;
void wdg_json_init(WdgJson* json, const char* data, size_t length);
bool wdg_json_object(WdgJson* json);
bool wdg_json_array(WdgJson* json);
/* next returns 1=item, 0=container end, -1=error. Set first=true before loop. */
int wdg_json_member(WdgJson* json, bool* first, char* key, size_t capacity);
int wdg_json_element(WdgJson* json, bool* first);
bool wdg_json_string(WdgJson* json, char* text, size_t capacity);
/* Truncated variants consume/validate the whole string and preserve a UTF-8 prefix. */
bool wdg_json_string_truncated(WdgJson* json, char* text, size_t capacity);
int wdg_json_member_truncated(WdgJson* json, bool* first, char* key, size_t capacity);
bool wdg_json_number(WdgJson* json, double* value);
bool wdg_json_uint(WdgJson* json, uint32_t* value);
bool wdg_json_bool(WdgJson* json, bool* value);
bool wdg_json_null(WdgJson* json);
bool wdg_json_skip(WdgJson* json);
bool wdg_json_finish(WdgJson* json);
/* Parsing commits to output only on success. Caller retains previous cache on error.
 * Contracts checked against authenticated member-territories and territories feeds.
 * Unknown geometry is deliberately NOT interpreted as a fixed 2 km grid. */
bool wdg_map_parse_territories(const char* data, size_t length, WdgMapSnapshot* output, char* error, size_t error_capacity);
/* Global overview preserves every API hull, including disjoint hulls for one gang. */
bool wdg_map_parse_overview(const char* data, size_t length, WdgMapSnapshot* output, char* error, size_t error_capacity);
bool wdg_map_parse_account(const char* data, size_t length, WdgMapAccount* output, char* error, size_t error_capacity);
