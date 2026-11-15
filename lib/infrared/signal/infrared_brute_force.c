#include "infrared_brute_force.h"

#include <stdlib.h>
#include <m-dict.h>
#include <m-array.h>
#include <flipper_format/flipper_format.h>
#include <storage/storage.h>
#include <esp_heap_caps.h>

#include "infrared_signal.h"

#define TAG "IrBruteForce"

/*
 * Universal remote databases (tv.ir, ac.ir, fans.ir, ...) are 60-260 KB of
 * text. Parsing them straight from the SD card is slow: the buffered stream
 * only caches 1 KB, every RAW signal makes flipper_format seek back to count
 * its timings (which drops the cache and re-reads from SD), and every SD
 * access takes the SPI bus shared with the display.
 *
 * Instead, the whole database is read once, sequentially, into RAM (PSRAM
 * when present) and both indexing and transmitting are done from that
 * read-only copy. If the buffer cannot be allocated the old SD-backed path
 * is used, so behaviour on boards without PSRAM is unchanged.
 */
#define IR_BF_RAM_DB_MAX_SIZE          (1024U * 1024U)
#define IR_BF_RAM_DB_READ_CHUNK        (8U * 1024U) // short SPI bus lock per read
#define IR_BF_RAM_DB_INTERNAL_MAX_SIZE (64U * 1024U)
#define IR_BF_RAM_DB_INTERNAL_RESERVE  (96U * 1024U) // keep this much internal heap free

ARRAY_DEF(SignalPositionArray, size_t, M_DEFAULT_OPLIST); //-V658

typedef struct {
    size_t index;
    SignalPositionArray_t signals;
} InfraredBruteForceRecord;

static inline void ir_bf_record_init(InfraredBruteForceRecord* record) {
    record->index = 0;
    SignalPositionArray_init(record->signals);
}
#define IR_BF_RECORD_INIT(r) (ir_bf_record_init(&(r)))

static inline void
    ir_bf_record_init_set(InfraredBruteForceRecord* dest, const InfraredBruteForceRecord* src) {
    dest->index = src->index;
    SignalPositionArray_init_set(dest->signals, src->signals);
}
#define IR_BF_RECORD_INIT_SET(d, s) (ir_bf_record_init_set(&(d), &(s)))

static inline void
    ir_bf_record_set(InfraredBruteForceRecord* dest, const InfraredBruteForceRecord* src) {
    dest->index = src->index;
    SignalPositionArray_set(dest->signals, src->signals);
}
#define IR_BF_RECORD_SET(d, s) (ir_bf_record_set(&(d), &(s)))

static inline void ir_bf_record_clear(InfraredBruteForceRecord* record) {
    SignalPositionArray_clear(record->signals);
}
#define IR_BF_RECORD_CLEAR(r) (ir_bf_record_clear(&(r)))

#define IR_BF_RECORD_OPLIST           \
    (INIT(IR_BF_RECORD_INIT),         \
     INIT_SET(IR_BF_RECORD_INIT_SET), \
     SET(IR_BF_RECORD_SET),           \
     CLEAR(IR_BF_RECORD_CLEAR))

DICT_DEF2(
    InfraredBruteForceRecordDict,
    FuriString*,
    FURI_STRING_OPLIST,
    InfraredBruteForceRecord,
    IR_BF_RECORD_OPLIST);

struct InfraredBruteForce {
    FlipperFormat* ff;
    const char* db_filename;
    FuriString* current_record_name;
    InfraredBruteForceRecord current_record;
    InfraredSignal* current_signal;
    InfraredBruteForceRecordDict_t records;
    bool is_started;
    // Read-only RAM copy of the database file, NULL when the SD path is used.
    // Written only by calculate_messages() (worker thread, before the owner
    // joins it) and freed only while not started, so it is never accessed
    // concurrently and never changes while signals are being sent.
    uint8_t* db_data;
    size_t db_size;
};

static void* ir_bf_db_buffer_alloc(size_t size) {
    void* buffer = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(!buffer && size <= IR_BF_RAM_DB_INTERNAL_MAX_SIZE) {
        // No PSRAM: only use internal RAM for small files with plenty of headroom
        const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
        if(heap_caps_get_largest_free_block(caps) >= size &&
           heap_caps_get_free_size(caps) >= size + IR_BF_RAM_DB_INTERNAL_RESERVE) {
            buffer = heap_caps_malloc(size, caps);
        }
    }
    return buffer;
}

