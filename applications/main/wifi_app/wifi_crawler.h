#pragma once

#include <stdbool.h>
#include <stdint.h>

#define CRAWLER_MAX_PAGES    200
#define CRAWLER_MAX_URL_LEN  256
#define CRAWLER_NUM_WORKERS  2

typedef struct {
    char domain[128];
    volatile bool running;
    volatile uint32_t page_count;
    volatile uint32_t image_count;
    volatile uint32_t total_bytes;
    volatile uint32_t queue_pending;
    char current_url[CRAWLER_MAX_URL_LEN];
    uint32_t max_pages;
} WifiCrawlerState;

/** Start crawling a domain. WiFi must be connected.
 *  Spawns worker tasks that download pages/images to SD card.
 *  @param state  Crawler state (caller-owned, must stay alive)
 *  @param domain Domain to crawl (e.g. "test.de") */
void wifi_crawler_start(WifiCrawlerState* state, const char* domain);

/** Stop crawling and wait for workers to finish. */
void wifi_crawler_stop(WifiCrawlerState* state);
