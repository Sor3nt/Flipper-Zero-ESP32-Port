#pragma once

#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_input.h>
#include <gui/modules/popup.h>
#include <gui/modules/dialog_ex.h>

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    SceneManager* scene_manager;

    VariableItemList* var_item_list;
    Submenu* submenu;
    TextInput* text_input;
    Popup* popup;
    DialogEx* dialog;

    FuriString* file_path;
} MomentumApp;

typedef enum {
    MomentumAppViewVarItemList,
    MomentumAppViewSubmenu,
    MomentumAppViewTextInput,
    MomentumAppViewPopup,
    MomentumAppViewDialogEx,
} MomentumAppView;
