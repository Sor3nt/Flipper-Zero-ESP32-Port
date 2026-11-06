#include <gui.h>
#include <view_dispatcher.h>
#include <menu.h>
#include <submenu.h>
#include <variable_item_list.h>
#include <assets_icons.h>
#include <applications.h>
#include <dialogs/dialogs.h>
#include <flipper_application/flipper_application.h>
#include <storage/storage.h>
#include <toolbox/path.h>
#include <archive/helpers/archive_favorites.h>
#include <esp_rom_sys.h>
#include <momentum_settings.h>
#include <stddef.h>
#include <string.h>

#include "loader.h"
#include "loader_menu.h"
#include "menu_custom.h"

#define LOADER_ASSET_PACKS_MAX 64

/* Desktop settings mirror: layout-compatible with
 * applications/services/desktop/desktop_settings.h so we can call the
 * desktop_settings_load/save/services-api functions linked into the final
 * image without pulling the FAM include path into this component. */
typedef struct {
    char name_or_path[128];
} LoaderMenuFavoriteApp;

typedef struct {
    uint32_t auto_lock_delay_ms;
    uint8_t usb_inhibit_auto_lock;
    uint8_t displayBatteryPercentage;
    uint8_t dummy_mode;
    uint8_t display_clock;
    LoaderMenuFavoriteApp favorite_apps[5];
    LoaderMenuFavoriteApp dummy_apps[9];
    uint8_t display_status_bar_background;
} LoaderMenuDesktopSettings;

typedef struct Desktop LoaderMenuDesktop;
typedef struct LoaderMenuPower LoaderMenuPower;

#define LOADER_DISPLAY_BATTERY_OFF              6
#define LOADER_DISPLAY_BATTERY_BAR              0
#define LOADER_DISPLAY_BATTERY_PERCENT          1
#define LOADER_DISPLAY_BATTERY_INVERTED_PERCENT 2
#define LOADER_DISPLAY_BATTERY_RETRO_3          3
#define LOADER_DISPLAY_BATTERY_RETRO_5          4
#define LOADER_DISPLAY_BATTERY_BAR_PERCENT      5

void desktop_settings_load(LoaderMenuDesktopSettings* settings);
void desktop_settings_save(const LoaderMenuDesktopSettings* settings);
void desktop_api_get_settings(LoaderMenuDesktop* instance, LoaderMenuDesktopSettings* settings);
void desktop_api_set_settings(
    LoaderMenuDesktop* instance,
    const LoaderMenuDesktopSettings* settings);
void desktop_api_reload_animation(LoaderMenuDesktop* instance);
void desktop_api_set_loader_menu_active(LoaderMenuDesktop* instance, bool active);
void power_trigger_ui_update(LoaderMenuPower* power);
void bt_statusbar_refresh(void);

#define LOADER_RECORD_DESKTOP "desktop"
#define LOADER_RECORD_POWER "power"

#define TAG "LoaderMenu"

#define LOADER_MENU_STACK_SIZE (8192)

#define LOADER_MENU_MAX_ENTRIES (96)
#define LOADER_MENU_MAX_FAPS MENU_CUSTOM_MAX_ITEMS

struct LoaderMenu {
    FuriThread* thread;
    void (*closed_cb)(void*);
    void* context;
    bool settings_first;
};

static void loader_menu_trace(const char* step) {
    esp_rom_printf(
        "\r\n[LM] %s free=%u\r\n",
        step,
        (unsigned)furi_thread_get_stack_space(furi_thread_get_current_id()));
}

static int32_t loader_menu_thread(void* p);

LoaderMenu* loader_menu_alloc(void (*closed_cb)(void*), void* context) {
    LoaderMenu* loader_menu = malloc(sizeof(LoaderMenu));
    loader_menu->closed_cb = closed_cb;
    loader_menu->context = context;
    loader_menu->settings_first = false;
    loader_menu->thread =
        furi_thread_alloc_ex(TAG, LOADER_MENU_STACK_SIZE, loader_menu_thread, loader_menu);
    furi_thread_start(loader_menu->thread);
    return loader_menu;
}

LoaderMenu* loader_menu_alloc_settings_first(void (*closed_cb)(void*), void* context) {
    LoaderMenu* loader_menu = malloc(sizeof(LoaderMenu));
    loader_menu->closed_cb = closed_cb;
    loader_menu->context = context;
    loader_menu->settings_first = true;
    loader_menu->thread =
        furi_thread_alloc_ex(TAG, LOADER_MENU_STACK_SIZE, loader_menu_thread, loader_menu);
    furi_thread_start(loader_menu->thread);
    return loader_menu;
}

void loader_menu_free(LoaderMenu* loader_menu) {
    furi_assert(loader_menu);
    furi_thread_join(loader_menu->thread);
    furi_thread_free(loader_menu->thread);
    free(loader_menu);
}

typedef enum {
    LoaderMenuViewPrimary,
    LoaderMenuViewSettings,
    LoaderMenuViewInterface,
    LoaderMenuViewInterfaceMain,
    LoaderMenuViewInterfaceLock,
    LoaderMenuViewInterfaceStatus,
    LoaderMenuViewInterfaceGraphics,
    LoaderMenuViewAdd,
    LoaderMenuViewAddMain,
    LoaderMenuViewRemove,
} LoaderMenuView;

typedef enum {
    LoaderMenuEntryKindApplications, // "Apps" (FAP browser)
    LoaderMenuEntryKindInternal, // FLIPPER_APPS[i]
    LoaderMenuEntryKindExternal, // FLIPPER_EXTERNAL_APPS[i]
    LoaderMenuEntryKindInternalAdded, // custom added internal app (FLIPPER_SYSTEM_APPS[i])
    LoaderMenuEntryKindFap, // custom added .fap
    LoaderMenuEntryKindSettings, // switch to settings submenu
} LoaderMenuEntryKind;

typedef struct {
    LoaderMenuEntryKind kind;
    uint32_t index; // array index of the source app / fap slot
} LoaderMenuEntry;

/* Runtime Icon, layout-compatible with gui/icon_i.h Icon (fields are const there,
 * so we build it through this writable mirror and cast when adding to the menu). */
typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t frame_count;
    uint8_t frame_rate;
    const uint8_t* const* frames;
} LoaderMenuRuntimeIcon;

typedef struct {
    FuriString* path; // full .fap path
    FuriString* name; // display name (from manifest or fallback)
    uint8_t icon_data[FAP_MANIFEST_MAX_ICON_SIZE]; // manifest icon incl. flag byte
    const uint8_t* frames[1]; // -> icon_data
    LoaderMenuRuntimeIcon icon; // {10,10,1,0,frames}
} LoaderMenuFapSlot;

typedef struct {
    Gui* gui;
    Storage* storage;
    DialogsApp* dialogs;
    ViewDispatcher* view_dispatcher;
    Menu* primary_menu;
    Submenu* settings_menu;
    Submenu* interface_menu;
    VariableItemList* interface_main_list;
    VariableItemList* interface_lock_list;
    VariableItemList* interface_status_list;
    VariableItemList* interface_graphics_list;
    FuriString* asset_pack_names[LOADER_ASSET_PACKS_MAX];
    size_t asset_pack_count;
    size_t asset_pack_index;
    Submenu* add_menu;
    Submenu* add_main_menu;
    Submenu* remove_menu;
    FuriString* browser_path;

    MenuCustom custom;
    LoaderMenuEntry entries[LOADER_MENU_MAX_ENTRIES];
    size_t entries_count;
    LoaderMenuFapSlot faps[LOADER_MENU_MAX_FAPS];
} LoaderMenuApp;