static void ir_bf_db_free(InfraredBruteForce* brute_force) {
    if(brute_force->db_data) {
        heap_caps_free(brute_force->db_data);
        brute_force->db_data = NULL;
    }
    brute_force->db_size = 0;
}

// Read the whole database into RAM. On any failure nothing is kept.
static bool ir_bf_db_load(InfraredBruteForce* brute_force, Storage* storage) {
    bool success = false;
    uint8_t* data = NULL;
    size_t size = 0;
    File* file = storage_file_alloc(storage);

    do {
        if(!storage_file_open(file, brute_force->db_filename, FSAM_READ, FSOM_OPEN_EXISTING))
            break;

        const uint64_t file_size = storage_file_size(file);
        if(file_size == 0 || file_size > IR_BF_RAM_DB_MAX_SIZE) break;
        size = (size_t)file_size;

        data = ir_bf_db_buffer_alloc(size);
        if(!data) break;

        size_t total = 0;
        while(total < size) {
            const size_t chunk = MIN(size - total, (size_t)IR_BF_RAM_DB_READ_CHUNK);
            const size_t read = storage_file_read(file, data + total, chunk);
            total += read;
            if(read != chunk) break;
        }
        if(total != size) break;

        success = true;
    } while(false);

    storage_file_close(file);
    storage_file_free(file);

    if(success) {
        brute_force->db_data = data;
        brute_force->db_size = size;
    } else if(data) {
        heap_caps_free(data);
    }

    return success;
}

InfraredBruteForce* infrared_brute_force_alloc(void) {
    InfraredBruteForce* brute_force = malloc(sizeof(InfraredBruteForce));
    brute_force->ff = NULL;
    brute_force->db_filename = NULL;
    brute_force->current_signal = NULL;
    brute_force->is_started = false;
    brute_force->db_data = NULL;
    brute_force->db_size = 0;
    brute_force->current_record_name = furi_string_alloc();
    InfraredBruteForceRecordDict_init(brute_force->records);
    return brute_force;
}

void infrared_brute_force_free(InfraredBruteForce* brute_force) {
    furi_check(brute_force);
    furi_assert(!brute_force->is_started);
    ir_bf_db_free(brute_force);
    InfraredBruteForceRecordDict_clear(brute_force->records);
    furi_string_free(brute_force->current_record_name);
    free(brute_force);
}

void infrared_brute_force_set_db_filename(InfraredBruteForce* brute_force, const char* db_filename) {
    furi_check(brute_force);
    furi_assert(!brute_force->is_started);
    // A RAM copy of another database must never be used for this one
    ir_bf_db_free(brute_force);
    brute_force->db_filename = db_filename;
}

InfraredErrorCode infrared_brute_force_calculate_messages(InfraredBruteForce* brute_force) {
    furi_check(brute_force);
    furi_assert(!brute_force->is_started);
    furi_assert(brute_force->db_filename);
    InfraredErrorCode error = InfraredErrorCodeNone;

    Storage* storage = furi_record_open(RECORD_STORAGE);

    ir_bf_db_free(brute_force);
    const bool in_ram = ir_bf_db_load(brute_force, storage);
    FlipperFormat* ff = NULL;
    if(in_ram) {
        ff = flipper_format_memory_alloc(brute_force->db_data, brute_force->db_size);
    } else {
        FURI_LOG_W(TAG, "Database not loaded to RAM, reading from SD");
        ff = flipper_format_buffered_file_alloc(storage);
    }

    FuriString* signal_name = furi_string_alloc();
    InfraredSignal* signal = infrared_signal_alloc();

    do {
        if(!in_ram && !flipper_format_buffered_file_open_existing(ff, brute_force->db_filename)) {
            error = InfraredErrorCodeFileOperationFailed;
            break;
        }

        bool signal_valid = false;
        while(infrared_signal_read_name(ff, signal_name) == InfraredErrorCodeNone) {
            size_t signal_start = flipper_format_tell(ff);
            error = infrared_signal_read_body(signal, ff);
            signal_valid = (!INFRARED_ERROR_PRESENT(error)) && infrared_signal_is_valid(signal);
            if(!signal_valid) break;

            InfraredBruteForceRecord* record =
                InfraredBruteForceRecordDict_get(brute_force->records, signal_name);
            // Skip button names the current remote layout does not use
            // (furi_assert is compiled out in release, this used to be a NULL deref)
            if(record) SignalPositionArray_push_back(record->signals, signal_start);
        }
        if(!signal_valid) break;
    } while(false);

    infrared_signal_free(signal);
    furi_string_free(signal_name);

    flipper_format_free(ff);
    if(INFRARED_ERROR_PRESENT(error)) ir_bf_db_free(brute_force);
    furi_record_close(RECORD_STORAGE);
    return error;
}

