#include "../streaming.h"
#include "../airplay_mdns.h"
#include "../dlna_ssdp.h"

#include <wlan_app/views/wlan_lan_view.h>
#include <wlan_app/views/wlan_view_events.h>

#include <stdio.h>
#include <string.h>

/* Discovery runs on a helper FuriThread (airplay_mdns_scan / dlna_ssdp_scan
 * block on the WiFi worker). The UI shows the shared LAN list view (same look
 * as the wlan_app Android TV / SMB scans) meanwhile. */
static int32_t scan_thread_fn(void* ctx) {
    StreamingApp* app = ctx;
    app->device_count = 0;

    /* Audio can stream to AirPlay too → discover RAOP receivers first. */
    if(app->sel_kind == MediaKindAudio) {
        AirplayDevice ad[AIRPLAY_MAX_DEVICES];
        int n = airplay_mdns_scan(ad, AIRPLAY_MAX_DEVICES, 3000);
        for(int i = 0; i < n && app->device_count < STREAMING_MAX_DEVICES; ++i) {
            StreamDevice* d = &app->devices[app->device_count++];
            memset(d, 0, sizeof(*d));
            d->type = StreamDeviceAirplay;
            strncpy(d->name, ad[i].name, sizeof(d->name) - 1);
            d->ip = ad[i].ip;
            d->port = ad[i].port;
            d->et = ad[i].et;
            d->cn = ad[i].cn;
            d->needs_password = ad[i].needs_password;
        }
    }

    /* Cast + DLNA renderers (both audio and video). */
    DlnaDevice dd[DLNA_MAX_DEVICES];
    int m = dlna_ssdp_scan(dd, DLNA_MAX_DEVICES, 6000);
    for(int i = 0; i < m && app->device_count < STREAMING_MAX_DEVICES; ++i) {
        StreamDevice* d = &app->devices[app->device_count++];
        memset(d, 0, sizeof(*d));
        d->type = dd[i].has_cast ? StreamDeviceCast : StreamDeviceDlna;
        strncpy(d->name, dd[i].name, sizeof(d->name) - 1);
        d->ip = dd[i].ip;
        d->port = dd[i].port;
        strncpy(d->av_control, dd[i].av_control, sizeof(d->av_control) - 1);
        strncpy(d->rc_control, dd[i].rc_control, sizeof(d->rc_control) - 1);
        d->has_cast = dd[i].has_cast;
    }

    app->scan_busy = false;
    view_dispatcher_send_custom_event(app->view_dispatcher, StreamingEventDeviceScanDone);
    return 0;
}

static void scan_join(StreamingApp* app) {
    if(app->scan_thread) {
        furi_thread_join(app->scan_thread);
        furi_thread_free(app->scan_thread);
        app->scan_thread = NULL;
    }
}

static const char* device_type_tag(StreamDeviceType t) {
    return (t == StreamDeviceAirplay) ? "AirPlay" : (t == StreamDeviceCast) ? "Cast" : "DLNA";
}

void streaming_scene_device_scan_on_enter(void* context) {
    StreamingApp* app = context;
    app->device_count = 0;
    app->scan_busy = true;

    View* v = app->view_lan;
    wlan_lan_view_clear_menu(v);
    wlan_lan_view_close_menu(v);
    wlan_lan_view_set_force_selection_counter(v, false);
    wlan_lan_view_set_header_title(v, "Devices");
    wlan_lan_view_clear(v);
    wlan_lan_view_set_empty_text(v, "Scanning...");
    view_dispatcher_switch_to_view(app->view_dispatcher, StreamingViewLan);

    app->scan_thread = furi_thread_alloc_ex("StrmScan", 4096, scan_thread_fn, app);
    furi_thread_start(app->scan_thread);
}

bool streaming_scene_device_scan_on_event(void* context, SceneManagerEvent event) {
    StreamingApp* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;

    if(event.event == StreamingEventDeviceScanDone) {
        scan_join(app);
        View* v = app->view_lan;
        wlan_lan_view_clear(v);
        wlan_lan_view_set_empty_text(v, "No devices");
        for(uint8_t i = 0; i < app->device_count; ++i) {
            StreamDevice* d = &app->devices[i];
            wlan_lan_view_add_device(
                v, d->name, NULL, device_type_tag(d->type), NULL, true, i);
        }
        char title[24];
        snprintf(title, sizeof(title), "Devices (%u)", (unsigned)app->device_count);
        wlan_lan_view_set_header_title(v, title);
        return true;
    }

    if(event.event == WlanAppCustomEventLanItemOk) {
        View* v = app->view_lan;
        uint8_t sel = wlan_lan_view_get_selected(v);
        WlanLanItem it = wlan_lan_view_get_item(v, sel);
        if(it.kind == WlanLanItemKindDevice && it.user_id < app->device_count) {
            StreamDevice* d = &app->devices[it.user_id];
            app->cur_device = *d;
            app->have_device = true;
            app->play_mode = (d->type == StreamDeviceAirplay) ? PlayModeAirplay :
                             (d->type == StreamDeviceCast)    ? PlayModeCast :
                                                                PlayModeDlna;
            scene_manager_next_scene(app->scene_manager, StreamingScenePlayer);
        }
        return true;
    }

    return false;
}

void streaming_scene_device_scan_on_exit(void* context) {
    StreamingApp* app = context;
    scan_join(app); /* waits out an in-flight scan if the user backed out */
    wlan_lan_view_clear(app->view_lan);
    wlan_lan_view_set_empty_text(app->view_lan, NULL);
}