static void loader_menu_start(const char* name) {
    Loader* loader = furi_record_open(RECORD_LOADER);
    loader_start_with_gui_error(loader, name, NULL);
    furi_record_close(RECORD_LOADER);
}

static void loader_menu_switch_to_view(LoaderMenuApp* app, LoaderMenuView view) {
    view_dispatcher_switch_to_view(app->view_dispatcher, view);
}

static void loader_menu_build_remove(LoaderMenuApp* app);
static void loader_menu_build_add_menu(LoaderMenuApp* app);
static void loader_menu_build_add_main(LoaderMenuApp* app);

/* ---------------------------------------------------------------- */
/* Primary menu (entries table + dispatch callback)                  */
/* ---------------------------------------------------------------- */

static void loader_menu_primary_callback(void* context, uint32_t index) {
    LoaderMenuApp* app = context;
    furi_assert(index < app->entries_count);
    const LoaderMenuEntry* entry = &app->entries[index];

    switch(entry->kind) {
    case LoaderMenuEntryKindApplications:
        loader_menu_start(LOADER_APPLICATIONS_NAME);
        break;
    case LoaderMenuEntryKindInternal:
        loader_menu_start(FLIPPER_APPS[entry->index].name);
        break;
    case LoaderMenuEntryKindExternal:
        loader_menu_start(FLIPPER_EXTERNAL_APPS[entry->index].path);
        break;
    case LoaderMenuEntryKindInternalAdded:
        loader_menu_start(FLIPPER_SYSTEM_APPS[entry->index].name);
        break;
    case LoaderMenuEntryKindFap:
        loader_menu_start(furi_string_get_cstr(app->faps[entry->index].path));
        break;
    case LoaderMenuEntryKindSettings:
        loader_menu_switch_to_view(app, LoaderMenuViewSettings);
        break;
    }
}

static size_t loader_menu_entries_add(LoaderMenuApp* app, LoaderMenuEntryKind kind, uint32_t index) {
    furi_assert(app->entries_count < LOADER_MENU_MAX_ENTRIES);
    LoaderMenuEntry* entry = &app->entries[app->entries_count];
    entry->kind = kind;
    entry->index = index;
    return app->entries_count++;
}

static const char* loader_menu_entries_label(const LoaderMenuApp* app, size_t pos) {
    const LoaderMenuEntry* entry = &app->entries[pos];
    switch(entry->kind) {
    case LoaderMenuEntryKindApplications:
        return LOADER_APPLICATIONS_NAME;
    case LoaderMenuEntryKindInternal:
        return FLIPPER_APPS[entry->index].name;
    case LoaderMenuEntryKindExternal:
        return FLIPPER_EXTERNAL_APPS[entry->index].name;
    case LoaderMenuEntryKindInternalAdded:
        return FLIPPER_SYSTEM_APPS[entry->index].name;
    case LoaderMenuEntryKindFap:
        return furi_string_get_cstr(app->faps[entry->index].name);
    case LoaderMenuEntryKindSettings:
        return "Settings";
    }
    return "";
}

static const Icon* loader_menu_entries_icon(const LoaderMenuApp* app, size_t pos) {
    const LoaderMenuEntry* entry = &app->entries[pos];
    switch(entry->kind) {
    case LoaderMenuEntryKindApplications:
        return &A_Plugins_14;
    case LoaderMenuEntryKindInternal:
        return FLIPPER_APPS[entry->index].icon;
    case LoaderMenuEntryKindExternal:
        return FLIPPER_EXTERNAL_APPS[entry->index].icon;
    case LoaderMenuEntryKindInternalAdded:
        return FLIPPER_SYSTEM_APPS[entry->index].icon;
    case LoaderMenuEntryKindFap:
        return (const Icon*)&app->faps[entry->index].icon;
    case LoaderMenuEntryKindSettings:
        return &A_Settings_14;
    }
    return NULL;
}

/* Load a .fap's display name and 10x10 icon into a persistent slot. */
static bool loader_menu_fap_slot_load(LoaderMenuApp* app, size_t slot_idx, const char* path) {
    LoaderMenuFapSlot* slot = &app->faps[slot_idx];
    furi_string_set(slot->path, path);

    uint8_t* icon_ptr = slot->icon_data;
    furi_string_reset(slot->name);
    bool ok = flipper_application_load_name_and_icon(slot->path, app->storage, &icon_ptr, slot->name);

    if(!ok || furi_string_empty(slot->name)) {
        path_extract_filename(slot->path, slot->name, true);
    }
    if(!ok) {
        memset(slot->icon_data, 0, sizeof(slot->icon_data));
    }

    slot->frames[0] = slot->icon_data;
    slot->icon.width = 10;
    slot->icon.height = 10;
    slot->icon.frame_count = 1;
    slot->icon.frame_rate = 0;
    slot->icon.frames = slot->frames;
    return ok;
}

static int32_t loader_menu_system_app_index_by_appid(const char* appid) {
    for(size_t i = 0; i < FLIPPER_SYSTEM_APPS_COUNT; ++i) {
        if(FLIPPER_SYSTEM_APPS[i].appid && (strcmp(FLIPPER_SYSTEM_APPS[i].appid, appid) == 0)) {
            return (int32_t)i;
        }
    }
    return -1;
}

/* Is this appid currently visible in the primary menu? */
static bool loader_menu_app_is_visible(const LoaderMenuApp* app, const char* appid) {
    for(size_t i = 0; i < FLIPPER_APPS_COUNT; ++i) {
        if(FLIPPER_APPS[i].appid && (strcmp(FLIPPER_APPS[i].appid, appid) == 0)) {
            return !menu_custom_is_hidden(&app->custom, appid);
        }
    }
    for(size_t i = 0; i < FLIPPER_EXTERNAL_APPS_COUNT; ++i) {
        if(FLIPPER_EXTERNAL_APPS[i].path && (strcmp(FLIPPER_EXTERNAL_APPS[i].path, appid) == 0)) {
            return !menu_custom_is_hidden(&app->custom, appid);
        }
    }
    return menu_custom_is_internal_added(&app->custom, appid);
}

/* Is this appid shown as a default (non-hidden) menu entry, ignoring added_internal? */
static bool loader_menu_is_default_visible(const LoaderMenuApp* app, const char* appid) {
    for(size_t i = 0; i < FLIPPER_APPS_COUNT; ++i) {
        if(FLIPPER_APPS[i].appid && (strcmp(FLIPPER_APPS[i].appid, appid) == 0)) {
            return !menu_custom_is_hidden(&app->custom, appid);
        }
    }
    for(size_t i = 0; i < FLIPPER_EXTERNAL_APPS_COUNT; ++i) {
        if(FLIPPER_EXTERNAL_APPS[i].path && (strcmp(FLIPPER_EXTERNAL_APPS[i].path, appid) == 0)) {
            return !menu_custom_is_hidden(&app->custom, appid);
        }
    }
    return false;
}

/* Is this appid one of the default-menu external apps (unhide instead of add)? */
static bool loader_menu_is_default_external(const char* appid) {
    for(size_t i = 0; i < FLIPPER_EXTERNAL_APPS_COUNT; ++i) {
        if(FLIPPER_EXTERNAL_APPS[i].path && (strcmp(FLIPPER_EXTERNAL_APPS[i].path, appid) == 0)) {
            return true;
        }
    }
    return false;
}

