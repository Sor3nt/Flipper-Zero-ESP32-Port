#include "../subghz_i.h"
#include "subghz_scene_start.h"
#include <dolphin/dolphin.h>

#include <lib/subghz/protocols/raw.h>
#include <storage/storage.h>
#include <loader/loader.h>

// Optionale externe FAPs: Menü-Einträge erscheinen nur, wenn die FAP auf der
// SD-Karte liegt (SubGhz-Unterordner bevorzugt, sonst /ext/apps).
static const char* const subghz_wmburst_fap_paths[] = {
    EXT_PATH("apps/SubGhz/wmbuster.fap"),
    EXT_PATH("apps/wmbuster.fap"),
};

static const char* const subghz_protopirate_fap_paths[] = {
    EXT_PATH("apps/SubGhz/proto_pirate.fap"),
    EXT_PATH("apps/proto_pirate.fap"),
};

// Liefert den ersten existierenden Pfad aus der Liste oder NULL.
static const char* subghz_scene_start_fap_find(const char* const* paths, size_t count) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    const char* found = NULL;
    for(size_t i = 0; i < count; i++) {
        if(storage_common_exists(storage, paths[i])) {
            found = paths[i];
            break;
        }
    }
    furi_record_close(RECORD_STORAGE);
    return found;
}

// Startet eine externe FAP und beendet Sub-GHz. Der Loader ist gelockt, solange
// Sub-GHz läuft; deshalb den Start aufschieben (loader_enqueue_launch merkt ihn)
// und die App beenden — sobald sie zu ist, startet der Loader die pending FAP.
static void subghz_scene_start_launch_fap(SubGhz* subghz, const char* fap_path) {
    Loader* loader = furi_record_open(RECORD_LOADER);
    loader_enqueue_launch(loader, fap_path, NULL, LoaderDeferredLaunchFlagGui);
    furi_record_close(RECORD_LOADER);
    scene_manager_stop(subghz->scene_manager);
    view_dispatcher_stop(subghz->view_dispatcher);
}

void subghz_scene_start_submenu_callback(void* context, uint32_t index) {
    SubGhz* subghz = context;
    view_dispatcher_send_custom_event(subghz->view_dispatcher, index);
}

void subghz_scene_start_on_enter(void* context) {
    SubGhz* subghz = context;
    if(subghz->state_notifications == SubGhzNotificationStateStarting) {
        subghz->state_notifications = SubGhzNotificationStateIDLE;
    }

    submenu_add_item(
        subghz->submenu, "Read", SubmenuIndexRead, subghz_scene_start_submenu_callback, subghz);
    submenu_add_item(
        subghz->submenu,
        "Read RAW",
        SubmenuIndexReadRAW,
        subghz_scene_start_submenu_callback,
        subghz);
    if(subghz_scene_start_fap_find(
           subghz_protopirate_fap_paths, COUNT_OF(subghz_protopirate_fap_paths)) != NULL) {
        submenu_add_item(
            subghz->submenu,
            "ProtoPirate",
            SubmenuIndexProtoPirate,
            subghz_scene_start_submenu_callback,
            subghz);
    }
    if(subghz_scene_start_fap_find(
           subghz_wmburst_fap_paths, COUNT_OF(subghz_wmburst_fap_paths)) != NULL) {
        submenu_add_item(
            subghz->submenu,
            "wM-Burst",
            SubmenuIndexWmBurst,
            subghz_scene_start_submenu_callback,
            subghz);
    }
    submenu_add_item(
        subghz->submenu,
        "Playlist",
        SubmenuIndexPlaylist,
        subghz_scene_start_submenu_callback,
        subghz);
    submenu_add_item(
        subghz->submenu,
        "Jammer",
        SubmenuIndexJammer,
        subghz_scene_start_submenu_callback,
        subghz);
    submenu_add_item(
        subghz->submenu,
        "Bruteforce",
        SubmenuIndexBruteforce,
        subghz_scene_start_submenu_callback,
        subghz);
    submenu_add_item(
        subghz->submenu, "Saved", SubmenuIndexSaved, subghz_scene_start_submenu_callback, subghz);
    submenu_add_item(
        subghz->submenu,
        "Add Manually",
        SubmenuIndexAddManually,
        subghz_scene_start_submenu_callback,
        subghz);
    submenu_add_item(
        subghz->submenu,
        "Add Manually [Advanced]",
        SubmenuIndexAddManuallyAdvanced,
        subghz_scene_start_submenu_callback,
        subghz);
    submenu_add_item(
        subghz->submenu,
        "Frequency Analyzer",
        SubmenuIndexFrequencyAnalyzer,
        subghz_scene_start_submenu_callback,
        subghz);
    submenu_add_item(
        subghz->submenu,
        "RF Spectrum",
        SubmenuIndexSpectrum,
        subghz_scene_start_submenu_callback,
        subghz);
    submenu_add_item(
        subghz->submenu,
        "Radio Settings",
        SubmenuIndexExtSettings,
        subghz_scene_start_submenu_callback,
        subghz);
    submenu_set_selected_item(
        subghz->submenu, scene_manager_get_scene_state(subghz->scene_manager, SubGhzSceneStart));

    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdMenu);
}

