#include "wifi_crawler.h"
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <furi.h>
#include <storage/storage.h>
#include <string.h>
#include <stdio.h>

#define TAG "Crawler"
#define WORKER_STACK_SIZE   6144
#define READ_BUF_SIZE       1024
#define QUEUE_CAPACITY      200
#define VISITED_SLOTS       256

// Forward declarations
static bool is_unwanted_path(const char* url);
static bool is_unwanted_ext(const char* url);
static bool is_image_ext(const char* url);

// --- URL Queue with visited hash set (dynamically allocated) ---

typedef struct {
    char (*urls)[CRAWLER_MAX_URL_LEN]; // malloc'd: QUEUE_CAPACITY * 256
    uint16_t head;
    uint16_t tail;
    uint16_t count;
    uint32_t visited[VISITED_SLOTS];
    uint16_t visited_count;
    FuriMutex* mutex;
} CrawlerQueue;

static CrawlerQueue* s_queue = NULL;
static WifiCrawlerState* s_state = NULL;
static TaskHandle_t s_workers[CRAWLER_NUM_WORKERS] = {0};
static StackType_t* s_worker_stacks[CRAWLER_NUM_WORKERS] = {0};
static StaticTask_t s_worker_bufs[CRAWLER_NUM_WORKERS];

static uint32_t fnv1a_hash(const char* str) {
    uint32_t h = 0x811c9dc5;
    while(*str) {
        h ^= (uint8_t)*str++;
        h *= 0x01000193;
    }
    return h;
}

static bool visited_contains(uint32_t hash) {
    uint32_t idx = hash & (VISITED_SLOTS - 1);
    for(uint16_t i = 0; i < VISITED_SLOTS; i++) {
        uint32_t slot = (idx + i) & (VISITED_SLOTS - 1);
        if(s_queue->visited[slot] == 0) return false;
        if(s_queue->visited[slot] == hash) return true;
    }
    return false;
}

static void visited_add(uint32_t hash) {
    if(s_queue->visited_count >= VISITED_SLOTS - 1) return;
    uint32_t idx = hash & (VISITED_SLOTS - 1);
    for(uint16_t i = 0; i < VISITED_SLOTS; i++) {
        uint32_t slot = (idx + i) & (VISITED_SLOTS - 1);
        if(s_queue->visited[slot] == 0 || s_queue->visited[slot] == hash) {
            s_queue->visited[slot] = hash;
            s_queue->visited_count++;
            return;
        }
    }
}

static bool queue_push(const char* url) {
    if(!s_queue) return false;

    // Normalize: strip trailing slash for dedup (except root "/")
    char norm[CRAWLER_MAX_URL_LEN];
    strncpy(norm, url, CRAWLER_MAX_URL_LEN - 1);
    norm[CRAWLER_MAX_URL_LEN - 1] = '\0';
    size_t len = strlen(norm);
    const char* path = strstr(norm, "://");
    if(path) path = strchr(path + 3, '/');
    // Only strip if path has more than just "/"
    if(len > 1 && norm[len - 1] == '/' && path && strlen(path) > 1) {
        norm[len - 1] = '\0';
    }

    // Filter unwanted paths
    if(is_unwanted_path(norm)) return false;

    furi_mutex_acquire(s_queue->mutex, FuriWaitForever);
    bool ok = false;

    uint32_t hash = fnv1a_hash(norm);
    if(!visited_contains(hash) && s_queue->count < QUEUE_CAPACITY) {
        visited_add(hash);
        // Store original URL (with or without slash — doesn't matter for fetch)
        strncpy(s_queue->urls[s_queue->tail], url, CRAWLER_MAX_URL_LEN - 1);
        s_queue->urls[s_queue->tail][CRAWLER_MAX_URL_LEN - 1] = '\0';
        s_queue->tail = (s_queue->tail + 1) % QUEUE_CAPACITY;
        s_queue->count++;
        ok = true;
    }

    if(s_state) s_state->queue_pending = s_queue->count;
    furi_mutex_release(s_queue->mutex);
    return ok;
}

