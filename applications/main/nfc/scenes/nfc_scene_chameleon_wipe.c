#include "../nfc_app_i.h"
#include "../chameleon/chameleon.h"
#include "../chameleon/chameleon_nfc.h"

/* "Wipe (Chameleon)" — reset a physical MIFARE Classic to factory blank via
 * an external ChameleonUltra. Connects if needed, then waits for a card in
 * the Chameleon's field and resets it (zero data + default trailers). */

typedef struct {
    FuriThread* thread;
    volatile bool abort;
    bool running;
} NfcChameleonWipeWorker;

static NfcChameleonWipeWorker s_wipe;

static int32_t nfc_scene_chameleon_wipe_worker(void* context) {
    NfcApp* instance = context;
    if(!chameleon_is_connected()) {
        if(!chameleon_connect(&s_wipe.abort)) {
            view_dispatcher_send_custom_event(
                instance->view_dispatcher, NfcCustomEventChameleonFailed);
            return 0;
        }
    }
    view_dispatcher_send_custom_event(instance->view_dispatcher, NfcCustomEventChameleonConnected);

    while(!s_wipe.abort) {
        if(chameleon_nfc_wipe_card(&s_wipe.abort)) {
            view_dispatcher_send_custom_event(
                instance->view_dispatcher, NfcCustomEventChameleonCardRead);
            break;
        }
        furi_delay_ms(300);
    }
    return 0;
}

static void nfc_scene_chameleon_wipe_worker_stop(void) {
    if(!s_wipe.running) return;
    s_wipe.abort = true;
    furi_thread_join(s_wipe.thread);
    furi_thread_free(s_wipe.thread);
    s_wipe.thread = NULL;
    s_wipe.running = false;
}

static void nfc_scene_chameleon_wipe_show(NfcApp* instance, const char* msg) {
    widget_reset(instance->widget);
    widget_add_string_multiline_element(
        instance->widget, 64, 28, AlignCenter, AlignCenter, FontPrimary, msg);
    view_dispatcher_switch_to_view(instance->view_dispatcher, NfcViewWidget);
}

void nfc_scene_chameleon_wipe_on_enter(void* context) {
    NfcApp* instance = context;
    nfc_scene_chameleon_wipe_show(
        instance,
        chameleon_is_connected() ? "Wiping via Chameleon...\nPlace card" :
                                   "Connecting Chameleon...");
    s_wipe.abort = false;
    s_wipe.thread =
        furi_thread_alloc_ex("ChamWipe", 8 * 1024, nfc_scene_chameleon_wipe_worker, instance);
    furi_thread_start(s_wipe.thread);
    s_wipe.running = true;
    nfc_blink_emulate_start(instance);
}

bool nfc_scene_chameleon_wipe_on_event(void* context, SceneManagerEvent event) {
    NfcApp* instance = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == NfcCustomEventChameleonConnected) {
            nfc_scene_chameleon_wipe_show(instance, "Wiping via Chameleon...\nPlace card");
            consumed = true;
        } else if(event.event == NfcCustomEventChameleonCardRead) {
            notification_message(instance->notifications, &sequence_success);
            nfc_scene_chameleon_wipe_show(instance, "Card wiped\nto blank!");
            consumed = true;
        } else if(event.event == NfcCustomEventChameleonFailed) {
            notification_message(instance->notifications, &sequence_error);
            nfc_scene_chameleon_wipe_show(instance, "Chameleon\nnot connected");
            consumed = true;
        }
    }
    return consumed;
}

void nfc_scene_chameleon_wipe_on_exit(void* context) {
    NfcApp* instance = context;
    nfc_scene_chameleon_wipe_worker_stop();
    widget_reset(instance->widget);
    nfc_blink_stop(instance);
}
