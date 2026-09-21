#pragma once

#include <furi.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MENU_CUSTOM_FILENAME "menu_custom.txt"
#define MENU_CUSTOM_MAX_ITEMS (32)

/* Opis pozycji dodanych/usunietych do/z glownego menu. */
typedef struct {
    FuriString* hidden[MENU_CUSTOM_MAX_ITEMS];
    size_t hidden_count;

    FuriString* added_fap[MENU_CUSTOM_MAX_ITEMS];
    size_t added_fap_count;

    FuriString* added_internal[MENU_CUSTOM_MAX_ITEMS];
    size_t added_internal_count;

    /* User-defined main menu ordering: a list of entry keys (appid / .fap
     * path / "Applications") in the order the user wants them shown. Entries
     * not listed here keep their default relative order, appended after the
     * known ones. */
    FuriString* order[MENU_CUSTOM_MAX_ITEMS];
    size_t order_count;
} MenuCustom;

/* Load/Save przy pomocy Storage. Load nie tworzy pliku, gdy nie istnieje. */
void menu_custom_load(MenuCustom* custom);
void menu_custom_save(const MenuCustom* custom);
void menu_custom_free(MenuCustom* custom);

bool menu_custom_is_hidden(const MenuCustom* custom, const char* app_id);
bool menu_custom_is_fap_added(const MenuCustom* custom, const char* fap_path);
bool menu_custom_is_internal_added(const MenuCustom* custom, const char* app_id);

/* Zwraca true, gdy pozycja została faktycznie zmieniona. */
bool menu_custom_hide(MenuCustom* custom, const char* app_id);
bool menu_custom_unhide(MenuCustom* custom, const char* app_id);
bool menu_custom_add_fap(MenuCustom* custom, const char* fap_path);
bool menu_custom_remove_fap(MenuCustom* custom, const char* fap_path);
bool menu_custom_add_internal(MenuCustom* custom, const char* app_id);
bool menu_custom_remove_internal(MenuCustom* custom, const char* app_id);

/* Replace the saved menu order wholesale with `keys` (in order). */
void menu_custom_set_order(MenuCustom* custom, const char* const* keys, size_t count);

#ifdef __cplusplus
}
#endif