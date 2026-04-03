#include "../esp_now_app.h"

void esp_now_app_scene_packet_info_on_enter(void* context) {
    EspNowApp* app = context;

    furi_mutex_acquire(app->mutex, FuriWaitForever);

    if(app->selected_index < app->packet_count) {
        EspNowPacket* pkt = &app->packets[app->selected_index];

        furi_string_printf(
            app->text_buf,
            "MAC: %02X:%02X:%02X:%02X:%02X:%02X\nLen: %d bytes\n\n",
            pkt->mac[0],
            pkt->mac[1],
            pkt->mac[2],
            pkt->mac[3],
            pkt->mac[4],
            pkt->mac[5],
            pkt->data_len);

        for(uint8_t i = 0; i < pkt->data_len; i++) {
            furi_string_cat_printf(app->text_buf, "%02X ", pkt->data[i]);
            if((i + 1) % 8 == 0) {
                furi_string_cat_str(app->text_buf, "\n");
            }
        }
    } else {
        furi_string_set_str(app->text_buf, "No packet data");
    }

    furi_mutex_release(app->mutex);

    widget_add_text_scroll_element(
        app->widget, 0, 0, 128, 64, furi_string_get_cstr(app->text_buf));

    view_dispatcher_switch_to_view(app->view_dispatcher, EspNowViewWidget);
}

bool esp_now_app_scene_packet_info_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void esp_now_app_scene_packet_info_on_exit(void* context) {
    EspNowApp* app = context;
    widget_reset(app->widget);
}
