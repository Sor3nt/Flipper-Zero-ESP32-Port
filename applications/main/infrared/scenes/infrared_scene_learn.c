#include "../infrared_app_i.h"
#include <core/core_defines.h>
#include <dolphin/dolphin.h>

const char* const easy_mode_button_names[] = {"Power", "Vol_up", "Vol_dn", "Mute",    "Ch_up",
                                              "Ch_dn", "Ok",     "Up",     "Down",    "Left",
                                              "Right", "Menu",   "Back",   "Play",    "Pause",
                                              "Stop",  "Next",   "Prev",   "FF",      "Rew",
                                              "Input", "Exit",   "Eject",  "Subtitle"};
const size_t easy_mode_button_count = COUNT_OF(easy_mode_button_names);

static void infrared_scene_learn_dialog_result_callback(DialogExResult result, void* context) {
    InfraredApp* infrared = context;
    view_dispatcher_send_custom_event(infrared->view_dispatcher, result);
}

static bool infrared_scene_learn_get_next_name(
    InfraredApp* infrared,
    int32_t start_index,
    int32_t* next_index) {
    if(!infrared->remote) return false;

    FuriString* name = furi_string_alloc();
    for(int32_t i = start_index; i < (int32_t)easy_mode_button_count; i++) {
        furi_string_set(name, easy_mode_button_names[i]);
        bool name_exists = false;

        for(size_t j = 0; j < infrared_remote_get_signal_count(infrared->remote); j++) {
            if(furi_string_cmpi(name, infrared_remote_get_signal_name(infrared->remote, j)) == 0) {
                name_exists = true;
                break;
            }
        }

        if(!name_exists) {
            *next_index = i;
            furi_string_free(name);
            return true;
        }
    }
    furi_string_free(name);

    return false;
}

static void infrared_scene_learn_update_button_name(InfraredApp* infrared, bool increment) {
    DialogEx* dialog_ex = infrared->dialog_ex;
    int32_t button_index;

    if(infrared->app_state.is_learning_new_remote) {
        button_index = infrared->app_state.current_button_index;
        if(increment) {
            if(button_index + 1 < (int32_t)easy_mode_button_count) {
                button_index++;
                infrared->app_state.current_button_index = button_index;
            }
        }
    } else if(infrared->remote) {
        button_index = infrared->app_state.existing_remote_button_index;
        if(increment) {
            int32_t next_index;
            if(infrared_scene_learn_get_next_name(infrared, button_index + 1, &next_index)) {
                button_index = next_index;
                infrared->app_state.existing_remote_button_index = button_index;
            }
        }
    } else {
        button_index = 0;
    }

    if(button_index < 0) button_index = 0;
    if(button_index >= (int32_t)easy_mode_button_count) {
        button_index = (int32_t)easy_mode_button_count - 1;
    }

    const char* button_name = easy_mode_button_names[button_index];
    dialog_ex_set_text(
        dialog_ex, "Point the remote at IR port\nand press button:", 5, 10, AlignLeft, AlignCenter);
    dialog_ex_set_header(dialog_ex, button_name, 78, 11, AlignLeft, AlignTop);

    bool has_more_buttons = false;
    if(!infrared->app_state.is_learning_new_remote && infrared->remote) {
        int32_t next_index;
        has_more_buttons =
            infrared_scene_learn_get_next_name(infrared, button_index + 1, &next_index);
    } else {
        has_more_buttons = (button_index + 1 < (int32_t)easy_mode_button_count);
    }

    if(!has_more_buttons) {
        dialog_ex_set_center_button_text(dialog_ex, NULL);
    } else {
        dialog_ex_set_center_button_text(dialog_ex, "Skip");
    }
}