static void loader_menu_build_menu(LoaderMenuApp* app) {
    menu_reset(app->primary_menu);
    app->entries_count = 0;

    if(!menu_custom_is_hidden(&app->custom, LOADER_APPLICATIONS_NAME)) {
        loader_menu_entries_add(app, LoaderMenuEntryKindApplications, 0);
    }

    for(size_t i = 0; i < FLIPPER_APPS_COUNT; ++i) {
        if(FLIPPER_APPS[i].appid && menu_custom_is_hidden(&app->custom, FLIPPER_APPS[i].appid)) {
            continue;
        }
        loader_menu_entries_add(app, LoaderMenuEntryKindInternal, (uint32_t)i);
    }

    for(size_t i = 0; i < FLIPPER_EXTERNAL_APPS_COUNT; ++i) {
        if(FLIPPER_EXTERNAL_APPS[i].path &&
           menu_custom_is_hidden(&app->custom, FLIPPER_EXTERNAL_APPS[i].path)) {
            continue;
        }
        loader_menu_entries_add(app, LoaderMenuEntryKindExternal, (uint32_t)i);
    }

    for(size_t i = 0; i < app->custom.added_internal_count; ++i) {
        const char* appid = furi_string_get_cstr(app->custom.added_internal[i]);
        int32_t sys_idx = loader_menu_system_app_index_by_appid(appid);
        if(sys_idx < 0) continue;
        if(loader_menu_app_is_visible(app, appid)) continue; // already shown as default
        loader_menu_entries_add(app, LoaderMenuEntryKindInternalAdded, (uint32_t)sys_idx);
    }

    for(size_t i = 0; i < app->custom.added_fap_count; ++i) {
        const char* path = furi_string_get_cstr(app->custom.added_fap[i]);
        loader_menu_fap_slot_load(app, i, path);
        loader_menu_entries_add(app, LoaderMenuEntryKindFap, (uint32_t)i);
    }

    if((FLIPPER_EXTSETTINGS_APPS_COUNT > 0) || (FLIPPER_SETTINGS_APPS_COUNT > 0)) {
        if(!menu_custom_is_hidden(&app->custom, "Settings")) {
            loader_menu_entries_add(app, LoaderMenuEntryKindSettings, 0);
        }
    }

    for(size_t i = 0; i < app->entries_count; ++i) {
        menu_add_item(
            app->primary_menu,
            loader_menu_entries_label(app, i),
            loader_menu_entries_icon(app, i),
            (uint32_t)i,
            loader_menu_primary_callback,
            app);
    }
}

/* ---------------------------------------------------------------- */
/* Settings submenu                                                  */
/* ---------------------------------------------------------------- */

static void loader_menu_build_interface(LoaderMenuApp* app);

static void
    loader_menu_settings_menu_callback(void* context, InputType input_type, uint32_t index) {
    UNUSED(context);
    if(input_type == InputTypeShort) {
        loader_menu_start((const char*)index);
    } else if(input_type == InputTypeLong) {
        archive_favorites_handle_setting_pin_unpin((const char*)index, NULL);
    }
}

static void loader_menu_update_fw_callback(void* context, uint32_t index) {
    UNUSED(context);
    UNUSED(index);
    /* Firmware-/SD-Update lebt in der WiFi-App; dort in den kombinierten
     * Update-Flow springen (Launch-Arg "update"). Gleiches Muster wie der
     * Web-Filesystem-Start aus dem Desktop-Lock-Menü. */
    Loader* loader = furi_record_open(RECORD_LOADER);
    loader_start_detached_with_gui_error(loader, "wlan", "update");
    furi_record_close(RECORD_LOADER);
}

static void loader_menu_settings_interface_callback(void* context, uint32_t index) {
    UNUSED(index);
    LoaderMenuApp* app = context;
    loader_menu_build_interface(app);
    loader_menu_switch_to_view(app, LoaderMenuViewInterface);
}

static void loader_menu_build_submenu(LoaderMenuApp* app) {
    for(size_t i = 0; i < FLIPPER_EXTSETTINGS_APPS_COUNT; i++) {
        submenu_add_item_ex(
            app->settings_menu,
            FLIPPER_EXTSETTINGS_APPS[i].name,
            (uint32_t)FLIPPER_EXTSETTINGS_APPS[i].path,
            loader_menu_settings_menu_callback,
            app);
        /* "Update Firmware" direkt nach "About" einfügen (vor "Bluetooth"). */
        if(strcmp(FLIPPER_EXTSETTINGS_APPS[i].path, "about") == 0) {
            submenu_add_item(
                app->settings_menu,
                "Update Firmware",
                0,
                loader_menu_update_fw_callback,
                app);
        }
    }
    for(size_t i = 0; i < FLIPPER_SETTINGS_APPS_COUNT; i++) {
        /* "Interface" is handled by our own Interface submenu below - skip the
         * stock interface_settings app so we don't get two entries. */
        if(strcmp(FLIPPER_SETTINGS_APPS[i].appid, "interface_settings") == 0) {
            continue;
        }
        submenu_add_item_ex(
            app->settings_menu,
            FLIPPER_SETTINGS_APPS[i].name,
            (uint32_t)FLIPPER_SETTINGS_APPS[i].name,
            loader_menu_settings_menu_callback,
            app);
    }
    submenu_add_item(
        app->settings_menu, "Interface", 0, loader_menu_settings_interface_callback, app);
}

/* ---------------------------------------------------------------- */
/* Interface submenu (Add/Remove Item)                               */
/* ---------------------------------------------------------------- */

/* ---------------------------------------------------------------- */
/* Interface submenu (Main menu / Lock screen / Status bar)         */
/* ---------------------------------------------------------------- */

/* Tab callbacks: switch into the desired Interface sub-tab */
static void loader_menu_interface_go_main(void* context, uint32_t index) {
    UNUSED(index);
    LoaderMenuApp* app = context;
    loader_menu_switch_to_view(app, LoaderMenuViewInterfaceMain);
}

static void loader_menu_interface_go_lock(void* context, uint32_t index) {
    UNUSED(index);
    LoaderMenuApp* app = context;
    loader_menu_switch_to_view(app, LoaderMenuViewInterfaceLock);
}

static void loader_menu_interface_go_status(void* context, uint32_t index) {
    UNUSED(index);
    LoaderMenuApp* app = context;
    loader_menu_switch_to_view(app, LoaderMenuViewInterfaceStatus);
}

static void loader_menu_build_interface_graphics(LoaderMenuApp* app);

static void loader_menu_interface_go_graphics(void* context, uint32_t index) {
    UNUSED(index);
    LoaderMenuApp* app = context;
    loader_menu_build_interface_graphics(app);
    loader_menu_switch_to_view(app, LoaderMenuViewInterfaceGraphics);
}

static void loader_menu_build_interface(LoaderMenuApp* app) {
    submenu_reset(app->interface_menu);
    submenu_set_header(app->interface_menu, "Interface:");
    submenu_add_item(app->interface_menu, "Graphics", 0, loader_menu_interface_go_graphics, app);
    submenu_add_item(app->interface_menu, "Main Menu", 1, loader_menu_interface_go_main, app);
    submenu_add_item(app->interface_menu, "Lock Screen", 2, loader_menu_interface_go_lock, app);
    submenu_add_item(app->interface_menu, "Status Bar", 3, loader_menu_interface_go_status, app);
}

/* ---------------------------------------------------------------- */
/* Interface -> Main Menu tab (menu style + Add/Remove Item)        */
/* ---------------------------------------------------------------- */

static const char* const loader_menu_menu_style_text[MenuStyleCount] = {
    "List",
    "Wii",
    "DSi",
    "PS4",
    "Vertical",
    "C64",
    "Compact",
    "MNTM",
    "CoverFlow",
};

static void loader_menu_main_menu_style_changed(VariableItem* item) {
    uint32_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, loader_menu_menu_style_text[index]);
    menu_set_style((MenuStyle)index);
}