static bool queue_pop(char* out_url) {
    if(!s_queue) return false;
    furi_mutex_acquire(s_queue->mutex, FuriWaitForever);
    bool ok = false;

    if(s_queue->count > 0) {
        strncpy(out_url, s_queue->urls[s_queue->head], CRAWLER_MAX_URL_LEN);
        s_queue->head = (s_queue->head + 1) % QUEUE_CAPACITY;
        s_queue->count--;
        ok = true;
    }

    if(s_state) s_state->queue_pending = s_queue->count;
    furi_mutex_release(s_queue->mutex);
    return ok;
}

// --- URL helpers ---

static bool is_same_domain(const char* url, const char* domain) {
    const char* host = strstr(url, "://");
    if(!host) return false;
    host += 3;
    size_t dlen = strlen(domain);
    if(strncmp(host, domain, dlen) != 0) return false;
    char next = host[dlen];
    return (next == '/' || next == '\0' || next == ':' || next == '?');
}

static bool is_unwanted_path(const char* url) {
    UNUSED(url);
    return false;
}

static void normalize_url(const char* base_url, const char* href, const char* domain, char* out, size_t out_len) {
    // Skip fragments and special protocols
    if(!href || !href[0] || href[0] == '#') { out[0] = '\0'; return; }
    if(strncmp(href, "mailto:", 7) == 0 || strncmp(href, "tel:", 4) == 0 ||
       strncmp(href, "javascript:", 11) == 0 || strncmp(href, "data:", 5) == 0) {
        out[0] = '\0'; return;
    }

    // Already absolute
    if(strncmp(href, "http://", 7) == 0 || strncmp(href, "https://", 8) == 0) {
        strncpy(out, href, out_len - 1);
        out[out_len - 1] = '\0';
    }
    // Protocol-relative
    else if(strncmp(href, "//", 2) == 0) {
        snprintf(out, out_len, "https:%s", href);
    }
    // Root-relative
    else if(href[0] == '/') {
        snprintf(out, out_len, "https://%s%s", domain, href);
    }
    // Relative
    else {
        // Find base path
        const char* host_end = strstr(base_url, "://");
        if(!host_end) { out[0] = '\0'; return; }
        const char* path_start = strchr(host_end + 3, '/');
        if(path_start) {
            const char* last_slash = strrchr(path_start, '/');
            if(last_slash) {
                int base_len = (int)(last_slash - base_url) + 1;
                snprintf(out, out_len, "%.*s%s", base_len, base_url, href);
            } else {
                snprintf(out, out_len, "https://%s/%s", domain, href);
            }
        } else {
            snprintf(out, out_len, "https://%s/%s", domain, href);
        }
    }

    // Strip fragment
    char* frag = strchr(out, '#');
    if(frag) *frag = '\0';

    // Strip query params for simplicity
    char* query = strchr(out, '?');
    if(query) *query = '\0';

    // Resolve /../ and /./ in path
    char* path = strstr(out, "://");
    if(path) {
        path = strchr(path + 3, '/');
    }
    if(path && (strstr(path, "/../") || strstr(path, "/./"))) {
        char resolved[CRAWLER_MAX_URL_LEN];
        size_t prefix_len = path - out;
        if(prefix_len < sizeof(resolved)) {
            memcpy(resolved, out, prefix_len);
            char* dst = resolved + prefix_len;
            const char* src = path;
            while(*src) {
                if(strncmp(src, "/../", 4) == 0 || (strncmp(src, "/..", 3) == 0 && src[3] == '\0')) {
                    // Go up one directory
                    if(dst > resolved + prefix_len) {
                        dst--;
                        while(dst > resolved + prefix_len && dst[-1] != '/') dst--;
                    }
                    src += (src[3] == '/') ? 4 : 3;
                } else if(strncmp(src, "/./", 3) == 0 || (strncmp(src, "/.", 2) == 0 && src[2] == '\0')) {
                    src += (src[2] == '/') ? 2 : 2;
                } else {
                    *dst++ = *src++;
                }
            }
            *dst = '\0';
            strncpy(out, resolved, out_len - 1);
            out[out_len - 1] = '\0';
        }
    }
}

