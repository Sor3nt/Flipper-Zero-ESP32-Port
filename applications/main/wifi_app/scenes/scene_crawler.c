#include "../wifi_app.h"
#include "../wifi_crawler.h"
#include "../views/crawler_view.h"

#include <esp_log.h>
#include <string.h>

#define TAG WIFI_APP_LOG_TAG

void wifi_app_scene_crawler_on_enter(void* context) {
    WifiApp* app = context;

    // Init crawler view model
    CrawlerModel* model = view_get_model(app->view_crawler);
    memset(model, 0, sizeof(CrawlerModel));
    strncpy(model->domain, app->crawler_domain, sizeof(model->domain) - 1);
    model->max_pages = CRAWLER_MAX_PAGES;
    model->running = true;
    view_commit_model(app->view_crawler, true);

    // Start crawler
    wifi_crawler_start(&app->crawler_state, app->crawler_domain);

    view_dispatcher_switch_to_view(app->view_dispatcher, WifiAppViewCrawler);
}

bool wifi_app_scene_crawler_on_event(void* context, SceneManagerEvent event) {
    WifiApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == InputKeyOk) {
            // Stop crawler
            wifi_crawler_stop(&app->crawler_state);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        // Update view model from crawler state
        WifiCrawlerState* s = &app->crawler_state;
        CrawlerModel* model = view_get_model(app->view_crawler);
        model->page_count = s->page_count;
        model->image_count = s->image_count;
        model->queue_pending = s->queue_pending;
        model->total_bytes = s->total_bytes;
        model->running = s->running;

        // Truncate current URL for display
        const char* cur = s->current_url;
        // Show just the path part
        const char* path = strstr(cur, "://");
        if(path) {
            path = strchr(path + 3, '/');
        }
        if(path) {
            strncpy(model->current_url, path, sizeof(model->current_url) - 1);
            model->current_url[sizeof(model->current_url) - 1] = '\0';
        } else {
            model->current_url[0] = '\0';
        }

        view_commit_model(app->view_crawler, true);

        // Auto-back when done
        if(!s->running && s->queue_pending == 0) {
            // Crawler finished naturally
        }

        consumed = true;
    }

    return consumed;
}

void wifi_app_scene_crawler_on_exit(void* context) {
    WifiApp* app = context;
    wifi_crawler_stop(&app->crawler_state);
}
