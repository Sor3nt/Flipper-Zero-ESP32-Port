#include <furi.h>
#include "u2f_hid.h"
#include "u2f.h"
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <lib/toolbox/args.h>
#include <furi_hal_usb_hid_u2f.h>
#include <storage/storage.h>

#define TAG "U2fHid"

#define WORKER_TAG TAG "Worker"

#define U2F_HID_MAX_PAYLOAD_LEN ((HID_U2F_PACKET_LEN - 7) + 128 * (HID_U2F_PACKET_LEN - 5))

#define U2F_HID_TYPE_MASK 0x80 // Frame type mask
#define U2F_HID_TYPE_INIT 0x80 // Initial frame identifier
#define U2F_HID_TYPE_CONT 0x00 // Continuation frame identifier

#define U2F_HID_PING  (U2F_HID_TYPE_INIT | 0x01) // Echo data through local processor only
#define U2F_HID_MSG   (U2F_HID_TYPE_INIT | 0x03) // Send U2F message frame
#define U2F_HID_LOCK  (U2F_HID_TYPE_INIT | 0x04) // Send lock channel command
#define U2F_HID_INIT  (U2F_HID_TYPE_INIT | 0x06) // Channel initialization
#define U2F_HID_WINK  (U2F_HID_TYPE_INIT | 0x08) // Send device identification wink
#define U2F_HID_ERROR (U2F_HID_TYPE_INIT | 0x3f) // Error response

#define U2F_HID_ERR_NONE          0x00 // No error
#define U2F_HID_ERR_INVALID_CMD   0x01 // Invalid command
#define U2F_HID_ERR_INVALID_PAR   0x02 // Invalid parameter
#define U2F_HID_ERR_INVALID_LEN   0x03 // Invalid message length
#define U2F_HID_ERR_INVALID_SEQ   0x04 // Invalid message sequencing
#define U2F_HID_ERR_MSG_TIMEOUT   0x05 // Message has timed out
#define U2F_HID_ERR_CHANNEL_BUSY  0x06 // Channel busy
#define U2F_HID_ERR_LOCK_REQUIRED 0x0a // Command requires channel lock
#define U2F_HID_ERR_SYNC_FAIL     0x0b // SYNC command failed
#define U2F_HID_ERR_OTHER         0x7f // Other unspecified error

#define U2F_HID_BROADCAST_CID 0xFFFFFFFF

typedef enum {
    WorkerEvtReserved = (1 << 0),
    WorkerEvtStop = (1 << 1),
    WorkerEvtConnect = (1 << 2),
    WorkerEvtDisconnect = (1 << 3),
    WorkerEvtRequest = (1 << 4),
    WorkerEvtUnlock = (1 << 5),
} WorkerEvtFlags;

struct U2fHid_packet {
    uint32_t cid;
    uint16_t len;
    uint8_t cmd;
    uint8_t payload[U2F_HID_MAX_PAYLOAD_LEN];
};

struct U2fHid {
    FuriThread* thread;
    FuriTimer* lock_timer;
    FuriHalUsbInterface* usb_mode_prev;
    uint8_t seq_id_last;
    uint16_t req_buf_ptr;
    uint32_t req_len_left;
    uint32_t lock_cid;
    uint32_t lock_expiry_tick;
    bool lock;
    U2fData* u2f_instance;
    struct U2fHid_packet packet;
};

static void u2f_hid_event_callback(HidU2fEvent ev, void* context) {
    furi_assert(context);
    U2fHid* u2f_hid = context;

    if(ev == HidU2fDisconnected) {
        furi_thread_flags_set(furi_thread_get_id(u2f_hid->thread), WorkerEvtDisconnect);
    } else if(ev == HidU2fConnected) {
        furi_thread_flags_set(furi_thread_get_id(u2f_hid->thread), WorkerEvtConnect);
    } else if(ev == HidU2fRequest) {
        furi_thread_flags_set(furi_thread_get_id(u2f_hid->thread), WorkerEvtRequest);
    }
}

static void u2f_hid_lock_timeout_callback(void* context) {
    furi_assert(context);
    U2fHid* u2f_hid = context;

    furi_thread_flags_set(furi_thread_get_id(u2f_hid->thread), WorkerEvtUnlock);
}

static bool u2f_hid_send_packet(const U2fHid* u2f_hid, const uint8_t* packet_buf) {
    if(furi_hal_hid_u2f_send_response(packet_buf, HID_U2F_PACKET_LEN)) return true;
    FURI_LOG_W(
        WORKER_TAG,
        "Response packet dropped cid=%lX cmd=%x",
        u2f_hid->packet.cid,
        u2f_hid->packet.cmd);
    return false;
}

