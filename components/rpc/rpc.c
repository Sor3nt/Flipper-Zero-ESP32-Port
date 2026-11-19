/**
 * @file rpc.c
 * RPC Service — ESP32 port
 *
 * Based on STM32 rpc.c. CLI integration removed (no CLI service yet).
 * Only System and Storage subsystems enabled initially.
 */

#include "rpc_i.h"

#include <pb.h>
#include <pb_decode.h>
#include <pb_encode.h>

#include <storage.pb.h>
#include <flipper.pb.h>

#include <furi.h>
#include <furi_hal.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <furi_hal_qfdiag.h>
#include <momentum_settings.h>

#include <stdint.h>
#include <stdio.h>
#include <m-dict.h>

#include <bt/bt_service/bt.h>

#define TAG "RpcSrv"

/* Per-message trace (every decoded RPC message, every handler call). At INFO
 * these cost 2-3 log lines per 1 KB upload chunk, so they are DEBUG (compiled
 * out at CONFIG_LOG_MAXIMUM_LEVEL=INFO) unless QFDIAG is enabled, where they
 * are part of the captured transfer trace. */
#if QFDIAG_ENABLED
#define RPC_TRACE(...) ESP_LOGI(TAG, __VA_ARGS__)
#else
#define RPC_TRACE(...) ESP_LOGD(TAG, __VA_ARGS__)
#endif

typedef enum {
    RpcEvtNewData = (1 << 0),
    RpcEvtDisconnect = (1 << 1),
} RpcEvtFlags;

#define RPC_ALL_EVENTS (RpcEvtNewData | RpcEvtDisconnect)

DICT_DEF2(RpcHandlerDict, pb_size_t, M_DEFAULT_OPLIST, RpcHandler, M_POD_OPLIST)

typedef struct {
    RpcSystemAlloc alloc;
    RpcSystemFree free;
    void* context;
} RpcSystemCallbacks;

static RpcSystemCallbacks rpc_systems[] = {
    {
        .alloc = rpc_system_system_alloc,
        .free = NULL,
    },
    {
        .alloc = rpc_system_storage_alloc,
        .free = rpc_system_storage_free,
    },
    {
        .alloc = rpc_system_app_alloc,
        .free = rpc_system_app_free,
    },
    {
        .alloc = rpc_system_gui_alloc,
        .free = rpc_system_gui_free,
    },
    {
        .alloc = rpc_system_property_alloc,
        .free = NULL,
    },
};

struct RpcSession {
    Rpc* rpc;

    FuriThread* thread;

    RpcHandlerDict_t handlers;
    FuriStreamBuffer* stream;
    PB_Main* decoded_message;
    bool terminate;
    /* FIX [QF-3]: RPC sessions piled up (one status-bar RPC icon each) and
     *   qFlipper could not reconnect until power was removed.
     * ROOT CAUSE: rpc_session_close() only sets a thread flag that the worker
     *   cannot see while it is inside a handler, and every handler runs under
     *   the GLOBAL rpc->busy_mutex. A recursive delete of a big tree runs for
     *   minutes, so each new session blocked at its first command.
     * WARNING: set atomically by rpc_session_close(); long-running handlers
     *   must poll rpc_session_is_closing() and return early. Do not remove, and
     *   do not add a long handler that ignores it. */
    volatile bool closing;
    void** system_contexts;
    bool decode_error;

    FuriMutex* callbacks_mutex;
    RpcSendBytesCallback send_bytes_callback;
    RpcBufferIsEmptyCallback buffer_is_empty_callback;
    RpcSessionClosedCallback closed_callback;
    RpcSessionTerminatedCallback terminated_callback;
    RpcOwner owner;
    void* context;
};

struct Rpc {
    FuriMutex* busy_mutex;
};

/* QFDIAG: which message handler the worker is running, and since when. */
static volatile uint32_t rpc_dbg_handler_tag;
static volatile uint32_t rpc_dbg_handler_since_ms;

void rpc_debug_get_handler(uint32_t* tag, uint32_t* running_ms) {
    const uint32_t since = rpc_dbg_handler_since_ms;
    if(tag) *tag = since ? rpc_dbg_handler_tag : 0;
    if(running_ms) *running_ms = since ? furi_get_tick() - since : 0;
}