void infrared_scene_learn_on_enter(void* context) {
    InfraredApp* infrared = context;
    DialogEx* dialog_ex = infrared->dialog_ex;
    InfraredWorker* worker = infrared->worker;

    if(infrared->app_state.is_learning_new_remote) {
        if(infrared->app_state.current_button_index < 0 ||
           infrared->app_state.current_button_index >= (int32_t)easy_mode_button_count) {
            infrared->app_state.current_button_index = 0;
        }
    } else {
        int32_t next_index;
        if(infrared_scene_learn_get_next_name(infrared, 0, &next_index)) {
            infrared->app_state.existing_remote_button_index = next_index;
        } else {
            infrared->app_state.existing_remote_button_index = 0;
        }
    }

    infrared_worker_rx_enable_signal_decoding(worker, infrared->app_state.is_decode_enabled);
    infrared_worker_rx_force_signal_decoding(worker, infrared->app_state.is_decode_forced);

    infrared_worker_rx_set_received_signal_callback(
        worker, infrared_signal_received_callback, context);
    infrared_worker_rx_start(worker);
    infrared_play_notification_message(infrared, InfraredNotificationMessageBlinkStartRead);

    dialog_ex_set_icon(dialog_ex, 0, 22, &I_InfraredLearnShort_128x31);
    dialog_ex_set_header(dialog_ex, NULL, 0, 0, AlignCenter, AlignCenter);

    if(infrared->app_state.is_easy_mode) {
        infrared_scene_learn_update_button_name(infrared, false);
    } else {
        dialog_ex_set_text(
            dialog_ex,
            "Point the remote at IR port\nand press the button",
            5,
            10,
            AlignLeft,
            AlignCenter);
    }

    dialog_ex_set_left_button_text(
        dialog_ex, infrared->app_state.is_easy_mode ? "Easy" : "Manual");
    dialog_ex_set_right_button_text(
        dialog_ex,
        infrared->app_state.is_decode_forced  ? "Decode" :
        infrared->app_state.is_decode_enabled ? "Auto" :
                                                "RAW");

    dialog_ex_set_context(dialog_ex, context);
    dialog_ex_set_result_callback(dialog_ex, infrared_scene_learn_dialog_result_callback);

    view_dispatcher_switch_to_view(infrared->view_dispatcher, InfraredViewDialogEx);
}

bool infrared_scene_learn_on_event(void* context, SceneManagerEvent event) {
    InfraredApp* infrared = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == InfraredCustomEventTypeSignalReceived) {
            infrared_play_notification_message(infrared, InfraredNotificationMessageSuccess);
            scene_manager_next_scene(infrared->scene_manager, InfraredSceneLearnSuccess);
            dolphin_deed(DolphinDeedIrLearnSuccess);
            consumed = true;
        } else if(event.event == DialogExResultCenter && infrared->app_state.is_easy_mode) {
            infrared_scene_learn_update_button_name(infrared, true);
            consumed = true;
        } else if(event.event == DialogExResultLeft) {
            infrared->app_state.is_easy_mode = !infrared->app_state.is_easy_mode;
            infrared_save_settings(infrared);
            scene_manager_previous_scene(infrared->scene_manager);
            scene_manager_next_scene(infrared->scene_manager, InfraredSceneLearn);
            consumed = true;
        } else if(event.event == DialogExResultRight) {
            if(infrared->app_state.is_decode_forced) {
                infrared->app_state.is_decode_enabled = false;
                infrared->app_state.is_decode_forced = false;
            } else if(infrared->app_state.is_decode_enabled) {
                infrared->app_state.is_decode_forced = true;
            } else {
                infrared->app_state.is_decode_enabled = true;
            }
            infrared_worker_rx_enable_signal_decoding(
                infrared->worker, infrared->app_state.is_decode_enabled);
            infrared_worker_rx_force_signal_decoding(
                infrared->worker, infrared->app_state.is_decode_forced);
            dialog_ex_set_right_button_text(
                infrared->dialog_ex,
                infrared->app_state.is_decode_forced  ? "Decode" :
                infrared->app_state.is_decode_enabled ? "Auto" :
                                                        "RAW");
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        if(infrared->app_state.is_learning_new_remote) {
            infrared->app_state.current_button_index = 0;
        } else {
            infrared->app_state.existing_remote_button_index = 0;
        }
        consumed = false;
    }

    return consumed;
}

void infrared_scene_learn_on_exit(void* context) {
    InfraredApp* infrared = context;
    DialogEx* dialog_ex = infrared->dialog_ex;
    infrared_worker_rx_set_received_signal_callback(infrared->worker, NULL, NULL);
    infrared_worker_rx_stop(infrared->worker);
    infrared_play_notification_message(infrared, InfraredNotificationMessageBlinkStop);
    dialog_ex_reset(dialog_ex);
}