static bool u2f_hid_send_response(U2fHid* u2f_hid) {
    uint8_t packet_buf[HID_U2F_PACKET_LEN];
    uint16_t len_remain = u2f_hid->packet.len;
    uint8_t len_cur = 0;
    uint8_t seq_cnt = 0;
    uint16_t data_ptr = 0;

    memset(packet_buf, 0, HID_U2F_PACKET_LEN);
    memcpy(packet_buf, &(u2f_hid->packet.cid), sizeof(uint32_t)); //-V1086

    // Init packet
    packet_buf[4] = u2f_hid->packet.cmd;
    packet_buf[5] = u2f_hid->packet.len >> 8;
    packet_buf[6] = (u2f_hid->packet.len & 0xFF);
    len_cur = (len_remain < (HID_U2F_PACKET_LEN - 7)) ? (len_remain) : (HID_U2F_PACKET_LEN - 7);
    if(len_cur > 0) memcpy(&packet_buf[7], u2f_hid->packet.payload, len_cur);
    if(!u2f_hid_send_packet(u2f_hid, packet_buf)) return false;
    data_ptr = len_cur;
    len_remain -= len_cur;

    // Continuation packets
    while(len_remain > 0) {
        memset(&packet_buf[4], 0, HID_U2F_PACKET_LEN - 4);
        packet_buf[4] = seq_cnt;
        len_cur = (len_remain < (HID_U2F_PACKET_LEN - 5)) ? (len_remain) :
                                                            (HID_U2F_PACKET_LEN - 5);
        memcpy(&packet_buf[5], &(u2f_hid->packet.payload[data_ptr]), len_cur);
        if(!u2f_hid_send_packet(u2f_hid, packet_buf)) return false;
        seq_cnt++;
        len_remain -= len_cur;
        data_ptr += len_cur;
    }
    return true;
}

static void u2f_hid_send_error(U2fHid* u2f_hid, uint8_t error) {
    u2f_hid->packet.len = 1;
    u2f_hid->packet.cmd = U2F_HID_ERROR;
    u2f_hid->packet.payload[0] = error;
    u2f_hid_send_response(u2f_hid);
}

static bool u2f_hid_parse_request(U2fHid* u2f_hid) {
    FURI_LOG_D(
        WORKER_TAG,
        "Req cid=%lX cmd=%x len=%u",
        u2f_hid->packet.cid,
        u2f_hid->packet.cmd,
        u2f_hid->packet.len);

    if(u2f_hid->lock && u2f_hid->packet.cid != u2f_hid->lock_cid) {
        u2f_hid_send_error(u2f_hid, U2F_HID_ERR_CHANNEL_BUSY);
        return true;
    }

    if(u2f_hid->packet.cmd == U2F_HID_PING) { // PING - echo request back
        u2f_hid_send_response(u2f_hid);

    } else if(u2f_hid->packet.cmd == U2F_HID_MSG) { // MSG - U2F message
        uint16_t resp_len =
            u2f_msg_parse(u2f_hid->u2f_instance, u2f_hid->packet.payload, u2f_hid->packet.len);
        if(resp_len > 0) {
            u2f_hid->packet.len = resp_len;
            u2f_hid_send_response(u2f_hid);
        } else
            return false;

    } else if(u2f_hid->packet.cmd == U2F_HID_LOCK) { // LOCK - lock all channels except current
        if(u2f_hid->packet.len != 1) {
            u2f_hid_send_error(u2f_hid, U2F_HID_ERR_INVALID_LEN);
            return true;
        }
        uint8_t lock_timeout = u2f_hid->packet.payload[0];
        if(lock_timeout > 10) {
            u2f_hid_send_error(u2f_hid, U2F_HID_ERR_INVALID_PAR);
            return true;
        }
        if(u2f_hid->lock && u2f_hid->lock_cid != u2f_hid->packet.cid) {
            u2f_hid_send_error(u2f_hid, U2F_HID_ERR_CHANNEL_BUSY);
            return true;
        }
        if(lock_timeout == 0) { // Lock off
            furi_timer_stop(u2f_hid->lock_timer);
            u2f_hid->lock = false;
            u2f_hid->lock_cid = 0;
            u2f_hid->lock_expiry_tick = 0;
        } else { // Lock on
            u2f_hid->lock = true;
            u2f_hid->lock_cid = u2f_hid->packet.cid;
            uint32_t timeout_ticks = furi_ms_to_ticks((uint32_t)lock_timeout * 1000U);
            u2f_hid->lock_expiry_tick = furi_get_tick() + timeout_ticks;
            furi_timer_start(u2f_hid->lock_timer, timeout_ticks);
        }
        u2f_hid->packet.len = 0;
        u2f_hid_send_response(u2f_hid);

    } else if(u2f_hid->packet.cmd == U2F_HID_INIT) { // INIT - channel initialization request
        if(u2f_hid->packet.len != 8) {
            u2f_hid_send_error(u2f_hid, U2F_HID_ERR_INVALID_LEN);
            return true;
        }
        // CID 0 is reserved. Broadcast INIT allocates a new channel; INIT on an already
        // allocated channel is a resync and is answered with that same CID.
        if(u2f_hid->packet.cid == 0) return false;
        uint32_t channel_cid = u2f_hid->packet.cid;
        if(channel_cid == U2F_HID_BROADCAST_CID) {
            do {
                channel_cid = furi_hal_random_get();
            } while(channel_cid == 0 || channel_cid == U2F_HID_BROADCAST_CID);
        }
        u2f_hid->packet.len = 17;
        memcpy(&(u2f_hid->packet.payload[8]), &channel_cid, sizeof(uint32_t)); //-V1086
        u2f_hid->packet.payload[12] = 2; // Protocol version
        u2f_hid->packet.payload[13] = 1; // Device version major
        u2f_hid->packet.payload[14] = 0; // Device version minor
        u2f_hid->packet.payload[15] = 1; // Device build version
        u2f_hid->packet.payload[16] = 1; // Capabilities: wink
        u2f_hid_send_response(u2f_hid);

    } else if(u2f_hid->packet.cmd == U2F_HID_WINK) { // WINK - notify user
        if(u2f_hid->packet.len != 0) return false;
        u2f_wink(u2f_hid->u2f_instance);
        u2f_hid->packet.len = 0; //-V1048
        u2f_hid_send_response(u2f_hid);
    } else
        return false;
    return true;
}