/* QFDIAG byte trace -------------------------------------------------------
 * Keeps two copies of the RX byte stream, indexed by absolute stream offset:
 * what the transport FED (rpc_session_feed) and what nanopb CONSUMED
 * (rpc_pb_stream_read). On a decode failure they are compared at the same
 * offsets: MATCH => the bytes were already wrong when they entered the stream
 * (USB / TinyUSB / bridge); MISMATCH => they changed inside the stream buffer.
 * The failing message is hexdumped so the corruption pattern (bytes lost,
 * duplicated, zeroed, a missing 64-byte USB packet, ...) is visible.
 * Diagnostic only; buffers are allocated only with CONFIG_FURI_QFDIAG. */
#define QD_PROD_SIZE 8192u
#define QD_CONS_SIZE 2048u
#define QD_FEEDLOG   64u
static uint8_t* qd_prod;
static uint8_t* qd_cons;
static volatile uint32_t qd_prod_off;
static volatile uint32_t qd_cons_off;
static uint32_t qd_msg_start;
static struct {
    uint32_t off;
    uint32_t len;
} qd_feedlog[QD_FEEDLOG];
static volatile uint32_t qd_feedlog_n;

static void qd_trace_reset(void) {
    if(!QFDIAG_ENABLED) return; /* buffers stay NULL: all trace paths no-op */
    if(!qd_prod) qd_prod = heap_caps_malloc(QD_PROD_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(!qd_cons) qd_cons = heap_caps_malloc(QD_CONS_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    qd_prod_off = 0;
    qd_cons_off = 0;
    qd_msg_start = 0;
    qd_feedlog_n = 0;
}

static void qd_ring_put(uint8_t* ring, uint32_t size, uint32_t off, const uint8_t* src, size_t len) {
    for(size_t i = 0; i < len; i++) {
        ring[(off + i) % size] = src[i];
    }
}

static void qd_hexdump(const char* label, const uint8_t* ring, uint32_t size, uint32_t from, uint32_t to) {
    for(uint32_t off = from; off < to; off += 32) {
        char line[32 * 3 + 1];
        size_t n = 0;
        for(uint32_t i = off; i < to && i < off + 32; i++) {
            n += snprintf(line + n, sizeof(line) - n, "%02x ", ring[i % size]);
        }
        line[n] = '\0';
        ESP_LOGE(TAG, "QD %s %06lx: %s", label, (unsigned long)off, line);
    }
}

static void qd_dump_decode_failure(const char* err) {
    if(!qd_prod || !qd_cons) return;
    const uint32_t cons = qd_cons_off;
    const uint32_t prod = qd_prod_off;
    uint32_t from = qd_msg_start > 64 ? qd_msg_start - 64 : 0;
    if(cons - from > QD_CONS_SIZE - 64) from = cons - (QD_CONS_SIZE - 64);

    /* Length prefix of the failing message (varint at msg_start). */
    uint32_t msg_len = 0;
    for(uint32_t i = 0, shift = 0; i < 5 && qd_msg_start + i < cons; i++, shift += 7) {
        const uint8_t b = qd_cons[(qd_msg_start + i) % QD_CONS_SIZE];
        msg_len |= (uint32_t)(b & 0x7F) << shift;
        if(!(b & 0x80)) break;
    }
    ESP_LOGE(
        TAG,
        "QD DECODE FAIL '%s': msg_start=%lu len=%lu fail_at=%lu (+%lu into msg) fed=%lu",
        err,
        (unsigned long)qd_msg_start,
        (unsigned long)msg_len,
        (unsigned long)cons,
        (unsigned long)(cons - qd_msg_start),
        (unsigned long)prod);

    if(prod - from <= QD_PROD_SIZE) {
        long mismatch = -1;
        for(uint32_t o = from; o < cons; o++) {
            if(qd_cons[o % QD_CONS_SIZE] != qd_prod[o % QD_PROD_SIZE]) {
                mismatch = (long)o;
                break;
            }
        }
        ESP_LOGE(
            TAG,
            "QD stream integrity %lu..%lu: %s (first mismatch %ld)",
            (unsigned long)from,
            (unsigned long)cons,
            mismatch < 0 ? "MATCH (bytes were bad on arrival)" : "MISMATCH (changed in stream buffer)",
            mismatch);
    }

    /* Transport feeds that delivered the bytes around the failure. */
    const uint32_t nfeed = qd_feedlog_n;
    const uint32_t first = nfeed > QD_FEEDLOG ? nfeed - QD_FEEDLOG : 0;
    for(uint32_t i = first; i < nfeed; i++) {
        const uint32_t off = qd_feedlog[i % QD_FEEDLOG].off;
        const uint32_t len = qd_feedlog[i % QD_FEEDLOG].len;
        if(off + len >= from && off <= cons + 256) {
            ESP_LOGE(TAG, "QD feed @%lu len %lu", (unsigned long)off, (unsigned long)len);
        }
    }

    qd_hexdump("cons", qd_cons, QD_CONS_SIZE, from, cons);
    const uint32_t next_end = (prod - cons > 160) ? cons + 160 : prod;
    if(prod - cons <= QD_PROD_SIZE) qd_hexdump("next", qd_prod, QD_PROD_SIZE, cons, next_end);
}

static bool rpc_owner_is_allowed(RpcOwner owner) {
    if(!furi_hal_rtc_is_flag_set(FuriHalRtcFlagLock)) return true;

    if(owner == RpcOwnerBle) {
        return momentum_settings_get_bool(MomentumSettingsBoolAllowLockedRpcBle);
    }
    if(owner == RpcOwnerUsb) {
        return momentum_settings_get_bool(MomentumSettingsBoolAllowLockedRpcUsb);
    }
    return true;
}

RpcOwner rpc_session_get_owner(RpcSession* session) {
    furi_check(session);
    return session->owner;
}

static void rpc_close_session_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(context);

    RpcSession* session = (RpcSession*)context;

    rpc_send_and_release_empty(session, request->command_id, PB_CommandStatus_OK);
    furi_mutex_acquire(session->callbacks_mutex, FuriWaitForever);
    if(session->closed_callback) {
        session->closed_callback(session->context);
    } else {
        FURI_LOG_W(TAG, "Session stop isn't processed by transport layer");
    }
    furi_mutex_release(session->callbacks_mutex);
}

void rpc_session_set_context(RpcSession* session, void* context) {
    furi_check(session);

    furi_mutex_acquire(session->callbacks_mutex, FuriWaitForever);
    session->context = context;
    furi_mutex_release(session->callbacks_mutex);
}

void rpc_session_set_close_callback(RpcSession* session, RpcSessionClosedCallback callback) {
    furi_check(session);

    furi_mutex_acquire(session->callbacks_mutex, FuriWaitForever);
    session->closed_callback = callback;
    furi_mutex_release(session->callbacks_mutex);
}

void rpc_session_set_send_bytes_callback(RpcSession* session, RpcSendBytesCallback callback) {
    furi_check(session);

    furi_mutex_acquire(session->callbacks_mutex, FuriWaitForever);
    session->send_bytes_callback = callback;
    furi_mutex_release(session->callbacks_mutex);
}

void rpc_session_set_buffer_is_empty_callback(
    RpcSession* session,
    RpcBufferIsEmptyCallback callback) {
    furi_check(session);

    furi_mutex_acquire(session->callbacks_mutex, FuriWaitForever);
    session->buffer_is_empty_callback = callback;
    furi_mutex_release(session->callbacks_mutex);
}

void rpc_session_set_terminated_callback(
    RpcSession* session,
    RpcSessionTerminatedCallback callback) {
    furi_check(session);

    furi_mutex_acquire(session->callbacks_mutex, FuriWaitForever);
    session->terminated_callback = callback;
    furi_mutex_release(session->callbacks_mutex);
}

size_t rpc_session_feed(
    RpcSession* session,
    const uint8_t* encoded_bytes,
    size_t size,
    uint32_t timeout) {
    furi_check(session);
    furi_check(encoded_bytes);

    if(!size) return 0;

    size_t bytes_sent = furi_stream_buffer_send(session->stream, encoded_bytes, size, timeout);

    if(qd_prod && bytes_sent) { /* QFDIAG */
        const uint32_t off = qd_prod_off;
        qd_ring_put(qd_prod, QD_PROD_SIZE, off, encoded_bytes, bytes_sent);
        const uint32_t n = qd_feedlog_n;
        qd_feedlog[n % QD_FEEDLOG].off = off;
        qd_feedlog[n % QD_FEEDLOG].len = (uint32_t)bytes_sent;
        qd_feedlog_n = n + 1;
        qd_prod_off = off + (uint32_t)bytes_sent;
    }

    furi_thread_flags_set(furi_thread_get_id(session->thread), RpcEvtNewData);

    return bytes_sent;
}

size_t rpc_session_get_available_size(RpcSession* session) {
    furi_check(session);
    return furi_stream_buffer_spaces_available(session->stream);
}

bool rpc_pb_stream_read(pb_istream_t* istream, pb_byte_t* buf, size_t count) {
    furi_assert(istream);
    furi_assert(buf);
    RpcSession* session = istream->state;
    furi_assert(session);
    furi_assert(istream->bytes_left);

    if(session->terminate) {
        return false;
    }

    uint32_t flags = 0;
    size_t bytes_received = 0;

    while(1) {
        const size_t qd_got = furi_stream_buffer_receive(
            session->stream, buf + bytes_received, count - bytes_received, 0);
        if(qd_cons && qd_got && !session->decode_error) { /* QFDIAG */
            qd_ring_put(qd_cons, QD_CONS_SIZE, qd_cons_off, buf + bytes_received, qd_got);
            qd_cons_off += (uint32_t)qd_got;
        }
        bytes_received += qd_got;
        if(furi_stream_buffer_is_empty(session->stream)) {
            if(session->buffer_is_empty_callback) {
                session->buffer_is_empty_callback(session->context);
            }
        }
        if(session->decode_error) {
            bytes_received = 0;
        }
        if(count == bytes_received) {
            break;
        } else {
            flags = furi_thread_flags_wait(RPC_ALL_EVENTS, FuriFlagWaitAny, FuriWaitForever);
            if(flags & RpcEvtDisconnect) {
                if(furi_stream_buffer_is_empty(session->stream)) {
                    session->terminate = true;
                    istream->bytes_left = 0;
                    bytes_received = 0;
                    break;
                } else {
                    furi_thread_flags_set(furi_thread_get_id(session->thread), RpcEvtDisconnect);
                }
            } else if(flags & RpcEvtNewData) {
                // Just wake thread up
            }
        }
    }

    return count == bytes_received;
}

static bool rpc_pb_content_callback(pb_istream_t* stream, const pb_field_t* field, void** arg) {
    furi_assert(stream);
    RpcSession* session = stream->state;
    furi_assert(session);
    furi_assert(field);

    RpcHandler* handler = RpcHandlerDict_get(session->handlers, field->tag);

    if(handler && handler->decode_submessage) {
        handler->decode_submessage(stream, field, arg);
    }

    return true;
}

static int32_t rpc_session_worker(void* context) {
    furi_assert(context);
    RpcSession* session = (RpcSession*)context;
    Rpc* rpc = session->rpc;

    FURI_LOG_D(TAG, "Session started");

    while(1) {
        pb_istream_t istream = {
            .callback = rpc_pb_stream_read,
            .state = session,
            .errmsg = NULL,
            .bytes_left = SIZE_MAX,
        };

        bool message_decode_failed = false;

        qd_msg_start = qd_cons_off; /* QFDIAG */
        if(pb_decode_ex(&istream, &PB_Main_msg, session->decoded_message, PB_DECODE_DELIMITED)) {
            RpcHandler* handler =
                RpcHandlerDict_get(session->handlers, session->decoded_message->which_content);

            RPC_TRACE("Decoded msg: cmd_id=%lu which=%lu has_next=%d",
                (unsigned long)session->decoded_message->command_id,
                (unsigned long)session->decoded_message->which_content,
                session->decoded_message->has_next);

            const bool owner_allowed = rpc_owner_is_allowed(session->owner);
            if(handler && handler->message_handler && owner_allowed) {
                RPC_TRACE("Handler found, executing...");
                const uint32_t qd_bw0 = QFDIAG_ENABLED ? furi_get_tick() : 0;
                furi_check(furi_mutex_acquire(rpc->busy_mutex, FuriWaitForever) == FuriStatusOk);
                uint32_t qd_h0 = 0;
                if(QFDIAG_ENABLED) { /* QFDIAG: lock wait + running handler */
                    const uint32_t qd_bw = furi_get_tick() - qd_bw0;
                    if(qd_bw > 100) ESP_LOGW(TAG, "QD busy_mutex wait %lu ms", (unsigned long)qd_bw);
                    qd_h0 = furi_get_tick();
                    rpc_dbg_handler_tag = session->decoded_message->which_content;
                    rpc_dbg_handler_since_ms = qd_h0 ? qd_h0 : 1;
                }
                handler->message_handler(session->decoded_message, handler->context);
                if(QFDIAG_ENABLED) { /* QFDIAG: slow handler */
                    rpc_dbg_handler_since_ms = 0;
                    const uint32_t qd_hd = furi_get_tick() - qd_h0;
                    if(qd_hd > 300) {
                        ESP_LOGW(
                            TAG,
                            "QD handler which=%lu took %lu ms",
                            (unsigned long)rpc_dbg_handler_tag,
                            (unsigned long)qd_hd);
                    }
                }
                furi_check(furi_mutex_release(rpc->busy_mutex) == FuriStatusOk);
            } else if(handler && handler->message_handler) {
                rpc_send_and_release_empty(
                    session, session->decoded_message->command_id, PB_CommandStatus_ERROR);
            } else if(session->decoded_message->which_content == 0) {
                ESP_LOGE(TAG, "which_content==0, treating as decode error");
                message_decode_failed = true;
            } else if(!handler && !session->terminate) {
                ESP_LOGW(TAG, "Message(%lu) decoded, but not implemented",
                    (unsigned long)session->decoded_message->which_content);
                rpc_send_and_release_empty(
                    session,
                    session->decoded_message->command_id,
                    PB_CommandStatus_ERROR_NOT_IMPLEMENTED);
            }
        } else {
            message_decode_failed = true;
        }

        if(message_decode_failed) {
            if(!session->terminate) qd_dump_decode_failure(PB_GET_ERROR(&istream)); /* QFDIAG */
            furi_stream_buffer_reset(session->stream);
            if(!session->terminate) {
                ESP_LOGE(TAG, "Decode FAILED: '%s'", PB_GET_ERROR(&istream));
                session->decode_error = true;
                rpc_send_and_release_empty(session, 0, PB_CommandStatus_ERROR_DECODE);
                furi_mutex_acquire(session->callbacks_mutex, FuriWaitForever);
                if(session->closed_callback) {
                    session->closed_callback(session->context);
                }
                furi_mutex_release(session->callbacks_mutex);

                if(session->owner == RpcOwnerBle) {
                    FURI_LOG_E(TAG, "BLE session closed due to a decode error");
                    Bt* bt = furi_record_open(RECORD_BT);
                    bt_profile_restore_default(bt);
                    furi_record_close(RECORD_BT);
                }
            }
        }

        pb_release(&PB_Main_msg, session->decoded_message);

        if(session->terminate) {
            FURI_LOG_D(TAG, "Session terminated");
            break;
        }
    }

    return 0;
}

static void rpc_session_thread_pending_callback(void* context, uint32_t arg) {
    UNUSED(arg);
    RpcSession* session = (RpcSession*)context;

    for(size_t i = 0; i < COUNT_OF(rpc_systems); ++i) {
        if(rpc_systems[i].free) {
            (rpc_systems[i].free)(session->system_contexts[i]);
        }
    }
    free(session->system_contexts);
    free(session->decoded_message);
    RpcHandlerDict_clear(session->handlers);
    furi_stream_buffer_free(session->stream);

    furi_mutex_acquire(session->callbacks_mutex, FuriWaitForever);
    if(session->terminated_callback) {
        session->terminated_callback(session->context);
    }
    furi_mutex_release(session->callbacks_mutex);

    furi_mutex_free(session->callbacks_mutex);
    furi_thread_join(session->thread);
    furi_thread_free(session->thread);
    free(session);
}

static void
    rpc_session_thread_state_callback(FuriThread* thread, FuriThreadState state, void* context) {
    UNUSED(thread);
    if(state == FuriThreadStateStopped) {
        furi_timer_pending_callback(rpc_session_thread_pending_callback, context, 0);
    }
}

RpcSession* rpc_session_open(Rpc* rpc, RpcOwner owner) {
    furi_check(rpc);

    if(!rpc_owner_is_allowed(owner)) {
        return NULL;
    }

    RpcSession* session = malloc(sizeof(RpcSession));
    session->callbacks_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    session->stream = furi_stream_buffer_alloc(RPC_BUFFER_SIZE, 1);
    session->rpc = rpc;
    session->terminate = false;
    session->closing = false;
    session->decode_error = false;
    qd_trace_reset(); /* QFDIAG */
    session->owner = owner;
    RpcHandlerDict_init(session->handlers);

    session->decoded_message = malloc(sizeof(PB_Main));
    session->decoded_message->cb_content.funcs.decode = rpc_pb_content_callback;
    session->decoded_message->cb_content.arg = session;

    session->system_contexts = malloc(COUNT_OF(rpc_systems) * sizeof(void*));
    for(size_t i = 0; i < COUNT_OF(rpc_systems); ++i) {
        session->system_contexts[i] = rpc_systems[i].alloc(session);
    }

    RpcHandler rpc_handler = {
        .message_handler = rpc_close_session_process,
        .decode_submessage = NULL,
        .context = session,
    };
    rpc_add_handler(session, PB_Main_stop_session_tag, &rpc_handler);

    /* 6 KB: devinfo/property-get formatiert ~50 Keys ueber FuriString/vsnprintf;
     * mit 4 KB lief der Worker beim ESP32-Port-Zusatz knapp an den Canary. */
    session->thread = furi_thread_alloc_ex("RpcSessionWorker", 6144, rpc_session_worker, session);

    furi_thread_set_state_context(session->thread, session);
    furi_thread_set_state_callback(session->thread, rpc_session_thread_state_callback);

    furi_thread_start(session->thread);

    return session;
}

void rpc_session_close(RpcSession* session) {
    furi_check(session);
    furi_check(session->rpc);

    /* FIX [QF-3]: publish "host gone" BEFORE anything else so a handler that is
     * running right now (e.g. recursive delete) stops and releases busy_mutex.
     * WARNING: keep this first; the thread flag below is only seen between
     * handlers. */
    __atomic_store_n(&session->closing, true, __ATOMIC_RELEASE);
    rpc_session_set_send_bytes_callback(session, NULL);
    rpc_session_set_close_callback(session, NULL);
    rpc_session_set_buffer_is_empty_callback(session, NULL);
    furi_thread_flags_set(furi_thread_get_id(session->thread), RpcEvtDisconnect);
}

bool rpc_session_is_closing(RpcSession* session) {
    furi_check(session);
    return __atomic_load_n(&session->closing, __ATOMIC_ACQUIRE);
}

void rpc_init(void) {
    Rpc* rpc = malloc(sizeof(Rpc));
    rpc->busy_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    furi_record_create(RECORD_RPC, rpc);
    FURI_LOG_I(TAG, "RPC service initialized");
}

void rpc_add_handler(RpcSession* session, pb_size_t message_tag, RpcHandler* handler) {
    furi_assert(RpcHandlerDict_get(session->handlers, message_tag) == NULL);

    RpcHandlerDict_set_at(session->handlers, message_tag, *handler);
}

void rpc_send(RpcSession* session, PB_Main* message) {
    furi_assert(session);
    furi_assert(message);

    pb_ostream_t ostream = PB_OSTREAM_SIZING;

    bool result = pb_encode_ex(&ostream, &PB_Main_msg, message, PB_ENCODE_DELIMITED);
    furi_check(result && ostream.bytes_written);

    uint8_t* buffer = malloc(ostream.bytes_written);
    ostream = pb_ostream_from_buffer(buffer, ostream.bytes_written);

    pb_encode_ex(&ostream, &PB_Main_msg, message, PB_ENCODE_DELIMITED);

    furi_mutex_acquire(session->callbacks_mutex, FuriWaitForever);
    if(session->send_bytes_callback) {
        session->send_bytes_callback(session->context, buffer, ostream.bytes_written);
    }
    furi_mutex_release(session->callbacks_mutex);

    free(buffer);
}

void rpc_send_and_release(RpcSession* session, PB_Main* message) {
    rpc_send(session, message);
    pb_release(&PB_Main_msg, message);
}

void rpc_send_and_release_empty(RpcSession* session, uint32_t command_id, PB_CommandStatus status) {
    furi_assert(session);

    PB_Main message = {
        .command_id = command_id,
        .command_status = status,
        .has_next = false,
        .which_content = PB_Main_empty_tag,
    };

    rpc_send_and_release(session, &message);
    pb_release(&PB_Main_msg, &message);
}