// --- File path mapping ---

static void url_to_sd_path(const char* url, const char* domain, char* path, size_t path_len) {
    const char* host_end = strstr(url, "://");
    if(!host_end) { path[0] = '\0'; return; }

    // Extract actual host from URL (may differ from crawl domain for CDN images)
    const char* host_start = host_end + 3;
    const char* after_host = strchr(host_start, '/');
    char url_host[128];
    if(after_host) {
        size_t hlen = after_host - host_start;
        if(hlen >= sizeof(url_host)) hlen = sizeof(url_host) - 1;
        memcpy(url_host, host_start, hlen);
        url_host[hlen] = '\0';
    } else {
        strncpy(url_host, host_start, sizeof(url_host) - 1);
        url_host[sizeof(url_host) - 1] = '\0';
    }
    // Strip port
    char* colon = strchr(url_host, ':');
    if(colon) *colon = '\0';

    // Use crawl domain folder for same-domain, host folder for external
    const char* folder = is_same_domain(url, domain) ? domain : url_host;

    char url_path[128] = "/";
    if(after_host && after_host[1]) {
        strncpy(url_path, after_host, sizeof(url_path) - 1);
        url_path[sizeof(url_path) - 1] = '\0';
    }

    // Strip query string from path
    char* qmark = strchr(url_path, '?');
    if(qmark) *qmark = '\0';

    // If path ends with / or has no extension, append index.html
    size_t plen = strlen(url_path);
    if(plen > 0 && url_path[plen - 1] == '/') {
        strncat(url_path, "index.html", sizeof(url_path) - plen - 1);
    } else {
        const char* last_slash = strrchr(url_path, '/');
        const char* dot = last_slash ? strchr(last_slash, '.') : strchr(url_path, '.');
        if(!dot) {
            strncat(url_path, "/index.html", sizeof(url_path) - plen - 1);
        }
    }

    // Sanitize: replace problematic chars
    for(char* p = url_path; *p; p++) {
        if(*p == '?' || *p == '*' || *p == '"' || *p == '<' || *p == '>' || *p == '|' || *p == '%') {
            *p = '_';
        }
    }

    snprintf(path, path_len, "/ext/wifi/crawler/%s%s", folder, url_path);
}

static void ensure_parent_dirs(Storage* storage, const char* path) {
    char dir[192];
    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';

    char* last_slash = strrchr(dir, '/');
    if(last_slash) *last_slash = '\0';

    // Create directories recursively
    for(char* p = dir + 1; *p; p++) {
        if(*p == '/') {
            *p = '\0';
            storage_common_mkdir(storage, dir);
            *p = '/';
        }
    }
    storage_common_mkdir(storage, dir);
}

// --- HTML link extraction ---

static bool is_image_ext(const char* url) {
    const char* dot = strrchr(url, '.');
    if(!dot) return false;
    // Strip query string after extension
    char ext[8];
    size_t i = 0;
    for(const char* p = dot; *p && *p != '?' && *p != '#' && i < sizeof(ext) - 1; p++) {
        ext[i++] = *p;
    }
    ext[i] = '\0';
    return (strcasecmp(ext, ".png") == 0 || strcasecmp(ext, ".jpg") == 0 ||
            strcasecmp(ext, ".jpeg") == 0 || strcasecmp(ext, ".webp") == 0 ||
            strcasecmp(ext, ".webm") == 0);
}

static bool is_unwanted_ext(const char* url) {
    const char* dot = strrchr(url, '.');
    if(!dot) return false;
    char ext[8];
    size_t i = 0;
    for(const char* p = dot; *p && *p != '?' && *p != '#' && i < sizeof(ext) - 1; p++) {
        ext[i++] = *p;
    }
    ext[i] = '\0';
    return (strcasecmp(ext, ".css") == 0 || strcasecmp(ext, ".js") == 0 ||
            strcasecmp(ext, ".svg") == 0 || strcasecmp(ext, ".ico") == 0 ||
            strcasecmp(ext, ".gif") == 0 || strcasecmp(ext, ".bmp") == 0 ||
            strcasecmp(ext, ".woff") == 0 || strcasecmp(ext, ".woff2") == 0 ||
            strcasecmp(ext, ".ttf") == 0 || strcasecmp(ext, ".eot") == 0 ||
            strcasecmp(ext, ".map") == 0 || strcasecmp(ext, ".xml") == 0 ||
            strcasecmp(ext, ".json") == 0 || strcasecmp(ext, ".pdf") == 0);
}