static int32_t u2f_hid_worker(void* context) {
    U2fHid* u2f_hid = context;
    uint8_t packet_buf[HID_U2F_PACKET_LEN];

    FURI_LOG_D(WORKER_TAG, "Init");

    furi_hal_hid_u2f_set_callback(u2f_hid_event_callback, u2f_hid);

    while(1) {
        uint32_t flags = furi_thread_flags_wait(
            WorkerEvtStop | WorkerEvtConnect | WorkerEvtDisconnect | WorkerEvtRequest |
                WorkerEvtUnlock,
            FuriFlagWaitAny,
            FuriWaitForever);
        furi_check(!(flags & FuriFlagError));
        if(flags & WorkerEvtStop) break;
        if(flags & (WorkerEvtConnect | WorkerEvtDisconnect)) {
            bool connected = furi_hal_usb_u2f_backend_is_mounted();
            if(!connected) {
                u2f_hid->req_len_left = 0;
                u2f_hid->req_buf_ptr = 0;
                u2f_hid->seq_id_last = 0;
                u2f_hid->lock = false;
                u2f_hid->lock_cid = 0;
                u2f_hid->lock_expiry_tick = 0;
                furi_timer_stop(u2f_hid->lock_timer);
            }
            u2f_set_state(u2f_hid->u2f_instance, connected);
            FURI_LOG_D(WORKER_TAG, "%s", connected ? "Connect" : "Disconnect");
        }
        if(flags & WorkerEvtUnlock) {
            if(u2f_hid->lock) {
                int32_t remaining =
                    (int32_t)(u2f_hid->lock_expiry_tick - furi_get_tick());
                if(remaining <= 0) {
                    u2f_hid->lock = false;
                    u2f_hid->lock_cid = 0;
                    u2f_hid->lock_expiry_tick = 0;
                } else {
                    furi_timer_start(u2f_hid->lock_timer, (uint32_t)remaining);
                }
            }
        }
        if(flags & WorkerEvtRequest) {
            uint32_t len_cur;
            while((len_cur = furi_hal_hid_u2f_get_request(packet_buf)) != 0) {
                bool request_complete = false;
                if((packet_buf[4] & U2F_HID_TYPE_MASK) == U2F_HID_TYPE_INIT) {
                    if(len_cur < 7) {
                        u2f_hid->req_len_left = 0;
                        memcpy(&(u2f_hid->packet.cid), packet_buf, sizeof(uint32_t));
                        u2f_hid_send_error(u2f_hid, U2F_HID_ERR_INVALID_LEN);
                        continue;
                    }
                    // Init packet
                    u2f_hid->packet.len = (packet_buf[5] << 8) | (packet_buf[6]);
                    memcpy(&(u2f_hid->packet.cid), packet_buf, 4);
                    if(u2f_hid->packet.len > U2F_HID_MAX_PAYLOAD_LEN) {
                        u2f_hid->req_len_left = 0;
                        u2f_hid_send_error(u2f_hid, U2F_HID_ERR_INVALID_LEN);
                        continue;
                    }
                    if(u2f_hid->packet.len > (len_cur - 7)) {
                        u2f_hid->req_len_left = u2f_hid->packet.len - (len_cur - 7);
                        len_cur = len_cur - 7;
                    } else {
                        u2f_hid->req_len_left = 0;
                        len_cur = u2f_hid->packet.len;
                    }
                    u2f_hid->packet.cmd = packet_buf[4];
                    u2f_hid->seq_id_last = 0;
                    u2f_hid->req_buf_ptr = len_cur;
                    if(len_cur > 0) memcpy(u2f_hid->packet.payload, &packet_buf[7], len_cur);
                    request_complete = u2f_hid->req_len_left == 0;
                } else {
                    if(len_cur < 5) {
                        u2f_hid->req_len_left = 0;
                        memcpy(&(u2f_hid->packet.cid), packet_buf, sizeof(uint32_t));
                        u2f_hid_send_error(u2f_hid, U2F_HID_ERR_INVALID_LEN);
                        continue;
                    }
                    // Continuation packet
                    uint32_t cid_temp = 0;
                    memcpy(&cid_temp, packet_buf, sizeof(cid_temp));
                    if(u2f_hid->req_len_left == 0 || cid_temp != u2f_hid->packet.cid ||
                       packet_buf[4] != u2f_hid->seq_id_last) {
                        u2f_hid->req_len_left = 0;
                        u2f_hid->packet.cid = cid_temp;
                        u2f_hid_send_error(u2f_hid, U2F_HID_ERR_INVALID_SEQ);
                        continue;
                    }

                    uint32_t payload_len = len_cur - 5;
                    if(u2f_hid->req_len_left > payload_len) {
                        len_cur = payload_len;
                        u2f_hid->req_len_left -= payload_len;
                    } else {
                        len_cur = u2f_hid->req_len_left;
                        u2f_hid->req_len_left = 0;
                    }
                    memcpy(
                        &(u2f_hid->packet.payload[u2f_hid->req_buf_ptr]),
                        &packet_buf[5],
                        len_cur);
                    u2f_hid->req_buf_ptr += len_cur;
                    u2f_hid->seq_id_last++;
                    request_complete = u2f_hid->req_len_left == 0;
                }
                if(request_complete) {
                    if(u2f_hid_parse_request(u2f_hid) == false) {
                        u2f_hid_send_error(u2f_hid, U2F_HID_ERR_INVALID_CMD);
                    }
                }
            }
        }
    }
    furi_timer_stop(u2f_hid->lock_timer);
    furi_timer_free(u2f_hid->lock_timer);

    furi_hal_hid_u2f_set_callback(NULL, NULL);
    if(!furi_hal_usb_set_config(u2f_hid->usb_mode_prev, NULL)) {
        FURI_LOG_E(WORKER_TAG, "Unable to restore the previous USB configuration");
    }
    FURI_LOG_D(WORKER_TAG, "End");

    return 0;
}

