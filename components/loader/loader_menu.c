#include <gui.h>
#include <view_dispatcher.h>
#include <menu.h>
#include <submenu.h>
#include <assets_icons.h>
#include <applications.h>
#include <archive/helpers/archive_favorites.h>
#include <esp_rom_sys.h>

#include "loader.h"
#include "loader_menu.h"

#define TAG "LoaderMenu"

struct LoaderMenu {
    FuriThread* thread;
    void (*closed_cb)(void*);
    void* context;
};

static void loader_menu_trace(const char* step) {
    esp_rom_printf(
        "\r\n[LM] %s free=%u\r\n",
        step,
        (unsigned)furi_thread_get_stack_space(furi_thread_get_current_id()));
}

static void loader_menu_trace_settings_registry(void) {
    esp_rom_printf(
        "\r\n[LM] settings_counts internal=%u external=%u\r\n",
        (unsigned)FLIPPER_SETTINGS_APPS_COUNT,
        (unsigned)FLIPPER_EXTSETTINGS_APPS_COUNT);

    if(FLIPPER_SETTINGS_APPS_COUNT == 0) {
        esp_rom_printf("\r\n[LM] settings_internal none\r\n");
    } else {
        for(size_t i = 0; i < FLIPPER_SETTINGS_APPS_COUNT; ++i) {
            esp_rom_printf(
                "\r\n[LM] settings_internal[%u] appid=%s name=%s\r\n",
                (unsigned)i,
                FLIPPER_SETTINGS_APPS[i].appid ? FLIPPER_SETTINGS_APPS[i].appid : "(null)",
                FLIPPER_SETTINGS_APPS[i].name ? FLIPPER_SETTINGS_APPS[i].name : "(null)");
        }
    }

    if(FLIPPER_EXTSETTINGS_APPS_COUNT == 0) {
        esp_rom_printf("\r\n[LM] settings_external none\r\n");
    } else {
        for(size_t i = 0; i < FLIPPER_EXTSETTINGS_APPS_COUNT; ++i) {
            esp_rom_printf(
                "\r\n[LM] settings_external[%u] launch=%s name=%s\r\n",
                (unsigned)i,
                FLIPPER_EXTSETTINGS_APPS[i].path ? FLIPPER_EXTSETTINGS_APPS[i].path : "(null)",
                FLIPPER_EXTSETTINGS_APPS[i].name ? FLIPPER_EXTSETTINGS_APPS[i].name : "(null)");
        }
    }
}

static int32_t loader_menu_thread(void* p);

LoaderMenu* loader_menu_alloc(void (*closed_cb)(void*), void* context) {
    LoaderMenu* loader_menu = malloc(sizeof(LoaderMenu));
    loader_menu->closed_cb = closed_cb;
    loader_menu->context = context;
    loader_menu->thread =
        furi_thread_alloc_ex(TAG, 4096, loader_menu_thread, loader_menu);
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
} LoaderMenuView;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    Menu* primary_menu;
    Submenu* settings_menu;
} LoaderMenuApp;

static void loader_menu_start(const char* name) {
    Loader* loader = furi_record_open(RECORD_LOADER);
    loader_start_with_gui_error(loader, name, NULL);
    furi_record_close(RECORD_LOADER);
}

static void loader_menu_apps_callback(void* context, uint32_t index) {
    UNUSED(context);
    const char* name = FLIPPER_APPS[index].name;
    loader_menu_start(name);
}

static void loader_menu_external_apps_callback(void* context, uint32_t index) {
    UNUSED(context);
    const char* launch = FLIPPER_EXTERNAL_APPS[index].path;
    loader_menu_start(launch);
}

static void loader_menu_applications_callback(void* context, uint32_t index) {
    UNUSED(index);
    UNUSED(context);
    loader_menu_trace("apps_callback");
    loader_menu_start(LOADER_APPLICATIONS_NAME);
}

static void
    loader_menu_settings_menu_callback(void* context, InputType input_type, uint32_t index) {
    UNUSED(context);
    if(input_type == InputTypeShort) {
        loader_menu_start((const char*)index);
    } else if(input_type == InputTypeLong) {
        archive_favorites_handle_setting_pin_unpin((const char*)index, NULL);
    }
}

static void loader_menu_switch_to_settings(void* context, uint32_t index) {
    UNUSED(index);
    LoaderMenuApp* app = context;
    view_dispatcher_switch_to_view(app->view_dispatcher, LoaderMenuViewSettings);
}

static uint32_t loader_menu_switch_to_primary(void* context) {
    UNUSED(context);
    return LoaderMenuViewPrimary;
}