static void extract_and_queue_links(const char* html, size_t len, const char* base_url, const char* domain) {
    if(!s_state || !s_state->running) return;

    const char* patterns[] = {"href=\"", "src=\"", "href='", "src='"};
    const char end_chars[] = {'"', '"', '\'', '\''};

    for(int p = 0; p < 4; p++) {
        const char* pos = html;
        const char* end = html + len;

        while(pos < end && s_state->running) {
            pos = strstr(pos, patterns[p]);
            if(!pos || pos >= end) break;
            pos += strlen(patterns[p]);

            const char* quote_end = strchr(pos, end_chars[p]);
            if(!quote_end || quote_end >= end) break;

            size_t href_len = quote_end - pos;
            if(href_len == 0 || href_len >= CRAWLER_MAX_URL_LEN - 1) {
                pos = quote_end + 1;
                continue;
            }

            char href[CRAWLER_MAX_URL_LEN];
            memcpy(href, pos, href_len);
            href[href_len] = '\0';

            char abs_url[CRAWLER_MAX_URL_LEN];
            normalize_url(base_url, href, domain, abs_url, sizeof(abs_url));

            if(abs_url[0] && !is_unwanted_ext(abs_url)) {
                if(is_image_ext(abs_url)) {
                    // Images: allow from ANY domain (CDNs like wp.com, etc.)
                    queue_push(abs_url);
                } else if((p == 0 || p == 2) && is_same_domain(abs_url, domain)) {
                    // HTML links: same domain only
                    queue_push(abs_url);
                }
            }

            pos = quote_end + 1;
        }
    }
}

// --- Worker task ---

