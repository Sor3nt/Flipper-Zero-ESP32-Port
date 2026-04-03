#pragma once

#include <gui/view.h>
#include <gui/view_dispatcher.h>

typedef struct {
    char domain[128];
    uint32_t page_count;
    uint32_t max_pages;
    uint32_t image_count;
    uint32_t queue_pending;
    uint32_t total_bytes;
    char current_url[64];
    bool running;
} CrawlerModel;

View* crawler_view_alloc(void);
void crawler_view_free(View* view);
void crawler_view_set_view_dispatcher(ViewDispatcher* vd);
