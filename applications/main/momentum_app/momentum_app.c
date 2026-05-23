#include "momentum_app.h"
#include <momentum/momentum_settings.h>
#include <momentum/asset_packs.h>
#include <loader/loader.h>

#define TAG "MomentumApp"

static bool momentum_app_custom_event_callback(void* context, uint32_t event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

static bool momentum_app_back_event_callback(void* context) {
    furi_assert(context);
    MomentumApp* app = context;
    return scene_manager_handle_back_event(app->scene_manager);
}

static void momentum_app_tick_event_callback(void* context) {
    furi_assert(context);
    MomentumApp* app = context;
    scene_manager_handle_tick_event(app->scene_manager);
}

MomentumApp* momentum_app_alloc(void) {
    MomentumApp* app = malloc(sizeof(MomentumApp));

    app->gui = furi_record_open(RECORD_GUI);

    app->view_dispatcher = view_dispatcher_alloc();
    app->scene_manager = scene_manager_alloc(&momentum_scene_handlers, app);

    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(
        app->view_dispatcher, momentum_app_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, momentum_app_back_event_callback);
    view_dispatcher_set_tick_event_callback(
        app->view_dispatcher, momentum_app_tick_event_callback, 1000);

    app->var_item_list = variable_item_list_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher,
        MomentumAppViewVarItemList,
        variable_item_list_get_view(app->var_item_list));

    app->submenu = submenu_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher,
        MomentumAppViewSubmenu,
        submenu_get_view(app->submenu));

    app->text_input = text_input_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher,
        MomentumAppViewTextInput,
        text_input_get_view(app->text_input));

    app->popup = popup_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher,
        MomentumAppViewPopup,
        popup_get_view(app->popup));

    app->dialog = dialog_ex_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher,
        MomentumAppViewDialogEx,
        dialog_ex_get_view(app->dialog));

    app->file_path = furi_string_alloc();

    scene_manager_next_scene(app->scene_manager, MomentumSceneStart);

    return app;
}

void momentum_app_free(MomentumApp* app) {
    furi_assert(app);

    view_dispatcher_remove_view(app->view_dispatcher, MomentumAppViewVarItemList);
    variable_item_list_free(app->var_item_list);

    view_dispatcher_remove_view(app->view_dispatcher, MomentumAppViewSubmenu);
    submenu_free(app->submenu);

    view_dispatcher_remove_view(app->view_dispatcher, MomentumAppViewTextInput);
    text_input_free(app->text_input);

    view_dispatcher_remove_view(app->view_dispatcher, MomentumAppViewPopup);
    popup_free(app->popup);

    view_dispatcher_remove_view(app->view_dispatcher, MomentumAppViewDialogEx);
    dialog_ex_free(app->dialog);

    furi_string_free(app->file_path);

    scene_manager_free(app->scene_manager);
    view_dispatcher_free(app->view_dispatcher);

    furi_record_close(RECORD_GUI);

    free(app);
}

int32_t momentum_app(void* p) {
    UNUSED(p);

    MomentumApp* app = momentum_app_alloc();

    view_dispatcher_attach_to_gui(
        app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    view_dispatcher_run(app->view_dispatcher);

    momentum_app_free(app);

    return 0;
}