static void crawler_worker_fn(void* arg) {
    int worker_id = (int)(intptr_t)arg;
    ESP_LOGI(TAG, "Worker %d started", worker_id);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    char url[CRAWLER_MAX_URL_LEN];
    char* html_buf = NULL;

    int idle_count = 0;
    while(s_state && s_state->running) {
        if(!queue_pop(url)) {
            furi_delay_ms(200);
            idle_count++;
            // Wait up to 30 seconds for new URLs before giving up
            if(idle_count > 150) {
                ESP_LOGI(TAG, "W%d: Queue empty for 30s, stopping", worker_id);
                break;
            }
            continue;
        }
        idle_count = 0;

        if(s_state->page_count + s_state->image_count >= s_state->max_pages) {
            ESP_LOGI(TAG, "W%d: Max pages reached", worker_id);
            break;
        }

        // Update current URL
        strncpy(s_state->current_url, url, CRAWLER_MAX_URL_LEN - 1);

        // Map URL to SD path
        char sd_path[192];
        url_to_sd_path(url, s_state->domain, sd_path, sizeof(sd_path));

        // Determine content type from URL
        bool is_html = !is_image_ext(url) && !is_unwanted_ext(url);

        // Check if file already exists on SD card (cache)
        FileInfo finfo;
        bool file_exists = (storage_common_stat(storage, sd_path, &finfo) == FSE_OK && finfo.size > 0);

        if(file_exists && is_html) {
            // HTML already on SD — read from card for link extraction, skip HTTP
            ESP_LOGI(TAG, "W%d: CACHE %s (%lu bytes)", worker_id, sd_path, (unsigned long)finfo.size);

            size_t cap = (finfo.size < 524288) ? finfo.size + 1 : 524288;
            html_buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if(html_buf) {
                File* file = storage_file_alloc(storage);
                if(storage_file_open(file, sd_path, FSAM_READ, FSOM_OPEN_EXISTING)) {
                    size_t html_buf_size = storage_file_read(file, html_buf, cap - 1);
                    html_buf[html_buf_size] = '\0';
                    storage_file_close(file);

                    s_state->page_count++;
                    uint32_t before = s_queue ? s_queue->count : 0;
                    extract_and_queue_links(html_buf, html_buf_size, url, s_state->domain);
                    uint32_t after = s_queue ? s_queue->count : 0;
                    ESP_LOGI(TAG, "W%d: Extracted %lu URLs from cached HTML",
                             worker_id, (unsigned long)(after - before));
                }
                storage_file_free(file);
                free(html_buf);
                html_buf = NULL;
            }
            continue;
        }

        if(file_exists) {
            // Image/other file already on SD — skip entirely
            s_state->image_count++;
            continue;
        }

        // --- File not on SD, download via HTTP ---
        ensure_parent_dirs(storage, sd_path);

        ESP_LOGI(TAG, "W%d: GET %s", worker_id, url);

        esp_http_client_config_t config = {
            .url = url,
            .timeout_ms = 15000,
            .buffer_size = 2048,
            .buffer_size_tx = 256,
            .skip_cert_common_name_check = true,
            .max_redirection_count = 5,
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if(!client) {
            ESP_LOGE(TAG, "W%d: client init failed", worker_id);
            continue;
        }

        esp_err_t err = esp_http_client_open(client, 0);
        if(err != ESP_OK) {
            ESP_LOGE(TAG, "W%d: open failed: %s", worker_id, esp_err_to_name(err));
            esp_http_client_cleanup(client);
            continue;
        }

        int content_length = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);

        // Handle redirects
        if(status == 301 || status == 302) {
            esp_http_client_set_redirection(client);
            esp_http_client_close(client);
            err = esp_http_client_open(client, 0);
            if(err == ESP_OK) {
                content_length = esp_http_client_fetch_headers(client);
                status = esp_http_client_get_status_code(client);
            }
        }

        if(status != 200) {
            ESP_LOGW(TAG, "W%d: HTTP %d for %s", worker_id, status, url);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            continue;
        }

        // Open file for writing
        File* file = storage_file_alloc(storage);
        if(!storage_file_open(file, sd_path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
            ESP_LOGE(TAG, "W%d: Cannot write %s", worker_id, sd_path);
            storage_file_free(file);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            continue;
        }

        // Stream response to file + buffer HTML for link extraction
        size_t html_buf_size = 0;
        size_t html_buf_cap = 0;
        if(is_html) {
            size_t cap = (content_length > 0) ? (size_t)content_length + 1 : 262144;
            if(cap > 524288) cap = 524288;
            html_buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if(html_buf) {
                html_buf_cap = cap;
            } else {
                html_buf = malloc(16384);
                html_buf_cap = html_buf ? 16384 : 0;
            }
        }

        char read_buf[READ_BUF_SIZE];
        uint32_t total_read = 0;
        int read_len;

        while(s_state->running) {
            read_len = esp_http_client_read(client, read_buf, READ_BUF_SIZE);
            if(read_len <= 0) break;

            storage_file_write(file, read_buf, read_len);
            total_read += read_len;

            if(is_html && html_buf && html_buf_size + read_len < html_buf_cap) {
                memcpy(html_buf + html_buf_size, read_buf, read_len);
                html_buf_size += read_len;
            }
        }

        storage_file_close(file);
        storage_file_free(file);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);

        s_state->total_bytes += total_read;

        if(is_html) {
            s_state->page_count++;
            if(html_buf && html_buf_size > 0) {
                html_buf[html_buf_size] = '\0';
                uint32_t before = s_queue ? s_queue->count : 0;
                extract_and_queue_links(html_buf, html_buf_size, url, s_state->domain);
                uint32_t after = s_queue ? s_queue->count : 0;
                ESP_LOGI(TAG, "W%d: Extracted %lu URLs from %lu bytes",
                         worker_id, (unsigned long)(after - before), (unsigned long)html_buf_size);
            }
        } else {
            s_state->image_count++;
        }

        if(html_buf) {
            free(html_buf);
            html_buf = NULL;
        }

        ESP_LOGI(TAG, "W%d: Saved %s (%lu bytes)", worker_id, sd_path, (unsigned long)total_read);
    }

    if(html_buf) {
        free(html_buf);
        html_buf = NULL;
    }

    furi_record_close(RECORD_STORAGE);
    ESP_LOGI(TAG, "Worker %d done", worker_id);
    vTaskSuspend(NULL);
}