static void loader_menu_main_menu_enter(void* context, uint32_t index) {
    LoaderMenuApp* app = context;
    if(index == 1) {
        loader_menu_build_add_menu(app);
        loader_menu_switch_to_view(app, LoaderMenuViewAdd);
    } else if(index == 2) {
        loader_menu_build_remove(app);
        loader_menu_switch_to_view(app, LoaderMenuViewRemove);
    }
}

static void loader_menu_build_interface_main(LoaderMenuApp* app) {
    variable_item_list_reset(app->interface_main_list);
    variable_item_list_set_header(app->interface_main_list, "Main Menu:");

    VariableItem* item = variable_item_list_add(
        app->interface_main_list,
        "Menu style",
        MenuStyleCount,
        loader_menu_main_menu_style_changed,
        app);
    uint32_t menu_style = (uint32_t)menu_get_style();
    if(menu_style >= MenuStyleCount) {
        menu_style = 0;
    }
    variable_item_set_current_value_index(item, (uint8_t)menu_style);
    variable_item_set_current_value_text(item, loader_menu_menu_style_text[menu_style]);

    variable_item_list_add(app->interface_main_list, "Add Item", 0, NULL, app);
    variable_item_list_add(app->interface_main_list, "Remove Item", 0, NULL, app);
    variable_item_list_set_enter_callback(app->interface_main_list, loader_menu_main_menu_enter, app);
}

/* ---------------------------------------------------------------- */
/* Interface -> Lock screen tab                                      */
/* ---------------------------------------------------------------- */

static const char* const loader_menu_lock_style_text[LockScreenStyleCount] = {
    "Default",
    "Momentum",
};

static void loader_menu_lock_style_changed(VariableItem* item) {
    uint32_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, loader_menu_lock_style_text[index]);
    lock_screen_set_style((LockScreenStyle)index);
}

typedef struct {
    const char* label;
    MomentumSettingsBool setting;
} LoaderMenuLockToggle;

#define LOCK_TOGGLE(label, setting) {label, MomentumSettingsBool##setting}

static const LoaderMenuLockToggle loader_menu_lock_toggles[] = {
    LOCK_TOGGLE("Lock on Boot if PIN set", LockOnBoot),
    LOCK_TOGGLE("Format on 10 Bad PINs", BadPinsFormat),
    LOCK_TOGGLE("Allow USB RPC While Locked", AllowLockedRpcUsb),
    LOCK_TOGGLE("Allow BLE RPC While Locked", AllowLockedRpcBle),
    LOCK_TOGGLE("Allow Poweroff", LockscreenPoweroff),
    LOCK_TOGGLE("Show Time", LockscreenTime),
    LOCK_TOGGLE("Show Seconds", LockscreenSeconds),
    LOCK_TOGGLE("Show Date", LockscreenDate),
    LOCK_TOGGLE("Show Statusbar", LockscreenStatusbar),
    LOCK_TOGGLE("Unlock Prompt", LockscreenPrompt),
    LOCK_TOGGLE("Transparent (see anim)", LockscreenTransparent),
    LOCK_TOGGLE("Skip Sliding Animation", LockscreenSkipAnimation),
};

#define LOADER_MENU_LOCK_TOGGLE_COUNT \
    (sizeof(loader_menu_lock_toggles) / sizeof(loader_menu_lock_toggles[0]))

static MomentumSettingsBool loader_menu_lock_setting(size_t index) {
    furi_assert(index < LOADER_MENU_LOCK_TOGGLE_COUNT);
    return loader_menu_lock_toggles[index].setting;
}

static void loader_menu_lock_toggle_changed(VariableItem* item) {
    size_t index = (size_t)(uintptr_t)variable_item_get_context(item);
    furi_assert(index < LOADER_MENU_LOCK_TOGGLE_COUNT);
    bool value = variable_item_get_current_value_index(item) != 0;
    variable_item_set_current_value_text(item, value ? "ON" : "OFF");
    momentum_settings_set_bool(loader_menu_lock_setting(index), value);
    momentum_settings_save();
}

static void loader_menu_build_interface_lock(LoaderMenuApp* app) {
    variable_item_list_reset(app->interface_lock_list);
    variable_item_list_set_header(app->interface_lock_list, "Lock Screen:");

    VariableItem* item = variable_item_list_add(
        app->interface_lock_list, "Style", LockScreenStyleCount, loader_menu_lock_style_changed, app);
    uint32_t style = (uint32_t)lock_screen_get_style();
    if(style >= LockScreenStyleCount) {
        style = 0;
    }
    variable_item_set_current_value_index(item, (uint8_t)style);
    variable_item_set_current_value_text(item, loader_menu_lock_style_text[style]);

    for(size_t i = 0; i < LOADER_MENU_LOCK_TOGGLE_COUNT; i++) {
        item = variable_item_list_add(
            app->interface_lock_list,
            loader_menu_lock_toggles[i].label,
            2,
            loader_menu_lock_toggle_changed,
            (void*)(uintptr_t)i);
        bool value = momentum_settings_get_bool(loader_menu_lock_setting(i));
        variable_item_set_current_value_index(item, value ? 1 : 0);
        variable_item_set_current_value_text(item, value ? "ON" : "OFF");
    }
}

/* ---------------------------------------------------------------- */
/* Interface -> Status bar tab                                       */
/* ---------------------------------------------------------------- */

#define LOADER_BATTERY_VIEW_COUNT 7
#define LOADER_CLOCK_ENABLE_COUNT 2
#define LOADER_STATUS_BAR_BG_COUNT 2

static const char* loader_menu_battery_view_text[LOADER_BATTERY_VIEW_COUNT] = {
    "OFF",
    "Bar",
    "%",
    "Inv. %",
    "Retro 3",
    "Retro 5",
    "Bar %",
};

static const uint32_t loader_menu_battery_view_value[LOADER_BATTERY_VIEW_COUNT] = {
    LOADER_DISPLAY_BATTERY_OFF,
    LOADER_DISPLAY_BATTERY_BAR,
    LOADER_DISPLAY_BATTERY_PERCENT,
    LOADER_DISPLAY_BATTERY_INVERTED_PERCENT,
    LOADER_DISPLAY_BATTERY_RETRO_3,
    LOADER_DISPLAY_BATTERY_RETRO_5,
    LOADER_DISPLAY_BATTERY_BAR_PERCENT,
};

typedef struct {
    const char* label;
    ViewPortStatusBarIcon icon;
} LoaderMenuStatusIconToggle;

static const LoaderMenuStatusIconToggle loader_menu_status_icon_toggles[] = {
    {"SD Card Icon", ViewPortStatusBarIconSdCard},
    {"Bluetooth Icon", ViewPortStatusBarIconBluetooth},
    {"BLE Beacon Icon", ViewPortStatusBarIconBleBeacon},
    {"WiFi Icon", ViewPortStatusBarIconWifi},
    {"RPC Session Icon", ViewPortStatusBarIconRpc},
};

#define LOADER_MENU_STATUS_ICON_COUNT \
    (sizeof(loader_menu_status_icon_toggles) / sizeof(loader_menu_status_icon_toggles[0]))

static const char* const loader_menu_status_bar_bg_text[LOADER_STATUS_BAR_BG_COUNT] = {
    "OFF",
    "ON",
};

static const char* loader_menu_clock_enable_text[LOADER_CLOCK_ENABLE_COUNT] = {
    "OFF",
    "ON",
};

static const uint32_t loader_menu_clock_enable_value[LOADER_CLOCK_ENABLE_COUNT] = {0, 1};

