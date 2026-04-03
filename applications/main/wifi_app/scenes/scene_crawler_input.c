#include "../wifi_app.h"
#include "../wifi_hal.h"

static void crawler_input_callback(void* context) {
    WifiApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, WifiAppCustomEventCrawlerDomainEntered);
}

void wifi_app_scene_crawler_input_on_enter(void* context) {
    WifiApp* app = context;

    // Check WiFi connection
    if(!wifi_hal_is_connected()) {
        widget_reset(app->widget);
        widget_add_string_multiline_element(
            app->widget, 64, 28, AlignCenter, AlignCenter, FontPrimary,
            "Not connected!\nUse Scanner first.");
        view_dispatcher_switch_to_view(app->view_dispatcher, WifiAppViewWidget);
        return;
    }

    // Set default domain
    if(!app->crawler_domain[0]) {
        strncpy(app->crawler_domain, "dummy.com", sizeof(app->crawler_domain));
    }

    text_input_set_header_text(app->text_input, "Domain:");
    text_input_set_result_callback(
        app->text_input,
        crawler_input_callback,
        app,
        app->crawler_domain,
        sizeof(app->crawler_domain),
        false);

    view_dispatcher_switch_to_view(app->view_dispatcher, WifiAppViewTextInput);
}

bool wifi_app_scene_crawler_input_on_event(void* context, SceneManagerEvent event) {
    WifiApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == WifiAppCustomEventCrawlerDomainEntered) {
            scene_manager_next_scene(app->scene_manager, WifiAppSceneCrawler);
            consumed = true;
        }
    }

    return consumed;
}

void wifi_app_scene_crawler_input_on_exit(void* context) {
    WifiApp* app = context;
    text_input_reset(app->text_input);
    widget_reset(app->widget);
}