bool subghz_scene_start_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;
    if(event.type == SceneManagerEventTypeBack) {
        //exit app
        scene_manager_stop(subghz->scene_manager);
        view_dispatcher_stop(subghz->view_dispatcher);
        return true;
    } else if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == SubmenuIndexReadRAW) {
            scene_manager_set_scene_state(
                subghz->scene_manager, SubGhzSceneStart, SubmenuIndexReadRAW);
            subghz_rx_key_state_set(subghz, SubGhzRxKeyStateIDLE);
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneReadRAW);
            return true;
        } else if(event.event == SubmenuIndexProtoPirate) {
            scene_manager_set_scene_state(
                subghz->scene_manager, SubGhzSceneStart, SubmenuIndexProtoPirate);
            const char* fap_path = subghz_scene_start_fap_find(
                subghz_protopirate_fap_paths, COUNT_OF(subghz_protopirate_fap_paths));
            if(fap_path != NULL) {
                subghz_scene_start_launch_fap(subghz, fap_path);
            }
            return true;
        } else if(event.event == SubmenuIndexWmBurst) {
            scene_manager_set_scene_state(
                subghz->scene_manager, SubGhzSceneStart, SubmenuIndexWmBurst);
            const char* fap_path = subghz_scene_start_fap_find(
                subghz_wmburst_fap_paths, COUNT_OF(subghz_wmburst_fap_paths));
            if(fap_path != NULL) {
                subghz_scene_start_launch_fap(subghz, fap_path);
            }
            return true;
        } else if(event.event == SubmenuIndexRead) {
            subghz_rx_key_state_set(subghz, SubGhzRxKeyStateIDLE);
            scene_manager_set_scene_state(
                subghz->scene_manager, SubGhzSceneStart, SubmenuIndexRead);
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneReceiver);
            return true;
        } else if(event.event == SubmenuIndexPlaylist) {
            scene_manager_set_scene_state(
                subghz->scene_manager, SubGhzSceneStart, SubmenuIndexPlaylist);
            scene_manager_next_scene(subghz->scene_manager, SubGhzScenePlaylist);
            return true;
        } else if(event.event == SubmenuIndexJammer) {
            scene_manager_set_scene_state(
                subghz->scene_manager, SubGhzSceneStart, SubmenuIndexJammer);
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneJammer);
            return true;
        } else if(event.event == SubmenuIndexBruteforce) {
            scene_manager_set_scene_state(
                subghz->scene_manager, SubGhzSceneStart, SubmenuIndexBruteforce);
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneBfStart);
            return true;
        } else if(event.event == SubmenuIndexSaved) {
            scene_manager_set_scene_state(
                subghz->scene_manager, SubGhzSceneStart, SubmenuIndexSaved);
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneSaved);
            return true;
        } else if(event.event == SubmenuIndexAddManually) {
            scene_manager_set_scene_state(
                subghz->scene_manager, SubGhzSceneStart, SubmenuIndexAddManually);
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneSetType);
            return true;
        } else if(event.event == SubmenuIndexAddManuallyAdvanced) {
            scene_manager_set_scene_state(
                subghz->scene_manager, SubGhzSceneStart, SubmenuIndexAddManuallyAdvanced);
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneSetType);
            return true;
        } else if(event.event == SubmenuIndexFrequencyAnalyzer) {
            scene_manager_set_scene_state(
                subghz->scene_manager, SubGhzSceneStart, SubmenuIndexFrequencyAnalyzer);
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneFrequencyAnalyzer);
            dolphin_deed(DolphinDeedSubGhzFrequencyAnalyzer);
            return true;
        } else if(event.event == SubmenuIndexSpectrum) {
            scene_manager_set_scene_state(
                subghz->scene_manager, SubGhzSceneStart, SubmenuIndexSpectrum);
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneSpectrum);
            return true;
        } else if(event.event == SubmenuIndexExtSettings) {
            scene_manager_set_scene_state(
                subghz->scene_manager, SubGhzSceneStart, SubmenuIndexExtSettings);
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneExtModuleSettings);
            return true;
        }
    }
    return false;
}

void subghz_scene_start_on_exit(void* context) {
    SubGhz* subghz = context;
    submenu_reset(subghz->submenu);
}
