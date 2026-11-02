#include "bw16_app_i.h"
#include <stdlib.h>
#include <string.h>

/* Runs in the UART worker thread. It NEVER calls the view dispatcher — it only mutates
 * shared state under app->mutex and raises flags the GUI scenes poll from their tick
 * handler. Keep it allocation-light and non-blocking. */

static void bw16_trim(const char* in, char* out, size_t out_size) {
    /* Copy `in` into `out`, dropping leading spaces and a trailing CR/space. */
    while(*in == ' ') in++;
    size_t n = strlen(in);
    while(n > 0 && (in[n - 1] == ' ' || in[n - 1] == '\r')) n--;
    if(n >= out_size) n = out_size - 1;
    memcpy(out, in, n);
    out[n] = '\0';
}

static void bw16_parse_station(Bw16App* app, const char* payload) {
    /* payload = "<ssid>,<channel>,<rssi>" (SSID may itself contain commas). */
    if(app->ap_count >= BW16_MAX_AP) return;

    const char* last = strrchr(payload, ',');
    if(!last) return;
    /* find the comma before `last` */
    const char* prev = NULL;
    for(const char* p = payload; p < last; p++) {
        if(*p == ',') prev = p;
    }
    if(!prev) return;

    Bw16ApEntry* ap = &app->aps[app->ap_count];

    size_t ssid_len = (size_t)(prev - payload);
    if(ssid_len >= BW16_SSID_LEN) ssid_len = BW16_SSID_LEN - 1;
    memcpy(ap->ssid, payload, ssid_len);
    ap->ssid[ssid_len] = '\0';
    if(ap->ssid[0] == '\0') strcpy(ap->ssid, "<hidden>");

    ap->channel = (int)strtol(prev + 1, NULL, 10);
    ap->rssi = (int)strtol(last + 1, NULL, 10);

    app->ap_count++;
}

/* Called with app->mutex held. */
static void bw16_process_line(Bw16App* app, const char* line) {
    if(app->capture_mode == Bw16CaptureAp &&
       strncmp(line, "list-station:", strlen("list-station:")) == 0) {
        char payload[BW16_SSID_LEN + 16];
        bw16_trim(line + strlen("list-station:"), payload, sizeof(payload));
        bw16_parse_station(app, payload);
        return;
    }

    if(app->capture_mode == Bw16CaptureClient &&
       strncmp(line, "list-client:", strlen("list-client:")) == 0) {
        if(app->client_count < BW16_MAX_CLIENT) {
            bw16_trim(
                line + strlen("list-client:"),
                app->clients[app->client_count],
                BW16_MAC_STR_LEN);
            if(app->clients[app->client_count][0] == '\0') {
                snprintf(
                    app->clients[app->client_count],
                    BW16_MAC_STR_LEN,
                    "client %u",
                    (unsigned)app->client_count);
            }
            app->client_count++;
        }
        return;
    }

    if(strcmp(line, "LIST_STATION_DONE") == 0) {
        app->capture_mode = Bw16CaptureNone;
        app->ap_list_done = true;
        return;
    }

    if(strcmp(line, "LIST_CLIENTS_DONE") == 0) {
        app->capture_mode = Bw16CaptureNone;
        app->client_list_done = true;
        return;
    }
}

void bw16_rx_handle_data(uint8_t* buf, size_t len, void* context) {
    furi_assert(context);
    Bw16App* app = context;

    furi_mutex_acquire(app->mutex, FuriWaitForever);

    /* 1) Mirror everything into the console producer buffer. Capacity is reserved up
     *    front and the length is capped below it, so no realloc happens here. */
    app->text_box_store_strlen += len;
    if(app->text_box_store_strlen >= BW16_TEXT_BOX_STORE_SIZE - 1) {
        furi_string_right(app->text_box_store, app->text_box_store_strlen / 2);
        app->text_box_store_strlen = furi_string_size(app->text_box_store) + len;
    }
    buf[len] = '\0';
    furi_string_cat_printf(app->text_box_store, "%s", buf);
    app->console_dirty = true;

    /* 2) Re-assemble complete lines and parse them for list/marker handling. */
    for(size_t i = 0; i < len; i++) {
        char c = (char)buf[i];
        if(c == '\n') {
            bw16_process_line(app, furi_string_get_cstr(app->rx_line));
            furi_string_reset(app->rx_line);
        } else if(c != '\r') {
            furi_string_push_back(app->rx_line, c);
            /* Guard against an unterminated flood of bytes. */
            if(furi_string_size(app->rx_line) > 160) {
                furi_string_reset(app->rx_line);
            }
        }
    }

    furi_mutex_release(app->mutex);
}
