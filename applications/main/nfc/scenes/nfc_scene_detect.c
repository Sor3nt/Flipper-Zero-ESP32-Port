#include "../nfc_app_i.h"
#include "../chameleon/chameleon.h"
#include "../chameleon/chameleon_nfc.h"
#include <dolphin/dolphin.h>

/* The "Read" screen. Default behaviour is unchanged: nfc_scanner (PN532)
 * detects a card and the scene advances to NfcSceneRead. A bottom-center
 * "Chameleon" button switches the read backend to an external ChameleonUltra
 * over BLE; while connected the button shows "Disconnect" and reading is
 * performed by the Chameleon instead of the PN532. */

typedef struct {
    FuriThread* thread;
    volatile bool abort;
    bool running;
    bool scanner_started;
    NfcApp* app;
} NfcSceneDetectChameleon;

static NfcSceneDetectChameleon s_cham;

static int32_t nfc_scene_detect_chameleon_worker(void* context) {
    NfcApp* instance = context;

    if(!chameleon_is_connected()) {
        if(!chameleon_connect(&s_cham.abort)) {
            view_dispatcher_send_custom_event(
                instance->view_dispatcher, NfcCustomEventChameleonFailed);
            return 0;
        }
        view_dispatcher_send_custom_event(
            instance->view_dispatcher, NfcCustomEventChameleonConnected);
    } else {
        view_dispatcher_send_custom_event(
            instance->view_dispatcher, NfcCustomEventChameleonConnected);
    }

    /* "Hold card" loop: poll the Chameleon until a card is read or aborted */
    while(!s_cham.abort) {
        if(chameleon_nfc_read_card(instance->nfc_device, &s_cham.abort)) {
            view_dispatcher_send_custom_event(
                instance->view_dispatcher, NfcCustomEventChameleonCardRead);
            break;
        }
        furi_delay_ms(300);
    }
    return 0;
}

static void nfc_scene_detect_chameleon_worker_start(NfcApp* instance) {
    if(s_cham.running) return;
    s_cham.app = instance;
    s_cham.abort = false;
    s_cham.thread =
        furi_thread_alloc_ex("ChamRead", 8 * 1024, nfc_scene_detect_chameleon_worker, instance);
    furi_thread_start(s_cham.thread);
    s_cham.running = true;
}

static void nfc_scene_detect_chameleon_worker_stop(void) {
    if(!s_cham.running) return;
    s_cham.abort = true;
    furi_thread_join(s_cham.thread);
    furi_thread_free(s_cham.thread);
    s_cham.thread = NULL;
    s_cham.running = false;
}

static void nfc_scene_detect_button_callback(
    GuiButtonType result,
    InputType type,
    void* context) {
    NfcApp* instance = context;
    if(type == InputTypeShort && result == GuiButtonTypeCenter) {
        view_dispatcher_send_custom_event(
            instance->view_dispatcher, NfcCustomEventChameleonButton);
    }
}

static void nfc_scene_detect_build_view(NfcApp* instance) {
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
        10,
        AlignCenter,
        AlignTop,
        FontSecondary,
        connected ? "Place Card\nat\nChameleon" : "Hold card\nnext to\nDevice");
    widget_add_button_element(
        instance->widget,
        GuiButtonTypeCenter,
        connected ? "Disconnect" : "Chameleon",
        nfc_scene_detect_button_callback,
        instance);
    view_dispatcher_switch_to_view(instance->view_dispatcher, NfcViewWidget);
}

void nfc_scene_detect_scan_callback(NfcScannerEvent event, void* context) {
    furi_assert(context);

    NfcApp* instance = context;

    if(event.type == NfcScannerEventTypeDetected) {
        nfc_detected_protocols_set(
            instance->detected_protocols, event.data.protocols, event.data.protocol_num);
        view_dispatcher_send_custom_event(instance->view_dispatcher, NfcCustomEventWorkerExit);
    }
}

void nfc_scene_detect_on_enter(void* context) {
    NfcApp* instance = context;

    nfc_show_loading_popup(instance, true);
    nfc_supported_cards_load_cache(instance->nfc_supported_cards);
    nfc_show_loading_popup(instance, false);

    nfc_detected_protocols_reset(instance->detected_protocols);
    nfc_scene_detect_build_view(instance);

    if(chameleon_is_connected()) {
        /* Chameleon backend active — read via BLE, no PN532 scanner */
        s_cham.scanner_started = false;
        nfc_scene_detect_chameleon_worker_start(instance);
    } else {
        instance->scanner = nfc_scanner_alloc(instance->nfc);
        nfc_scanner_start(instance->scanner, nfc_scene_detect_scan_callback, instance);
        s_cham.scanner_started = true;
    }

    nfc_blink_detect_start(instance);
}

bool nfc_scene_detect_on_event(void* context, SceneManagerEvent event) {
    NfcApp* instance = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == NfcCustomEventWorkerExit) {
            if(nfc_detected_protocols_get_num(instance->detected_protocols) > 1) {
                notification_message(instance->notifications, &sequence_single_vibro);
                scene_manager_next_scene(instance->scene_manager, NfcSceneSelectProtocol);
            } else {
                scene_manager_next_scene(instance->scene_manager, NfcSceneRead);
            }
            consumed = true;
        } else if(event.event == NfcCustomEventChameleonButton) {
            if(chameleon_is_connected()) {
                /* Disconnect: stop loop, drop BLE link, resume PN532 */
                nfc_scene_detect_chameleon_worker_stop();
                chameleon_disconnect();
                nfc_scene_detect_build_view(instance);
                instance->scanner = nfc_scanner_alloc(instance->nfc);
                nfc_scanner_start(instance->scanner, nfc_scene_detect_scan_callback, instance);
                s_cham.scanner_started = true;
            } else {
                /* Connect: stop PN532 scanner, spawn connect+read worker */
                if(s_cham.scanner_started) {
                    nfc_scanner_stop(instance->scanner);
                    nfc_scanner_free(instance->scanner);
                    s_cham.scanner_started = false;
                }
                widget_reset(instance->widget);
                widget_add_string_element(
                    instance->widget,
                    64,
                    28,
                    AlignCenter,
                    AlignCenter,
                    FontPrimary,
                    "Connecting Chameleon...");
                view_dispatcher_switch_to_view(instance->view_dispatcher, NfcViewWidget);
                nfc_scene_detect_chameleon_worker_start(instance);
            }
            consumed = true;
        } else if(event.event == NfcCustomEventChameleonConnected) {
            nfc_scene_detect_build_view(instance);
            consumed = true;
        } else if(event.event == NfcCustomEventChameleonFailed) {
            nfc_scene_detect_chameleon_worker_stop();
            nfc_scene_detect_build_view(instance);
            instance->scanner = nfc_scanner_alloc(instance->nfc);
            nfc_scanner_start(instance->scanner, nfc_scene_detect_scan_callback, instance);
            s_cham.scanner_started = true;
            consumed = true;
        } else if(event.event == NfcCustomEventChameleonCardRead) {
            scene_manager_next_scene(instance->scene_manager, NfcSceneReadSuccess);
            consumed = true;
        }
    }

    return consumed;
}

void nfc_scene_detect_on_exit(void* context) {
    NfcApp* instance = context;

    nfc_scene_detect_chameleon_worker_stop();
    if(s_cham.scanner_started) {
        nfc_scanner_stop(instance->scanner);
        nfc_scanner_free(instance->scanner);
        s_cham.scanner_started = false;
    }
    widget_reset(instance->widget);
    popup_reset(instance->popup);

    nfc_blink_stop(instance);
}