static uint32_t loader_menu_exit(void* context) {
    UNUSED(context);
    return VIEW_NONE;
}

static void loader_menu_build_menu(LoaderMenuApp* app, LoaderMenu* menu) {
    size_t i = 0;

    loader_menu_trace_settings_registry();

    menu_add_item(
        app->primary_menu,
        LOADER_APPLICATIONS_NAME,
        &A_Plugins_14,
        i++,
        loader_menu_applications_callback,
        (void*)menu);

    for(i = 0; i < FLIPPER_APPS_COUNT; i++) {
        menu_add_item(
            app->primary_menu,
            FLIPPER_APPS[i].name,
            FLIPPER_APPS[i].icon,
            i,
            loader_menu_apps_callback,
            (void*)menu);
    }

    for(i = 0; i < FLIPPER_EXTERNAL_APPS_COUNT; i++) {
        menu_add_item(
            app->primary_menu,
            FLIPPER_EXTERNAL_APPS[i].name,
            FLIPPER_EXTERNAL_APPS[i].icon,
            i,
            loader_menu_external_apps_callback,
            (void*)menu);
    }

    if((FLIPPER_EXTSETTINGS_APPS_COUNT > 0) || (FLIPPER_SETTINGS_APPS_COUNT > 0)) {
        menu_add_item(
            app->primary_menu, "Settings", &A_Settings_14, i++, loader_menu_switch_to_settings, app);
    }
}

static void loader_menu_build_submenu(LoaderMenuApp* app, LoaderMenu* loader_menu) {
    for(size_t i = 0; i < FLIPPER_EXTSETTINGS_APPS_COUNT; i++) {
        submenu_add_item_ex(
            app->settings_menu,
            FLIPPER_EXTSETTINGS_APPS[i].name,
            (uint32_t)FLIPPER_EXTSETTINGS_APPS[i].path,
            loader_menu_settings_menu_callback,
            loader_menu);
    }
    for(size_t i = 0; i < FLIPPER_SETTINGS_APPS_COUNT; i++) {
        submenu_add_item_ex(
            app->settings_menu,
            FLIPPER_SETTINGS_APPS[i].name,
            (uint32_t)FLIPPER_SETTINGS_APPS[i].name,
            loader_menu_settings_menu_callback,
            loader_menu);
    }
}

static LoaderMenuApp* loader_menu_app_alloc(LoaderMenu* loader_menu) {
    LoaderMenuApp* app = malloc(sizeof(LoaderMenuApp));
    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    app->primary_menu = menu_alloc();
    app->settings_menu = submenu_alloc();

    loader_menu_build_menu(app, loader_menu);
    loader_menu_build_submenu(app, loader_menu);

    // Primary menu
    View* primary_view = menu_get_view(app->primary_menu);
    view_set_context(primary_view, app->primary_menu);
    view_set_previous_callback(primary_view, loader_menu_exit);
    view_dispatcher_add_view(
        app->view_dispatcher, LoaderMenuViewPrimary, primary_view);

    // Settings menu
    View* settings_view = submenu_get_view(app->settings_menu);
    view_set_context(settings_view, app->settings_menu);
    view_set_previous_callback(settings_view, loader_menu_switch_to_primary);
    view_dispatcher_add_view(
        app->view_dispatcher, LoaderMenuViewSettings, settings_view);
    view_dispatcher_switch_to_view(app->view_dispatcher, LoaderMenuViewPrimary);

    return app;
}

static void loader_menu_app_free(LoaderMenuApp* app) {
    view_dispatcher_remove_view(app->view_dispatcher, LoaderMenuViewPrimary);
    view_dispatcher_remove_view(app->view_dispatcher, LoaderMenuViewSettings);
    view_dispatcher_free(app->view_dispatcher);

    menu_free(app->primary_menu);
    submenu_free(app->settings_menu);
    furi_record_close(RECORD_GUI);
    free(app);
}

static int32_t loader_menu_thread(void* p) {
    LoaderMenu* loader_menu = p;
    furi_assert(loader_menu);
    loader_menu_trace("thread_start");

    LoaderMenuApp* app = loader_menu_app_alloc(loader_menu);
    loader_menu_trace("app_alloc");

    view_dispatcher_attach_to_gui(
        app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    loader_menu_trace("attached");
    view_dispatcher_run(app->view_dispatcher);
    loader_menu_trace("dispatcher_return");

    if(loader_menu->closed_cb) {
        loader_menu->closed_cb(loader_menu->context);
    }

    loader_menu_app_free(app);
    loader_menu_trace("thread_end");

    return 0;
}
