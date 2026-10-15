#pragma once
#include <stdio.h>
#include <string.h>

/* Kept inline for independently loaded scene plugins; storage belongs to the app. */
static inline void protopirate_actions_widget_reset(void* context, Widget* widget) {
    ProtoPirateApp* app = context;
    app->action_count = 0;
    memset(app->actions, 0, sizeof(app->actions));
    widget_reset(widget);
}

static inline void protopirate_actions_button_callback(
    GuiButtonType button, InputType type, void* context) {
    ProtoPirateApp* app = context;
    if(button == GuiButtonTypeCenter && type == InputTypeShort)
        view_dispatcher_send_custom_event(app->view_dispatcher, ProtoPirateCustomEventOpenActions);
}

static inline void protopirate_widget_add_button_element(
    Widget* widget, GuiButtonType button, const char* text, ButtonCallback callback, void* context) {
    ProtoPirateApp* app = context;
    if(app->action_count >= 3) return;
    ProtoPirateAction* action = &app->actions[app->action_count++];
    snprintf(action->label, sizeof(action->label), "%s", text);
    action->button = button;
    action->callback = callback;
    action->context = context;
    if(app->action_count == 1)
        widget_add_button_element(widget, GuiButtonTypeCenter, "Actions",
            protopirate_actions_button_callback, app);
    // Rotation scrolls text; it must never activate Left/Right actions.
    view_set_input_mode(widget_get_view(widget), ViewInputModeUpDown);
}
