#include "../nfc_app_i.h"
#include "../chameleon/chameleon.h"

/* "Place blank Card" — second step of the Clone flow. Reached automatically
 * from NfcSceneReadSuccess once a writable card has been read in clone_mode.
 *
 * Left button  -> Write  (NfcSceneWrite)            transfer onto a blank card
 * Right button -> Wipe   (NfcSceneChameleonWipe)    reset a card to blank
 * Center button-> Chameleon: connect/disconnect the external ChameleonUltra.
 *   When connected, both Write and Wipe automatically use the Chameleon
 *   backend (those scenes pick it up via chameleon_is_connected()).
 *
 * Write/Wipe are entered via scene_manager_next_scene(), so they return here
 * on completion — the user can clone several blanks in a row. Back leaves the
 * Clone flow straight to the NFC main menu (skipping the ReadSuccess scene,
 * which would otherwise immediately re-route back here). */

typedef struct {
    FuriThread* thread;
    volatile bool abort;
    bool running;
} NfcSceneCloneConnectWorker;

static NfcSceneCloneConnectWorker s_conn;

static int32_t nfc_scene_clone_place_blank_connect_worker(void* context) {
    NfcApp* instance = context;
    if(chameleon_connect(&s_conn.abort)) {
        view_dispatcher_send_custom_event(
            instance->view_dispatcher, NfcCustomEventChameleonConnected);
    } else {
        view_dispatcher_send_custom_event(
            instance->view_dispatcher, NfcCustomEventChameleonFailed);
    }
    return 0;
}

static void nfc_scene_clone_place_blank_connect_worker_start(NfcApp* instance) {
    if(s_conn.running) return;
    s_conn.abort = false;
    s_conn.thread = furi_thread_alloc_ex(
        "ChamConn", 8 * 1024, nfc_scene_clone_place_blank_connect_worker, instance);
    furi_thread_start(s_conn.thread);
    s_conn.running = true;
}

static void nfc_scene_clone_place_blank_connect_worker_stop(void) {
    if(!s_conn.running) return;
    s_conn.abort = true;
    furi_thread_join(s_conn.thread);
    furi_thread_free(s_conn.thread);
    s_conn.thread = NULL;
    s_conn.running = false;
}

static void nfc_scene_clone_place_blank_button_callback(
    GuiButtonType result,
    InputType type,
    void* context) {
    NfcApp* instance = context;
    if(type != InputTypeShort) return;

    if(result == GuiButtonTypeCenter) {
        view_dispatcher_send_custom_event(
            instance->view_dispatcher, NfcCustomEventChameleonButton);
    } else if(result == GuiButtonTypeLeft || result == GuiButtonTypeRight) {
        view_dispatcher_send_custom_event(instance->view_dispatcher, result);
    }
}

static void nfc_scene_clone_place_blank_build_view(NfcApp* instance) {
    bool connected = chameleon_is_connected();
    widget_reset(instance->widget);
    widget_add_icon_element(
        instance->widget,
        0,
        1,
        connected ? &I_NFC_manual_chameleon_60x50 : &I_NFC_manual_60x50);
    widget_add_string_multiline_element(
        instance->widget,
        94,
        8,
        AlignCenter,
        AlignTop,
        FontSecondary,
        connected ? "Place blank\nCard at\nChameleon" : "Place blank\nCard");
    widget_add_button_element(
        instance->widget,
        GuiButtonTypeLeft,
        "Write",
        nfc_scene_clone_place_blank_button_callback,
        instance);
    widget_add_button_element(
        instance->widget,
        GuiButtonTypeCenter,
        connected ? "Disconnect" : "Chameleon",
        nfc_scene_clone_place_blank_button_callback,
        instance);
    widget_add_button_element(
        instance->widget,
        GuiButtonTypeRight,
        "Wipe",
        nfc_scene_clone_place_blank_button_callback,
        instance);
    view_dispatcher_switch_to_view(instance->view_dispatcher, NfcViewWidget);
}

static void nfc_scene_clone_place_blank_show_message(NfcApp* instance, const char* msg) {
    widget_reset(instance->widget);
    widget_add_string_element(
        instance->widget, 64, 28, AlignCenter, AlignCenter, FontPrimary, msg);
    view_dispatcher_switch_to_view(instance->view_dispatcher, NfcViewWidget);
}

void nfc_scene_clone_place_blank_on_enter(void* context) {
    NfcApp* instance = context;
    nfc_scene_clone_place_blank_build_view(instance);
}

bool nfc_scene_clone_place_blank_on_event(void* context, SceneManagerEvent event) {
    NfcApp* instance = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == GuiButtonTypeLeft) {
            dolphin_deed(DolphinDeedNfcEmulate);
            scene_manager_next_scene(instance->scene_manager, NfcSceneWrite);
            consumed = true;
        } else if(event.event == GuiButtonTypeRight) {
            scene_manager_next_scene(instance->scene_manager, NfcSceneChameleonWipe);
            consumed = true;
        } else if(event.event == NfcCustomEventChameleonButton) {
            if(chameleon_is_connected()) {
                chameleon_disconnect();
                nfc_scene_clone_place_blank_build_view(instance);
            } else {
                nfc_scene_clone_place_blank_show_message(
                    instance, "Connecting\nChameleon...");
                nfc_scene_clone_place_blank_connect_worker_start(instance);
            }
            consumed = true;
        } else if(event.event == NfcCustomEventChameleonConnected) {
            nfc_scene_clone_place_blank_connect_worker_stop();
            nfc_scene_clone_place_blank_build_view(instance);
            consumed = true;
        } else if(event.event == NfcCustomEventChameleonFailed) {
            nfc_scene_clone_place_blank_connect_worker_stop();
            notification_message(instance->notifications, &sequence_error);
            nfc_scene_clone_place_blank_build_view(instance);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        /* Skip the ReadSuccess scene (it auto-routes back here in clone_mode) */
        consumed = scene_manager_search_and_switch_to_previous_scene(
            instance->scene_manager, NfcSceneStart);
    }

    return consumed;
}

void nfc_scene_clone_place_blank_on_exit(void* context) {
    NfcApp* instance = context;
    nfc_scene_clone_place_blank_connect_worker_stop();
    widget_reset(instance->widget);
}
