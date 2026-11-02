#include <furi.h>
#include <gui.h>
#include <view_dispatcher.h>
#include <variable_item_list.h>
#include <menu.h>
#include <lib/toolbox/value_index.h>
#include <desktop/desktop.h>
#include <desktop/desktop_settings.h>
#include <power/power_service/power.h>

#define INTERFACE_SETTINGS_VIEW_LIST (0)

#define BATTERY_VIEW_COUNT 5
#define CLOCK_ENABLE_COUNT 2
#define STATUS_BAR_BACKGROUND_COUNT 2

typedef struct {
    ViewDispatcher* view_dispatcher;
    VariableItemList* variable_item_list;
} InterfaceSettingsApp;

static const char* const battery_view_count_text[BATTERY_VIEW_COUNT] =
    {"OFF", "Bar", "%", "Inv. %", "Bar %"};

static const uint32_t battery_view_value[BATTERY_VIEW_COUNT] = {
    DISPLAY_BATTERY_OFF,
    DISPLAY_BATTERY_BAR,
    DISPLAY_BATTERY_PERCENT,
    DISPLAY_BATTERY_INVERTED_PERCENT,
    DISPLAY_BATTERY_BAR_PERCENT};

static const char* const clock_enable_text[CLOCK_ENABLE_COUNT] = {"OFF", "ON"};
static const uint32_t clock_enable_value[CLOCK_ENABLE_COUNT] = {0, 1};

static const char* const status_bar_background_text[STATUS_BAR_BACKGROUND_COUNT] = {"OFF", "ON"};

static void battery_view_changed(VariableItem* item) {
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, battery_view_count_text[index]);

    DesktopSettings* settings = malloc(sizeof(DesktopSettings));
    desktop_settings_load(settings);
    settings->displayBatteryPercentage = (uint8_t)battery_view_value[index];
    desktop_settings_save(settings);
    free(settings);

    Power* power = furi_record_open(RECORD_POWER);
    power_trigger_ui_update(power);
    furi_record_close(RECORD_POWER);
}

static void clock_enable_changed(VariableItem* item) {
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, clock_enable_text[index]);

    Desktop* desktop = furi_record_open(RECORD_DESKTOP);
    DesktopSettings* settings = malloc(sizeof(DesktopSettings));
    desktop_api_get_settings(desktop, settings);
    settings->display_clock = (uint8_t)clock_enable_value[index];
    desktop_api_set_settings(desktop, settings);
    free(settings);
    furi_record_close(RECORD_DESKTOP);
}

static void status_bar_background_changed(VariableItem* item) {
    const uint8_t value = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, status_bar_background_text[value]);

    Desktop* desktop = furi_record_open(RECORD_DESKTOP);
    DesktopSettings* settings = malloc(sizeof(DesktopSettings));
    desktop_api_get_settings(desktop, settings);
    settings->display_status_bar_background = value;
    desktop_api_set_settings(desktop, settings);
    free(settings);
    furi_record_close(RECORD_DESKTOP);
}

static const char* const menu_style_text[MenuStyleCount] = {
    "List",
    "DSi",
    "Wii",
};

static void menu_style_changed(VariableItem* item) {
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, menu_style_text[index]);
    menu_set_style((MenuStyle)index);
}

static const char* const lock_screen_style_text[LockScreenStyleCount] = {
    "Default",
    "Momentum",
};

static void lock_screen_style_changed(VariableItem* item) {
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, lock_screen_style_text[index]);
    lock_screen_set_style((LockScreenStyle)index);
}

static uint32_t interface_settings_exit(void* context) {
    UNUSED(context);
    return VIEW_NONE;
}

static InterfaceSettingsApp* interface_settings_alloc(void) {
    InterfaceSettingsApp* app = malloc(sizeof(InterfaceSettingsApp));

    app->variable_item_list = variable_item_list_alloc();
    View* view = variable_item_list_get_view(app->variable_item_list);
    view_set_previous_callback(view, interface_settings_exit);

    VariableItem* item = variable_item_list_add(
        app->variable_item_list, "Menu style", MenuStyleCount, menu_style_changed, app);
    uint8_t value_index = (uint8_t)menu_get_style();
    if(value_index >= MenuStyleCount) {
        value_index = 0;
    }
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, menu_style_text[value_index]);

    item = variable_item_list_add(
        app->variable_item_list,
        "Lock screen",
        LockScreenStyleCount,
        lock_screen_style_changed,
        app);
    value_index = (uint8_t)lock_screen_get_style();
    if(value_index >= LockScreenStyleCount) {
        value_index = 0;
    }
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, lock_screen_style_text[value_index]);

    DesktopSettings* settings = malloc(sizeof(DesktopSettings));
    desktop_settings_load(settings);

    // Battery Icon: OFF / Bar / % / Inv. % / Bar %
    item = variable_item_list_add(
        app->variable_item_list, "Battery Icon", BATTERY_VIEW_COUNT, battery_view_changed, app);
    value_index = value_index_uint32(
        settings->displayBatteryPercentage, battery_view_value, BATTERY_VIEW_COUNT);
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, battery_view_count_text[value_index]);

    // Show Clock: OFF / ON
    item = variable_item_list_add(
        app->variable_item_list, "Show Clock", CLOCK_ENABLE_COUNT, clock_enable_changed, app);
    value_index =
        value_index_uint32(settings->display_clock, clock_enable_value, CLOCK_ENABLE_COUNT);
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, clock_enable_text[value_index]);

    item = variable_item_list_add(
        app->variable_item_list,
        "Bar Background",
        STATUS_BAR_BACKGROUND_COUNT,
        status_bar_background_changed,
        app);
    value_index = settings->display_status_bar_background ? 1 : 0;
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, status_bar_background_text[value_index]);
    free(settings);

    Gui* gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_attach_to_gui(app->view_dispatcher, gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_add_view(app->view_dispatcher, INTERFACE_SETTINGS_VIEW_LIST, view);
    view_dispatcher_switch_to_view(app->view_dispatcher, INTERFACE_SETTINGS_VIEW_LIST);
    return app;
}

static void interface_settings_free(InterfaceSettingsApp* app) {
    view_dispatcher_remove_view(app->view_dispatcher, INTERFACE_SETTINGS_VIEW_LIST);
    variable_item_list_free(app->variable_item_list);
    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);
    free(app);
}

int32_t interface_settings_app(void* p) {
    UNUSED(p);
    InterfaceSettingsApp* app = interface_settings_alloc();
    view_dispatcher_run(app->view_dispatcher);
    interface_settings_free(app);
    return 0;
}