// --- Public API ---

void wifi_crawler_start(WifiCrawlerState* state, const char* domain) {
    if(!state || !domain || !domain[0]) return;

    // Init state
    memset(state, 0, sizeof(WifiCrawlerState));
    strncpy(state->domain, domain, sizeof(state->domain) - 1);
    state->max_pages = CRAWLER_MAX_PAGES;
    state->running = true;
    s_state = state;

    // Init queue (dynamically allocated — ~52KB, goes to PSRAM)
    s_queue = malloc(sizeof(CrawlerQueue));
    if(!s_queue) {
        ESP_LOGE(TAG, "Cannot alloc crawler queue");
        state->running = false;
        return;
    }
    memset(s_queue, 0, sizeof(CrawlerQueue));
    s_queue->urls = malloc(QUEUE_CAPACITY * CRAWLER_MAX_URL_LEN);
    if(!s_queue->urls) {
        ESP_LOGE(TAG, "Cannot alloc URL buffer");
        free(s_queue);
        s_queue = NULL;
        state->running = false;
        return;
    }
    memset(s_queue->urls, 0, QUEUE_CAPACITY * CRAWLER_MAX_URL_LEN);
    s_queue->mutex = furi_mutex_alloc(FuriMutexTypeNormal);

    // Seed with start URL
    char start_url[CRAWLER_MAX_URL_LEN];
    snprintf(start_url, sizeof(start_url), "https://%s/", domain);
    queue_push(start_url);

    // Spawn workers
    for(int i = 0; i < CRAWLER_NUM_WORKERS; i++) {
        s_worker_stacks[i] = heap_caps_malloc(
            WORKER_STACK_SIZE * sizeof(StackType_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

        if(!s_worker_stacks[i]) {
            ESP_LOGE(TAG, "Cannot alloc stack for worker %d", i);
            continue;
        }

        char name[12];
        snprintf(name, sizeof(name), "CrawlW%d", i);
        s_workers[i] = xTaskCreateStaticPinnedToCore(
            crawler_worker_fn, name, WORKER_STACK_SIZE,
            (void*)(intptr_t)i, 4, s_worker_stacks[i], &s_worker_bufs[i], 0);
    }

    ESP_LOGI(TAG, "Crawler started for %s", domain);
}

void wifi_crawler_stop(WifiCrawlerState* state) {
    if(!state) return;
    state->running = false;

    // Wait for workers to finish
    for(int i = 0; i < CRAWLER_NUM_WORKERS; i++) {
        if(s_workers[i]) {
            // Give worker time to notice running=false
            for(int j = 0; j < 50; j++) {
                eTaskState ts = eTaskGetState(s_workers[i]);
                if(ts == eSuspended || ts == eDeleted) break;
                furi_delay_ms(100);
            }
            vTaskDelete(s_workers[i]);
            s_workers[i] = NULL;
        }
        if(s_worker_stacks[i]) {
            heap_caps_free(s_worker_stacks[i]);
            s_worker_stacks[i] = NULL;
        }
    }

    if(s_queue) {
        if(s_queue->mutex) {
            furi_mutex_free(s_queue->mutex);
        }
        if(s_queue->urls) {
            free(s_queue->urls);
        }
        free(s_queue);
        s_queue = NULL;
    }

    s_state = NULL;
    ESP_LOGI(TAG, "Crawler stopped. Pages: %lu, Images: %lu, Bytes: %lu",
             (unsigned long)state->page_count, (unsigned long)state->image_count,
             (unsigned long)state->total_bytes);
}