static void loader_menu_battery_view_changed(VariableItem* item) {
    uint32_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, loader_menu_battery_view_text[index]);

    LoaderMenuDesktopSettings* settings = malloc(sizeof(LoaderMenuDesktopSettings));
    desktop_settings_load(settings);
    settings->displayBatteryPercentage = (uint8_t)loader_menu_battery_view_value[index];
    desktop_settings_save(settings);
    free(settings);

    if(furi_record_exists(LOADER_RECORD_POWER)) {
        LoaderMenuPower* power = furi_record_open(LOADER_RECORD_POWER);
        power_trigger_ui_update(power);
        furi_record_close(LOADER_RECORD_POWER);
    }
}

static void loader_menu_clock_enabled_changed(VariableItem* item) {
    uint32_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, loader_menu_clock_enable_text[index]);

    if(furi_record_exists(LOADER_RECORD_DESKTOP)) {
        LoaderMenuDesktop* desktop = furi_record_open(LOADER_RECORD_DESKTOP);
        LoaderMenuDesktopSettings* settings = malloc(sizeof(LoaderMenuDesktopSettings));
        desktop_api_get_settings(desktop, settings);
        settings->display_clock = (uint8_t)loader_menu_clock_enable_value[index];
        desktop_api_set_settings(desktop, settings);
        free(settings);
        furi_record_close(LOADER_RECORD_DESKTOP);
    } else {
        LoaderMenuDesktopSettings* settings = malloc(sizeof(LoaderMenuDesktopSettings));
        desktop_settings_load(settings);
        settings->display_clock = (uint8_t)loader_menu_clock_enable_value[index];
        desktop_settings_save(settings);
        free(settings);
    }
}

static void loader_menu_status_bar_bg_changed(VariableItem* item) {
    uint32_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, loader_menu_status_bar_bg_text[index]);

    if(furi_record_exists(LOADER_RECORD_DESKTOP)) {
        LoaderMenuDesktop* desktop = furi_record_open(LOADER_RECORD_DESKTOP);
        LoaderMenuDesktopSettings* settings = malloc(sizeof(LoaderMenuDesktopSettings));
        desktop_api_get_settings(desktop, settings);
        settings->display_status_bar_background = (uint8_t)index;
        desktop_api_set_settings(desktop, settings);
        free(settings);
        furi_record_close(LOADER_RECORD_DESKTOP);
    } else {
        LoaderMenuDesktopSettings* settings = malloc(sizeof(LoaderMenuDesktopSettings));
        desktop_settings_load(settings);
        settings->display_status_bar_background = (uint8_t)index;
        desktop_settings_save(settings);
        free(settings);
    }
}

static void loader_menu_status_icon_changed(VariableItem* item) {
    size_t index = (size_t)(uintptr_t)variable_item_get_context(item);
    furi_assert(index < LOADER_MENU_STATUS_ICON_COUNT);
    bool visible = variable_item_get_current_value_index(item) != 0;
    variable_item_set_current_value_text(item, visible ? "ON" : "OFF");
    momentum_settings_set_statusbar_icon_visible(loader_menu_status_icon_toggles[index].icon, visible);
    momentum_settings_save();
    bt_statusbar_refresh();

    if(furi_record_exists(LOADER_RECORD_POWER)) {
        LoaderMenuPower* power = furi_record_open(LOADER_RECORD_POWER);
        power_trigger_ui_update(power);
        furi_record_close(LOADER_RECORD_POWER);
    }
}

static void loader_menu_build_interface_status(LoaderMenuApp* app) {
    variable_item_list_reset(app->interface_status_list);
    variable_item_list_set_header(app->interface_status_list, "Status Bar:");

    LoaderMenuDesktopSettings* settings = malloc(sizeof(LoaderMenuDesktopSettings));
    desktop_settings_load(settings);

    VariableItem* item = variable_item_list_add(
        app->interface_status_list,
        "Battery Icon",
        LOADER_BATTERY_VIEW_COUNT,
        loader_menu_battery_view_changed,
        app);
    uint32_t v = 0;
    for(uint32_t i = 0; i < LOADER_BATTERY_VIEW_COUNT; i++) {
        if(loader_menu_battery_view_value[i] == settings->displayBatteryPercentage) {
            v = i;
            break;
        }
    }
    variable_item_set_current_value_index(item, (uint8_t)v);
    variable_item_set_current_value_text(item, loader_menu_battery_view_text[v]);

    item = variable_item_list_add(
        app->interface_status_list,
        "Show Clock",
        LOADER_CLOCK_ENABLE_COUNT,
        loader_menu_clock_enabled_changed,
        app);
    uint32_t c = (settings->display_clock != 0) ? 1 : 0;
    variable_item_set_current_value_index(item, (uint8_t)c);
    variable_item_set_current_value_text(item, loader_menu_clock_enable_text[c]);

    item = variable_item_list_add(
        app->interface_status_list,
        "Bar Background",
        LOADER_STATUS_BAR_BG_COUNT,
        loader_menu_status_bar_bg_changed,
        app);
    uint32_t bg = settings->display_status_bar_background != 0 ? 1 : 0;
    variable_item_set_current_value_index(item, (uint8_t)bg);
    variable_item_set_current_value_text(item, loader_menu_status_bar_bg_text[bg]);

    free(settings);

    for(size_t i = 0; i < LOADER_MENU_STATUS_ICON_COUNT; i++) {
        item = variable_item_list_add(
            app->interface_status_list,
            loader_menu_status_icon_toggles[i].label,
            2,
            loader_menu_status_icon_changed,
            (void*)(uintptr_t)i);
        bool visible =
            momentum_settings_statusbar_icon_visible(loader_menu_status_icon_toggles[i].icon);
        variable_item_set_current_value_index(item, visible ? 1 : 0);
        variable_item_set_current_value_text(item, visible ? "ON" : "OFF");
    }
}

/* ---------------------------------------------------------------- */
/* Interface -> Graphics tab                                        */
/* ---------------------------------------------------------------- */

static const char* const loader_menu_anim_speed_text[] = {
    "25%", "50%", "75%", "100%", "125%", "150%", "175%", "200%", "225%", "250%", "275%", "300%",
};
static const uint32_t loader_menu_anim_speed_value[] = {
    25, 50, 75, 100, 125, 150, 175, 200, 225, 250, 275, 300,
};
#define LOADER_ANIM_SPEED_COUNT \
    (sizeof(loader_menu_anim_speed_value) / sizeof(loader_menu_anim_speed_value[0]))

static const char* const loader_menu_cycle_anims_text[] = {
    "OFF",  "Meta.txt", "15 S",  "30 S", "1 M",  "2 M",  "5 M", "10 M",
    "15 M", "30 M",     "1 H",   "2 H",  "6 H",  "12 H", "24 H",
};
static const int32_t loader_menu_cycle_anims_value[] = {
    -1,  0,    15,   30,   60,    120,   300,   600,
    900, 1800, 3600, 7200, 21600, 43200, 86400,
};
#define LOADER_CYCLE_ANIMS_COUNT \
    (sizeof(loader_menu_cycle_anims_value) / sizeof(loader_menu_cycle_anims_value[0]))

static bool loader_menu_graphics_dirty;

