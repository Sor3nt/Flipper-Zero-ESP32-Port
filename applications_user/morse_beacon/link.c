#include "link.h"

#include <furi.h>
#include <furi_hal.h>
#include <bt/bt_service/bt.h>
// PORT: this firmware's BLE serial profile header lives under ble_profile/extra_profiles.
#include <ble_profile/extra_profiles/serial_profile.h>
#include <string.h>

#include "idlog.h"

#define TAG "MorseLink"

#define LINK_RX_BUFFER 256
#define LINK_QUEUE_LEN 4

/* A BLE terminal writes a message as one packet with no line ending, so treat a
 * pause in the byte stream as an end-of-line too. */
#define LINK_IDLE_FLUSH_MS 400

typedef struct {
    char text[MORSE_MAX_TEXT + 1];
} LinkLine;

struct MorseLink {
    LinkSource source;
    volatile LinkState state;

    Bt* bt;
    FuriHalBleProfileBase* profile;
    // PORT: UART remote-text removed (this firmware has no furi_hal_serial API); BLE path only.

    FuriMessageQueue* queue;
    FuriThread* worker;
    volatile bool running;

    FuriMutex* mutex;
    char line[MORSE_MAX_TEXT + 1];
    size_t line_len;
    char last_line[MORSE_MAX_TEXT + 1];
    uint32_t rx_count;
    volatile uint32_t last_byte_tick;
    volatile bool connect_event;

    LinkLineCallback callback;
    void* context;
};

/* ---------------------------------------------------------- line assembly */

static void link_flush_line(MorseLink* link) {
    if(link->line_len == 0) return;

    LinkLine item;
    link->line[link->line_len] = '\0';
    strncpy(item.text, link->line, MORSE_MAX_TEXT);
    item.text[MORSE_MAX_TEXT] = '\0';

    strncpy(link->last_line, item.text, MORSE_MAX_TEXT);
    link->last_line[MORSE_MAX_TEXT] = '\0';
    link->rx_count++;
    link->line_len = 0;

    // drop the oldest if the sender is running ahead of the key
    if(furi_message_queue_get_space(link->queue) == 0) {
        LinkLine discard;
        furi_message_queue_get(link->queue, &discard, 0);
    }
    furi_message_queue_put(link->queue, &item, 0);
}

static void link_push_byte(MorseLink* link, uint8_t byte) {
    furi_mutex_acquire(link->mutex, FuriWaitForever);
    link->last_byte_tick = furi_get_tick();

    if(byte == '\n' || byte == '\r') {
        link_flush_line(link);
    } else if(byte >= 0x20 && byte < 0x7F) {
        if(link->line_len >= MORSE_MAX_TEXT) link_flush_line(link);
        link->line[link->line_len++] = (char)byte;
    }

    furi_mutex_release(link->mutex);
}

/* ------------------------------------------------------------------- BLE */

static uint16_t link_ble_callback(SerialServiceEvent event, void* context) {
    MorseLink* link = context;

    if(event.event == SerialServiceEventTypeDataReceived) {
        for(uint16_t i = 0; i < event.data.size; i++) {
            link_push_byte(link, event.data.buffer[i]);
        }
    }
    return LINK_RX_BUFFER;
}

static void link_bt_status_callback(BtStatus status, void* context) {
    MorseLink* link = context;

    if(status == BtStatusConnected) {
        /* The bt service opens an RPC session the instant a client connects and
         * installs its own serial callback over ours, so everything the phone
         * writes gets fed to the RPC protobuf decoder and dropped. Take the
         * link back. bt_close_rpc_connection() is not exported to apps, but
         * re-registering the callback is enough - the service dispatches to
         * whichever one is currently installed. */
        if(link->profile) {
            ble_profile_serial_set_rpc_active(link->profile, false);
            ble_profile_serial_set_event_callback(
                link->profile, LINK_RX_BUFFER, link_ble_callback, link);
        }
        link->connect_event = true;
        link->state = LinkStateConnected;
    } else if(status == BtStatusAdvertising) {
        link->state = LinkStateAdvertising;
    } else if(link->running) {
        link->state = LinkStateAdvertising;
    }
}

static bool link_start_ble(MorseLink* link) {
    link->bt = furi_record_open(RECORD_BT);
    bt_disconnect(link->bt);
    furi_delay_ms(200); // let the stack drop the old profile before swapping

    bt_set_status_changed_callback(link->bt, link_bt_status_callback, link);

    link->profile = bt_profile_start(link->bt, ble_profile_serial, NULL);
    if(!link->profile) {
        FURI_LOG_E(TAG, "failed to start the BLE serial profile");
        bt_set_status_changed_callback(link->bt, NULL, NULL);
        furi_record_close(RECORD_BT);
        link->bt = NULL;
        return false;
    }

    ble_profile_serial_set_event_callback(
        link->profile, LINK_RX_BUFFER, link_ble_callback, link);
    furi_hal_bt_start_advertising();

    morse_log("link: BLE serial profile up, advertising");
    link->state = LinkStateAdvertising;
    return true;
}