bool infrared_brute_force_start(
    InfraredBruteForce* brute_force,
    uint32_t index,
    uint32_t* record_count) {
    furi_check(brute_force);
    furi_assert(!brute_force->is_started);
    bool success = false;
    *record_count = 0;

    InfraredBruteForceRecordDict_it_t it;
    for(InfraredBruteForceRecordDict_it(it, brute_force->records);
        !InfraredBruteForceRecordDict_end_p(it);
        InfraredBruteForceRecordDict_next(it)) {
        const InfraredBruteForceRecordDict_itref_t* record = InfraredBruteForceRecordDict_cref(it);
        if(record->value.index == index) {
            *record_count = SignalPositionArray_size(record->value.signals);
            if(*record_count) {
                furi_string_set(brute_force->current_record_name, record->key);
                brute_force->current_record = record->value;
            }
            break;
        }
    }

    if(*record_count) {
        Storage* storage = furi_record_open(RECORD_STORAGE);
        brute_force->current_signal = infrared_signal_alloc();
        brute_force->is_started = true;
        if(brute_force->db_data) {
            // Same bytes the offsets were computed from, no SD access while sending
            brute_force->ff =
                flipper_format_memory_alloc(brute_force->db_data, brute_force->db_size);
            success = true;
        } else {
            brute_force->ff = flipper_format_buffered_file_alloc(storage);
            success = flipper_format_buffered_file_open_existing(
                brute_force->ff, brute_force->db_filename);
        }
        if(!success) infrared_brute_force_stop(brute_force);
    }
    return success;
}

bool infrared_brute_force_is_started(const InfraredBruteForce* brute_force) {
    furi_check(brute_force);
    return brute_force->is_started;
}

void infrared_brute_force_stop(InfraredBruteForce* brute_force) {
    furi_check(brute_force);
    furi_assert(brute_force->is_started);
    furi_string_reset(brute_force->current_record_name);
    infrared_signal_free(brute_force->current_signal);
    flipper_format_free(brute_force->ff);
    brute_force->current_signal = NULL;
    brute_force->ff = NULL;
    brute_force->is_started = false;
    furi_record_close(RECORD_STORAGE);
}

bool infrared_brute_force_send(InfraredBruteForce* brute_force, uint32_t signal_index) {
    furi_check(brute_force);
    furi_assert(brute_force->is_started);

    if(signal_index >= SignalPositionArray_size(brute_force->current_record.signals)) return false;

    size_t signal_start =
        *SignalPositionArray_cget(brute_force->current_record.signals, signal_index);
    if(!flipper_format_seek(brute_force->ff, signal_start, FlipperFormatOffsetFromStart))
        return false;

    if(INFRARED_ERROR_PRESENT(
           infrared_signal_read_body(brute_force->current_signal, brute_force->ff)))
        return false;

    infrared_signal_transmit(brute_force->current_signal);
    return true;
}

void infrared_brute_force_add_record(
    InfraredBruteForce* brute_force,
    uint32_t index,
    const char* name) {
    InfraredBruteForceRecord value;
    ir_bf_record_init(&value);
    value.index = index;
    FuriString* key;
    key = furi_string_alloc_set(name);
    InfraredBruteForceRecordDict_set_at(brute_force->records, key, value);
    furi_string_free(key);
}

void infrared_brute_force_reset(InfraredBruteForce* brute_force) {
    furi_assert(!brute_force->is_started);
    InfraredBruteForceRecordDict_reset(brute_force->records);
    ir_bf_db_free(brute_force);
}