static void loader_menu_scan_asset_packs(LoaderMenuApp* app) {
    for(size_t i = 0; i < app->asset_pack_count; i++) {
        furi_string_free(app->asset_pack_names[i]);
        app->asset_pack_names[i] = NULL;
    }
    app->asset_pack_count = 0;
    app->asset_pack_index = 0;

    char selected[ASSET_PACKS_NAME_LEN];
    momentum_settings_get_asset_pack(selected, sizeof(selected));
    bool selected_found = selected[0] == '\0';

    File* dir = storage_file_alloc(app->storage);
    FileInfo info;
    char* name = malloc(ASSET_PACKS_NAME_LEN);
    furi_check(name);
    bool directory_opened = storage_dir_open(dir, ASSET_PACKS_PATH);
    if(directory_opened) {
        while(storage_dir_read(dir, &info, name, ASSET_PACKS_NAME_LEN) &&
              app->asset_pack_count < LOADER_ASSET_PACKS_MAX) {
            if((info.flags & FSF_DIRECTORY) && name[0] != '.') {
                app->asset_pack_names[app->asset_pack_count] = furi_string_alloc_set_str(name);
                if(selected[0] != '\0' && strcmp(name, selected) == 0) {
                    app->asset_pack_index = app->asset_pack_count + 1;
                    selected_found = true;
                }
                app->asset_pack_count++;
            }
        }
    }
    free(name);
    storage_dir_close(dir);
    storage_file_free(dir);
    if(directory_opened && !selected_found) {
        FURI_LOG_W(TAG, "Asset pack '%s' is unavailable; using Default", selected);
        momentum_settings_set_asset_pack("");
        loader_menu_graphics_dirty = true;
    } else if(!directory_opened && selected[0] != '\0') {
        FURI_LOG_W(TAG, "Could not scan asset packs; retaining '%s'", selected);
    }
}

static void loader_menu_graphics_apply(void) {
    if(!loader_menu_graphics_dirty) return;
    loader_menu_graphics_dirty = false;
    momentum_settings_save();
    if(furi_record_exists(LOADER_RECORD_DESKTOP)) {
        LoaderMenuDesktop* desktop = furi_record_open(LOADER_RECORD_DESKTOP);
        desktop_api_reload_animation(desktop);
        furi_record_close(LOADER_RECORD_DESKTOP);
    }
}

static void loader_menu_asset_pack_changed(VariableItem* item) {
    LoaderMenuApp* app = variable_item_get_context(item);
    uint32_t index = variable_item_get_current_value_index(item);
    furi_assert(index <= app->asset_pack_count);
    const char* text =
        (index == 0) ? "Default" : furi_string_get_cstr(app->asset_pack_names[index - 1]);
    variable_item_set_current_value_text(item, text);
    app->asset_pack_index = index;
    momentum_settings_set_asset_pack(index == 0 ? "" : text);
    loader_menu_graphics_dirty = true;
}

static void loader_menu_anim_speed_changed(VariableItem* item) {
    uint32_t index = variable_item_get_current_value_index(item);
    furi_assert(index < LOADER_ANIM_SPEED_COUNT);
    variable_item_set_current_value_text(item, loader_menu_anim_speed_text[index]);
    momentum_settings_set_anim_speed(loader_menu_anim_speed_value[index]);
    loader_menu_graphics_dirty = true;
}

static void loader_menu_cycle_anims_changed(VariableItem* item) {
    uint32_t index = variable_item_get_current_value_index(item);
    furi_assert(index < LOADER_CYCLE_ANIMS_COUNT);
    variable_item_set_current_value_text(item, loader_menu_cycle_anims_text[index]);
    momentum_settings_set_cycle_anims(loader_menu_cycle_anims_value[index]);
    loader_menu_graphics_dirty = true;
}

static void loader_menu_unlock_anims_changed(VariableItem* item) {
    bool value = variable_item_get_current_value_index(item) != 0;
    variable_item_set_current_value_text(item, value ? "ON" : "OFF");
    momentum_settings_set_bool(MomentumSettingsBoolUnlockAnims, value);
    loader_menu_graphics_dirty = true;
}

static void loader_menu_build_interface_graphics(LoaderMenuApp* app) {
    variable_item_list_reset(app->interface_graphics_list);
    variable_item_list_set_header(app->interface_graphics_list, "Graphics:");
    loader_menu_scan_asset_packs(app);

    VariableItem* item = variable_item_list_add(
        app->interface_graphics_list,
        "Asset Pack",
        (uint8_t)(app->asset_pack_count + 1),
        loader_menu_asset_pack_changed,
        app);
    uint32_t pack_index = app->asset_pack_index;
    if(pack_index > app->asset_pack_count) pack_index = 0;
    variable_item_set_current_value_index(item, (uint8_t)pack_index);
    variable_item_set_current_value_text(
        item,
        pack_index == 0 ? "Default" :
                          furi_string_get_cstr(app->asset_pack_names[pack_index - 1]));

    item = variable_item_list_add(
        app->interface_graphics_list,
        "Anim Speed",
        (uint8_t)LOADER_ANIM_SPEED_COUNT,
        loader_menu_anim_speed_changed,
        app);
    MomentumSettings settings;
    momentum_settings_get_snapshot(&settings);
    const uint32_t current_speed = settings.anim_speed;
    uint32_t speed_index = 3;
    for(uint32_t i = 0; i < LOADER_ANIM_SPEED_COUNT; i++) {
        if(loader_menu_anim_speed_value[i] == current_speed) {
            speed_index = i;
            break;
        }
    }
    variable_item_set_current_value_index(item, (uint8_t)speed_index);
    variable_item_set_current_value_text(item, loader_menu_anim_speed_text[speed_index]);

    item = variable_item_list_add(
        app->interface_graphics_list,
        "Cycle Anims",
        (uint8_t)LOADER_CYCLE_ANIMS_COUNT,
        loader_menu_cycle_anims_changed,
        app);
    const int32_t current_cycle = settings.cycle_anims;
    uint32_t cycle_index = 1;
    for(uint32_t i = 0; i < LOADER_CYCLE_ANIMS_COUNT; i++) {
        if(loader_menu_cycle_anims_value[i] == current_cycle) {
            cycle_index = i;
            break;
        }
    }
    variable_item_set_current_value_index(item, (uint8_t)cycle_index);
    variable_item_set_current_value_text(item, loader_menu_cycle_anims_text[cycle_index]);

    item = variable_item_list_add(
        app->interface_graphics_list, "Unlock Anims", 2, loader_menu_unlock_anims_changed, app);
    variable_item_set_current_value_index(item, settings.unlock_anims ? 1 : 0);
    variable_item_set_current_value_text(item, settings.unlock_anims ? "ON" : "OFF");
}

/* ---------------------------------------------------------------- */
/* Add Item view                                                     */
/* ---------------------------------------------------------------- */

static void loader_menu_add_main_callback(void* context, uint32_t index);
static void loader_menu_add_external_callback(void* context, uint32_t index);
static void loader_menu_add_menu_main_callback(void* context, uint32_t index);

static void loader_menu_build_add_menu(LoaderMenuApp* app) {
    submenu_reset(app->add_menu);
    submenu_set_header(app->add_menu, "Add Menu Item:");
    submenu_add_item(app->add_menu, "Main App", 0, loader_menu_add_menu_main_callback, app);
    submenu_add_item(app->add_menu, "External App", 1, loader_menu_add_external_callback, app);
}

/* "Main App" entry: switch to the built-in apps picker (Add Main view). */
static void loader_menu_add_menu_main_callback(void* context, uint32_t index) {
    UNUSED(index);
    LoaderMenuApp* app = context;
    loader_menu_build_add_main(app);
    loader_menu_switch_to_view(app, LoaderMenuViewAddMain);
}