static void link_stop_ble(MorseLink* link) {
    if(!link->bt) return;
    furi_hal_bt_stop_advertising();
    bt_set_status_changed_callback(link->bt, NULL, NULL);
    bt_profile_restore_default(link->bt);
    furi_record_close(RECORD_BT);
    link->bt = NULL;
    link->profile = NULL;
}

/* ------------------------------------------------------------------ UART */
// PORT: the GPIO 13/14 UART remote-text path is removed. This firmware exposes no furi_hal_serial
// API (control_acquire/init/async_rx/tx), so the whole path is compiled out. The UI offers BLE only
// (see morse_beacon.c), and morse_link_start() rejects any non-BLE source defensively.

/* ---------------------------------------------------------------- worker */

static int32_t link_worker(void* context) {
    MorseLink* link = context;
    LinkLine item;

    while(link->running) {
        if(link->connect_event) {
            link->connect_event = false;
            morse_log("link: client connected, serial callback reclaimed from RPC");
        }

        if(furi_message_queue_get(link->queue, &item, 100) == FuriStatusOk) {
            morse_log("link: rx \"%s\"", item.text);
            if(link->callback) link->callback(item.text, link->context);
            // hand the flow-control credit back; must not be done from inside
            // the RX callback, which already holds the service mutex
            if(link->source == LinkSourceBle && link->profile) {
                ble_profile_serial_notify_buffer_is_empty(link->profile);
            }
            continue;
        }

        // nothing queued: flush a partial line that has gone quiet
        furi_mutex_acquire(link->mutex, FuriWaitForever);
        if(link->line_len > 0 &&
           furi_get_tick() - link->last_byte_tick >= LINK_IDLE_FLUSH_MS) {
            link_flush_line(link);
        }
        furi_mutex_release(link->mutex);
    }
    return 0;
}

/* ------------------------------------------------------------------- API */

MorseLink* morse_link_alloc(void) {
    MorseLink* link = malloc(sizeof(MorseLink));
    memset(link, 0, sizeof(MorseLink));
    link->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    link->queue = furi_message_queue_alloc(LINK_QUEUE_LEN, sizeof(LinkLine));
    link->state = LinkStateStopped;
    return link;
}

void morse_link_free(MorseLink* link) {
    furi_check(link);
    morse_link_stop(link);
    furi_message_queue_free(link->queue);
    furi_mutex_free(link->mutex);
    free(link);
}

void morse_link_set_callback(MorseLink* link, LinkLineCallback callback, void* context) {
    furi_check(link);
    link->callback = callback;
    link->context = context;
}

bool morse_link_start(MorseLink* link, LinkSource source, uint32_t baudrate) {
    furi_check(link);
    if(link->running) return true;

    link->source = source;
    link->line_len = 0;
    link->rx_count = 0;
    link->last_line[0] = '\0';
    furi_message_queue_reset(link->queue);

    UNUSED(baudrate);
    // PORT: only the BLE serial source is supported on this firmware (no UART HAL).
    if(source != LinkSourceBle) {
        FURI_LOG_W(TAG, "UART remote-text is not available on this firmware; use BLE");
        link->state = LinkStateError;
        return false;
    }
    bool ok = link_start_ble(link);
    if(!ok) {
        link->state = LinkStateError;
        return false;
    }

    link->running = true;
    link->worker = furi_thread_alloc_ex("MorseLink", 4096, link_worker, link);
    furi_thread_start(link->worker);
    return true;
}

void morse_link_stop(MorseLink* link) {
    furi_check(link);
    if(!link->running) {
        link->state = LinkStateStopped;
        return;
    }

    link->running = false;
    furi_thread_join(link->worker);
    furi_thread_free(link->worker);
    link->worker = NULL;

    link_stop_ble(link); // PORT: BLE is the only supported source
    link->state = LinkStateStopped;
}

LinkState morse_link_state(MorseLink* link) {
    return link ? link->state : LinkStateStopped;
}

uint32_t morse_link_rx_count(MorseLink* link) {
    return link ? link->rx_count : 0;
}

void morse_link_last_line(MorseLink* link, char* out, size_t size) {
    furi_check(link);
    furi_mutex_acquire(link->mutex, FuriWaitForever);
    strncpy(out, link->last_line, size - 1);
    out[size - 1] = '\0';
    furi_mutex_release(link->mutex);
}

void morse_link_reply(MorseLink* link, const char* text) {
    furi_check(link);
    if(!link->running) return;

    size_t len = strlen(text);
    // PORT: BLE reply only (UART path removed).
    if(link->profile && link->state == LinkStateConnected) {
        ble_profile_serial_tx(link->profile, (uint8_t*)text, (uint16_t)len);
    }
}