U2fHid* u2f_hid_start(U2fData* u2f_inst) {
    U2fHid* u2f_hid = malloc(sizeof(U2fHid));
    if(!u2f_hid) return NULL;
    memset(u2f_hid, 0, sizeof(U2fHid));

    u2f_hid->u2f_instance = u2f_inst;
    u2f_hid->usb_mode_prev = furi_hal_usb_get_config();
    u2f_hid->lock_timer =
        furi_timer_alloc(u2f_hid_lock_timeout_callback, FuriTimerTypeOnce, u2f_hid);
    u2f_hid->thread =
        furi_thread_alloc_ex("U2fHidWorker", 8 * 1024, u2f_hid_worker, u2f_hid);
    if(!u2f_hid->lock_timer || !u2f_hid->thread) {
        if(u2f_hid->lock_timer) furi_timer_free(u2f_hid->lock_timer);
        if(u2f_hid->thread) furi_thread_free(u2f_hid->thread);
        free(u2f_hid);
        return NULL;
    }

    if(!furi_hal_usb_set_config(&usb_hid_u2f, NULL)) {
        furi_timer_free(u2f_hid->lock_timer);
        furi_thread_free(u2f_hid->thread);
        free(u2f_hid);
        return NULL;
    }
    furi_thread_start(u2f_hid->thread);
    return u2f_hid;
}

void u2f_hid_stop(U2fHid* u2f_hid) {
    furi_assert(u2f_hid);
    furi_thread_flags_set(furi_thread_get_id(u2f_hid->thread), WorkerEvtStop);
    furi_thread_join(u2f_hid->thread);
    u2f_shutdown(u2f_hid->u2f_instance);
    furi_thread_free(u2f_hid->thread);
    free(u2f_hid);
}