static bool loader_menu_add_main_skip(const char* appid) {
    static const char* const blocked[] = {
        "example_apps_assets",
        "example_apps_data",
        "example_number_input",
        "about",
        "js_app",
        "passport",
        "bt_settings",
    };
    for(size_t i = 0; i < COUNT_OF(blocked); ++i) {
        if(strcmp(appid, blocked[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* "Main App" tab: every built-in internal app, locked if already in menu. */
static void loader_menu_build_add_main(LoaderMenuApp* app) {
    submenu_reset(app->add_main_menu);
    submenu_set_header(app->add_main_menu, "Main App");

    for(size_t i = 0; i < FLIPPER_APPS_COUNT; ++i) {
        if(loader_menu_add_main_skip(FLIPPER_APPS[i].appid)) {
            continue;
        }
        bool locked = loader_menu_app_is_visible(app, FLIPPER_APPS[i].appid);
        submenu_add_lockable_item(
            app->add_main_menu,
            FLIPPER_APPS[i].name,
            (uint32_t)i,
            loader_menu_add_main_callback,
            app,
            locked,
            "Already in menu");
    }
    for(size_t i = 0; i < FLIPPER_SYSTEM_APPS_COUNT; ++i) {
        if(loader_menu_add_main_skip(FLIPPER_SYSTEM_APPS[i].appid)) {
            continue;
        }
        bool locked = loader_menu_app_is_visible(app, FLIPPER_SYSTEM_APPS[i].appid);
        submenu_add_lockable_item(
            app->add_main_menu,
            FLIPPER_SYSTEM_APPS[i].name,
            (uint32_t)(FLIPPER_APPS_COUNT + i),
            loader_menu_add_main_callback,
            app,
            locked,
            "Already in menu");
    }
}

static void loader_menu_add_main_callback(void* context, uint32_t index) {
    LoaderMenuApp* app = context;
    const char* appid = NULL;
    bool is_default = false;

    if(index < FLIPPER_APPS_COUNT) {
        appid = FLIPPER_APPS[index].appid;
        is_default = true;
    } else {
        size_t sys_idx = index - FLIPPER_APPS_COUNT;
        if(sys_idx < FLIPPER_SYSTEM_APPS_COUNT) {
            appid = FLIPPER_SYSTEM_APPS[sys_idx].appid;
            is_default = loader_menu_is_default_external(appid);
        }
    }
    if(!appid) return;

    bool changed;
    if(is_default) {
        changed = menu_custom_unhide(&app->custom, appid); // bring back a hidden default app
    } else {
        changed = menu_custom_add_internal(&app->custom, appid);
    }
    if(!changed) return;

    menu_custom_save(&app->custom);
    loader_menu_build_menu(app);
    loader_menu_build_add_main(app);
    loader_menu_build_remove(app);
    loader_menu_switch_to_view(app, LoaderMenuViewPrimary);
}

static bool loader_menu_file_browser_item_callback(
    FuriString* path,
    void* context,
    uint8_t** icon,
    FuriString* item_name) {
    LoaderMenuApp* app = context;
    if(furi_string_end_with(path, ".fap")) {
        return flipper_application_load_name_and_icon(path, app->storage, icon, item_name);
    }
    return false;
}

static void loader_menu_add_external_callback(void* context, uint32_t index) {
    UNUSED(index);
    LoaderMenuApp* app = context;

    const DialogsFileBrowserOptions browser_options = {
        .extension = ".fap",
        .skip_assets = true,
        .icon = &I_unknown_10px,
        .hide_ext = true,
        .item_loader_callback = loader_menu_file_browser_item_callback,
        .item_loader_context = app,
        .base_path = EXT_PATH("apps"),
    };

    furi_string_set(app->browser_path, EXT_PATH("apps"));
    if(!dialog_file_browser_show(app->dialogs, app->browser_path, app->browser_path, &browser_options)) {
        return; // cancelled, stay in Add view
    }
    if(!furi_string_end_with(app->browser_path, ".fap")) {
        return;
    }

    if(!menu_custom_add_fap(&app->custom, furi_string_get_cstr(app->browser_path))) {
        return; // already added
    }

    menu_custom_save(&app->custom);
    loader_menu_build_menu(app);
    loader_menu_build_remove(app);
    loader_menu_switch_to_view(app, LoaderMenuViewPrimary);
}

/* ---------------------------------------------------------------- */
/* Remove Item view                                                  */
/* ---------------------------------------------------------------- */

static void loader_menu_remove_callback(void* context, uint32_t index) {
    LoaderMenuApp* app = context;
    furi_assert(index < app->entries_count);
    const LoaderMenuEntry* entry = &app->entries[index];

    bool changed = false;
    switch(entry->kind) {
    case LoaderMenuEntryKindApplications:
        changed = menu_custom_hide(&app->custom, LOADER_APPLICATIONS_NAME);
        break;
    case LoaderMenuEntryKindInternal:
        changed = menu_custom_hide(&app->custom, FLIPPER_APPS[entry->index].appid);
        break;
    case LoaderMenuEntryKindExternal:
        changed = menu_custom_hide(&app->custom, FLIPPER_EXTERNAL_APPS[entry->index].path);
        break;
    case LoaderMenuEntryKindInternalAdded:
        changed = menu_custom_remove_internal(&app->custom, FLIPPER_SYSTEM_APPS[entry->index].appid);
        break;
    case LoaderMenuEntryKindFap:
        changed = menu_custom_remove_fap(&app->custom, furi_string_get_cstr(app->faps[entry->index].path));
        break;
    default:
        break; // Settings is not removable
    }

    if(!changed) return;

    menu_custom_save(&app->custom);
    loader_menu_build_menu(app);
    loader_menu_build_add_main(app);
    loader_menu_build_remove(app); // stay on the Remove view
}

void loader_menu_build_remove(LoaderMenuApp* app) {
    submenu_reset(app->remove_menu);
    submenu_set_header(app->remove_menu, "Remove Item:");
    for(size_t i = 0; i < app->entries_count; ++i) {
        const LoaderMenuEntry* entry = &app->entries[i];
        if(entry->kind == LoaderMenuEntryKindSettings) {
            continue;
        }
        submenu_add_item(
            app->remove_menu,
            loader_menu_entries_label(app, i),
            (uint32_t)i,
            loader_menu_remove_callback,
            app);
    }
}

/* ---------------------------------------------------------------- */
/* Back navigation                                                   */
/* ---------------------------------------------------------------- */

static uint32_t loader_menu_switch_to_primary(void* context) {
    UNUSED(context);
    return LoaderMenuViewPrimary;
}

static uint32_t loader_menu_switch_to_settings(void* context) {
    UNUSED(context);
    return LoaderMenuViewSettings;
}

static uint32_t loader_menu_switch_to_interface(void* context) {
    UNUSED(context);
    return LoaderMenuViewInterface;
}

static uint32_t loader_menu_graphics_previous(void* context) {
    UNUSED(context);
    loader_menu_graphics_apply();
    return LoaderMenuViewInterface;
}

static uint32_t loader_menu_switch_to_add(void* context) {
    UNUSED(context);
    return LoaderMenuViewAdd;
}

static uint32_t loader_menu_exit(void* context) {
    UNUSED(context);
    return VIEW_NONE;
}

/* ---------------------------------------------------------------- */
/* App lifecycle                                                     */
/* ---------------------------------------------------------------- */

static LoaderMenuApp* loader_menu_app_alloc(LoaderMenu* loader_menu) {
    LoaderMenuApp* app = malloc(sizeof(LoaderMenuApp));
    app->asset_pack_count = 0;
    app->asset_pack_index = 0;
    loader_menu_graphics_dirty = false;
    momentum_settings_load();
    app->gui = furi_record_open(RECORD_GUI);
    app->storage = furi_record_open(RECORD_STORAGE);
    app->dialogs = furi_record_open(RECORD_DIALOGS);
    app->view_dispatcher = view_dispatcher_alloc();
    app->primary_menu = menu_alloc();
    app->settings_menu = submenu_alloc();
    app->interface_menu = submenu_alloc();
    app->interface_main_list = variable_item_list_alloc();
    app->interface_lock_list = variable_item_list_alloc();
    app->interface_status_list = variable_item_list_alloc();
    app->interface_graphics_list = variable_item_list_alloc();
    app->add_menu = submenu_alloc();
    app->add_main_menu = submenu_alloc();
    app->remove_menu = submenu_alloc();
    app->browser_path = furi_string_alloc();

    for(size_t i = 0; i < LOADER_MENU_MAX_FAPS; ++i) {
        app->faps[i].path = furi_string_alloc();
        app->faps[i].name = furi_string_alloc();
    }

    menu_custom_load(&app->custom);
    loader_menu_build_menu(app);
    loader_menu_build_submenu(app);
    loader_menu_build_interface(app);
    loader_menu_build_interface_main(app);
    loader_menu_build_interface_lock(app);
    loader_menu_build_interface_status(app);
    loader_menu_build_add_menu(app);
    loader_menu_build_add_main(app);
    loader_menu_build_remove(app);

    // Primary menu
    View* primary_view = menu_get_view(app->primary_menu);
    view_set_context(primary_view, app->primary_menu);
    view_set_previous_callback(primary_view, loader_menu_exit);
    view_dispatcher_add_view(app->view_dispatcher, LoaderMenuViewPrimary, primary_view);

    // Settings menu
    View* settings_view = submenu_get_view(app->settings_menu);
    view_set_context(settings_view, app->settings_menu);
    view_set_previous_callback(settings_view, loader_menu_switch_to_primary);
    view_dispatcher_add_view(app->view_dispatcher, LoaderMenuViewSettings, settings_view);

    // Interface menu and its settings views
    View* interface_view = submenu_get_view(app->interface_menu);
    view_set_context(interface_view, app->interface_menu);
    view_set_previous_callback(interface_view, loader_menu_switch_to_settings);
    view_dispatcher_add_view(app->view_dispatcher, LoaderMenuViewInterface, interface_view);

    View* interface_main_view = variable_item_list_get_view(app->interface_main_list);
    view_set_context(interface_main_view, app->interface_main_list);
    view_set_previous_callback(interface_main_view, loader_menu_switch_to_interface);
    view_dispatcher_add_view(
        app->view_dispatcher, LoaderMenuViewInterfaceMain, interface_main_view);

    View* interface_lock_view = variable_item_list_get_view(app->interface_lock_list);
    view_set_context(interface_lock_view, app->interface_lock_list);
    view_set_previous_callback(interface_lock_view, loader_menu_switch_to_interface);
    view_dispatcher_add_view(
        app->view_dispatcher, LoaderMenuViewInterfaceLock, interface_lock_view);

    View* interface_status_view = variable_item_list_get_view(app->interface_status_list);
    view_set_context(interface_status_view, app->interface_status_list);
    view_set_previous_callback(interface_status_view, loader_menu_switch_to_interface);
    view_dispatcher_add_view(
        app->view_dispatcher, LoaderMenuViewInterfaceStatus, interface_status_view);

    View* interface_graphics_view = variable_item_list_get_view(app->interface_graphics_list);
    view_set_context(interface_graphics_view, app->interface_graphics_list);
    view_set_previous_callback(interface_graphics_view, loader_menu_graphics_previous);
    view_dispatcher_add_view(
        app->view_dispatcher, LoaderMenuViewInterfaceGraphics, interface_graphics_view);

    // Add menu
    View* add_view = submenu_get_view(app->add_menu);
    view_set_context(add_view, app->add_menu);
    view_set_previous_callback(add_view, loader_menu_switch_to_interface);
    view_dispatcher_add_view(app->view_dispatcher, LoaderMenuViewAdd, add_view);

    // Add Main App menu
    View* add_main_view = submenu_get_view(app->add_main_menu);
    view_set_context(add_main_view, app->add_main_menu);
    view_set_previous_callback(add_main_view, loader_menu_switch_to_add);
    view_dispatcher_add_view(app->view_dispatcher, LoaderMenuViewAddMain, add_main_view);

    // Remove menu
    View* remove_view = submenu_get_view(app->remove_menu);
    view_set_context(remove_view, app->remove_menu);
    view_set_previous_callback(remove_view, loader_menu_switch_to_interface);
    view_dispatcher_add_view(app->view_dispatcher, LoaderMenuViewRemove, remove_view);

    view_dispatcher_switch_to_view(
        app->view_dispatcher,
        loader_menu->settings_first ? LoaderMenuViewSettings : LoaderMenuViewPrimary);

    return app;
}

static void loader_menu_app_free(LoaderMenuApp* app) {
    loader_menu_graphics_apply();

    view_dispatcher_remove_view(app->view_dispatcher, LoaderMenuViewPrimary);
    view_dispatcher_remove_view(app->view_dispatcher, LoaderMenuViewSettings);
    view_dispatcher_remove_view(app->view_dispatcher, LoaderMenuViewInterface);
    view_dispatcher_remove_view(app->view_dispatcher, LoaderMenuViewInterfaceMain);
    view_dispatcher_remove_view(app->view_dispatcher, LoaderMenuViewInterfaceLock);
    view_dispatcher_remove_view(app->view_dispatcher, LoaderMenuViewInterfaceStatus);
    view_dispatcher_remove_view(app->view_dispatcher, LoaderMenuViewInterfaceGraphics);
    view_dispatcher_remove_view(app->view_dispatcher, LoaderMenuViewAdd);
    view_dispatcher_remove_view(app->view_dispatcher, LoaderMenuViewAddMain);
    view_dispatcher_remove_view(app->view_dispatcher, LoaderMenuViewRemove);
    view_dispatcher_free(app->view_dispatcher);

    menu_free(app->primary_menu);
    submenu_free(app->settings_menu);
    submenu_free(app->interface_menu);
    variable_item_list_free(app->interface_main_list);
    variable_item_list_free(app->interface_lock_list);
    variable_item_list_free(app->interface_status_list);
    variable_item_list_free(app->interface_graphics_list);
    for(size_t i = 0; i < app->asset_pack_count; i++) {
        furi_string_free(app->asset_pack_names[i]);
    }
    submenu_free(app->add_menu);
    submenu_free(app->add_main_menu);
    submenu_free(app->remove_menu);

    menu_custom_free(&app->custom);
    for(size_t i = 0; i < LOADER_MENU_MAX_FAPS; ++i) {
        furi_string_free(app->faps[i].path);
        furi_string_free(app->faps[i].name);
    }
    furi_string_free(app->browser_path);

    furi_record_close(RECORD_DIALOGS);
    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_GUI);
    free(app);
}

static int32_t loader_menu_thread(void* p) {
    LoaderMenu* loader_menu = p;
    furi_assert(loader_menu);
    loader_menu_trace("thread_start");

    if(furi_record_exists(LOADER_RECORD_DESKTOP)) {
        LoaderMenuDesktop* desktop = furi_record_open(LOADER_RECORD_DESKTOP);
        desktop_api_set_loader_menu_active(desktop, true);
        furi_record_close(LOADER_RECORD_DESKTOP);
    }

    LoaderMenuApp* app = loader_menu_app_alloc(loader_menu);
    loader_menu_trace("app_alloc");

    view_dispatcher_attach_to_gui(
        app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    loader_menu_trace("attached");
    view_dispatcher_run(app->view_dispatcher);
    loader_menu_trace("dispatcher_return");

    if(furi_record_exists(LOADER_RECORD_DESKTOP)) {
        LoaderMenuDesktop* desktop = furi_record_open(LOADER_RECORD_DESKTOP);
        desktop_api_set_loader_menu_active(desktop, false);
        furi_record_close(LOADER_RECORD_DESKTOP);
    }

    if(loader_menu->closed_cb) {
        loader_menu->closed_cb(loader_menu->context);
    }

    loader_menu_app_free(app);
    loader_menu_trace("thread_end");

    return 0;
}
