#include <furi.h>
#include <furi_hal_light.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/variable_item_list.h>

#define LEVEL_COUNT 4

static const char* const level_names[LEVEL_COUNT] = {"30%", "50%", "70%", "100%"};
static const uint8_t level_values[LEVEL_COUNT] = {77, 128, 179, 255};

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    VariableItemList* vil;
} BacklightSettingsApp;

static void backlight_changed(VariableItem* item) {
    uint8_t idx = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, level_names[idx]);
    furi_hal_light_set(LightBacklight, level_values[idx]);
}

static uint32_t backlight_exit(void* context) {
    UNUSED(context);
    return VIEW_NONE;
}

int32_t backlight_settings_app(void* p) {
    UNUSED(p);

    BacklightSettingsApp* app = malloc(sizeof(BacklightSettingsApp));
    app->gui = furi_record_open(RECORD_GUI);

    app->vil = variable_item_list_alloc();
    VariableItem* item = variable_item_list_add(
        app->vil, "Backlight", LEVEL_COUNT, backlight_changed, app);

    // Find current brightness level (default to max)
    uint8_t current_idx = LEVEL_COUNT - 1;
    // Just default to "On" — no persistent storage for now
    variable_item_set_current_value_index(item, current_idx);
    variable_item_set_current_value_text(item, level_names[current_idx]);

    View* view = variable_item_list_get_view(app->vil);
    view_set_previous_callback(view, backlight_exit);

    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_add_view(app->view_dispatcher, 0, view);
    view_dispatcher_switch_to_view(app->view_dispatcher, 0);
    view_dispatcher_run(app->view_dispatcher);

    view_dispatcher_remove_view(app->view_dispatcher, 0);
    variable_item_list_free(app->vil);
    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);
    free(app);
    return 0;
}
