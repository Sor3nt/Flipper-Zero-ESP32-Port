#include "infrared_brute_force.h"

#include <stdlib.h>
#include <string.h>
#include <m-dict.h>
#include <m-array.h>
#include <flipper_format/flipper_format.h>
#include <storage/storage.h>
#include <esp_heap_caps.h>
#include <esp_rom_crc.h>

#include "infrared_signal.h"

#define TAG "IrBruteForce"

/*
 * Universal remote databases (tv.ir, ac.ir, fans.ir, ...) are up to ~330 KB
 * of text. Parsing them straight from the SD card is slow: the buffered
 * stream only caches 1 KB, every RAW signal makes flipper_format seek back to
 * count its timings (which drops the cache and re-reads from SD), and every
 * stream call is a round trip to the storage service.
 *
 * When PSRAM is available the whole database is copied into it once and both
 * indexing and transmitting are done from that read-only copy. Integrity:
 *
 *  - the copy must be exactly as long as the file, nothing may follow it, and
 *    it is compared byte-for-byte against a second read of the file;
 *  - every "name:" line in the file must yield one parsed signal, so a parser
 *    stop in the middle of the file is an error instead of silently dropping
 *    the remaining signals;
 *  - every indexed signal stores the CRC32 of the exact bytes the parser
 *    consumed for it; the CRC is checked before each transmission and a
 *    mismatching signal is re-read from the SD card (never sent corrupted);
 *  - I/O failures are retried; data that still fails is reported as an error.
 *
 * Without PSRAM (or for files over the limit) the original SD-backed path is
 * used, with the same "name:" completeness check and retries.
 *
 * Signal transmission itself (infrared_signal_read_body +
 * infrared_signal_transmit) is unchanged.
 */
#define IR_BF_DB_MAX_RAM_SIZE   (1024U * 1024U)
#define IR_BF_DB_IO_CHUNK       (8U * 1024U) // bytes per storage request
#define IR_BF_DB_ATTEMPTS       (5U)
#define IR_BF_DB_RETRY_DELAY_MS (50U)

typedef struct {
    size_t start; // offset of the signal body, passed to flipper_format_seek
    size_t length; // bytes [start, start + length) examined by the parser (RAM mode)
    uint32_t crc; // CRC32 of those bytes (RAM mode)
} InfraredBruteForceSignal;

ARRAY_DEF(SignalPositionArray, InfraredBruteForceSignal, M_POD_OPLIST); //-V658

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
    // Read-only PSRAM copy of the database file, NULL when the SD path is used.
    // Filled only by calculate_messages() (the owner joins that thread before
    // touching the instance again) and freed only while not started. While
    // started it is only accessed by the thread calling send().
    uint8_t* db_data;
    size_t db_size;
};

typedef enum {
    IrBfLoadOk, // verified copy in RAM
    IrBfLoadNoRam, // too large or no PSRAM: use the SD path
    IrBfLoadIoError, // open/read/verify failed: retry
} IrBfLoadResult;

/* ---- "name:" line counter (independent completeness check) ---- */

typedef struct {
    size_t count;
    uint8_t matched;
    bool at_line_start;
} IrBfNameCounter;

static void ir_bf_name_counter_init(IrBfNameCounter* counter) {
    counter->count = 0;
    counter->matched = 0;
    counter->at_line_start = true;
}

// Counts lines that start with "name:", i.e. exactly the lines the
// flipper_format key reader reports as key "name" ('\r' is ignored there too,
// '#' comment lines and indented keys never match).
static void ir_bf_name_counter_feed(IrBfNameCounter* counter, const uint8_t* data, size_t size) {
    static const char key[] = "name:";
    for(size_t i = 0; i < size; i++) {
        const uint8_t c = data[i];
        if(c == '\n') {
            counter->at_line_start = true;
            counter->matched = 0;
        } else if(c == '\r') {
            // ignored, same as the flipper_format key reader
        } else if(counter->at_line_start) {
            if(c == (uint8_t)key[counter->matched]) {
                counter->matched++;
                if(counter->matched == sizeof(key) - 1) {
                    counter->count++;
                    counter->at_line_start = false;
                    counter->matched = 0;
                }
            } else {
                counter->at_line_start = false;
                counter->matched = 0;
            }
        }
    }
}

/* ---- File helpers ---- */

static bool ir_bf_file_read_exact(File* file, uint8_t* dst, size_t size) {
    size_t total = 0;
    while(total < size) {
        const size_t chunk = MIN(size - total, (size_t)IR_BF_DB_IO_CHUNK);
        const size_t read = storage_file_read(file, dst + total, chunk);
        if(read != chunk || storage_file_get_error(file) != FSE_OK) return false;
        total += read;
    }
    return true;
}

// Internal RAM first: SD transfers then need no extra staging copy
static uint8_t* ir_bf_internal_alloc(size_t size) {
    uint8_t* buf = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if(!buf) buf = malloc(size);
    return buf;
}

/* ---- RAM copy ---- */

static void ir_bf_db_free(InfraredBruteForce* brute_force) {
    if(brute_force->db_data) {
        heap_caps_free(brute_force->db_data);
        brute_force->db_data = NULL;
    }
    brute_force->db_size = 0;
}

// One attempt to copy the database into PSRAM and prove the copy is identical
// to the file. Nothing is kept unless the result is IrBfLoadOk.
static IrBfLoadResult ir_bf_db_load_once(
    InfraredBruteForce* brute_force,
    Storage* storage,
    uint8_t* chunk_buf,
    uint32_t* file_crc) {
    IrBfLoadResult result = IrBfLoadIoError;
    uint8_t* data = NULL;
    size_t size = 0;
    File* file = storage_file_alloc(storage);

    do {
        if(!storage_file_open(file, brute_force->db_filename, FSAM_READ, FSOM_OPEN_EXISTING)) {
            FURI_LOG_E(TAG, "Cannot open %s", brute_force->db_filename);
            break;
        }

        const uint64_t file_size = storage_file_size(file);
        if(storage_file_get_error(file) != FSE_OK) break;
        if(file_size == 0 || file_size > IR_BF_DB_MAX_RAM_SIZE) {
            result = IrBfLoadNoRam;
            break;
        }
        size = (size_t)file_size;

        data = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if(!data) {
            result = IrBfLoadNoRam;
            break;
        }

        // Pass 1: copy every byte of the file
        if(!ir_bf_file_read_exact(file, data, size)) {
            FURI_LOG_E(TAG, "Short read");
            break;
        }

        // Pass 2: read the file again, compare byte-for-byte and take the
        // CRC32 of the bytes as read from the card (not from PSRAM)
        if(!storage_file_seek(file, 0, true)) break;
        bool identical = true;
        uint32_t crc = 0;
        size_t offset = 0;
        while(offset < size) {
            const size_t chunk = MIN(size - offset, (size_t)IR_BF_DB_IO_CHUNK);
            const size_t read = storage_file_read(file, chunk_buf, chunk);
            if(read != chunk || storage_file_get_error(file) != FSE_OK ||
               memcmp(chunk_buf, data + offset, chunk) != 0) {
                identical = false;
                break;
            }
            crc = esp_rom_crc32_le(crc, chunk_buf, chunk);
            offset += chunk;
        }
        if(!identical) {
            FURI_LOG_E(TAG, "Verify mismatch at %u", (unsigned)offset);
            break;
        }
        *file_crc = crc;

        result = IrBfLoadOk;
    } while(false);

    storage_file_free(file); // closes the file if open

    // The directory entry must still report the size that was copied
    // (catches the file being rewritten through another handle meanwhile)
    if(result == IrBfLoadOk) {
        FileInfo info;
        if(storage_common_stat(storage, brute_force->db_filename, &info) != FSE_OK ||
           info.size != (uint64_t)size) {
            FURI_LOG_E(TAG, "File changed while loading");
            result = IrBfLoadIoError;
        }
    }

    if(result == IrBfLoadOk) {
        brute_force->db_data = data;
        brute_force->db_size = size;
    } else if(data) {
        heap_caps_free(data);
    }

    return result;
}

static bool ir_bf_count_names_in_file(
    InfraredBruteForce* brute_force,
    Storage* storage,
    uint8_t* chunk_buf,
    size_t* count) {
    bool success = false;
    IrBfNameCounter counter;
    ir_bf_name_counter_init(&counter);
    File* file = storage_file_alloc(storage);

    if(storage_file_open(file, brute_force->db_filename, FSAM_READ, FSOM_OPEN_EXISTING)) {
        while(true) {
            const size_t read = storage_file_read(file, chunk_buf, IR_BF_DB_IO_CHUNK);
            if(storage_file_get_error(file) != FSE_OK) break;
            ir_bf_name_counter_feed(&counter, chunk_buf, read);
            if(read < IR_BF_DB_IO_CHUNK) {
                success = true;
                break;
            }
        }
    }

    storage_file_free(file);
    *count = counter.count;
    return success;
}

/* ---- Index ---- */

static void ir_bf_clear_signals(InfraredBruteForce* brute_force) {
    InfraredBruteForceRecordDict_it_t it;
    for(InfraredBruteForceRecordDict_it(it, brute_force->records);
        !InfraredBruteForceRecordDict_end_p(it);
        InfraredBruteForceRecordDict_next(it)) {
        InfraredBruteForceRecordDict_itref_t* record = InfraredBruteForceRecordDict_ref(it);
        SignalPositionArray_reset(record->value.signals);
    }
}

static FlipperFormat* ir_bf_ff_open(InfraredBruteForce* brute_force, Storage* storage) {
    if(brute_force->db_data) {
        return flipper_format_memory_alloc(brute_force->db_data, brute_force->db_size);
    }
    FlipperFormat* ff = flipper_format_buffered_file_alloc(storage);
    if(!flipper_format_buffered_file_open_existing(ff, brute_force->db_filename)) {
        flipper_format_free(ff);
        ff = NULL;
    }
    return ff;
}

// Parse every signal of the opened database and index the ones whose name is
// a registered record. *parsed counts every signal read, *indexed the kept ones.
static InfraredErrorCode ir_bf_index(
    InfraredBruteForce* brute_force,
    FlipperFormat* ff,
    size_t* parsed,
    size_t* indexed) {
    InfraredErrorCode error = InfraredErrorCodeNone;
    FuriString* signal_name = furi_string_alloc();
    InfraredSignal* signal = infrared_signal_alloc();
    *parsed = 0;
    *indexed = 0;

    while(infrared_signal_read_name(ff, signal_name) == InfraredErrorCodeNone) {
        const size_t signal_start = flipper_format_tell(ff);
        error = infrared_signal_read_body(signal, ff);
        if(INFRARED_ERROR_PRESENT(error)) break;
        if(!infrared_signal_is_valid(signal)) {
            // The original code broke out here with error == None, silently
            // dropping this and every following signal. Report it instead.
            error = infrared_signal_is_raw(signal) ? InfraredErrorCodeSignalRawUnableToReadData :
                                                     InfraredErrorCodeSignalMessageIsInvalid;
            break;
        }
        (*parsed)++;

        InfraredBruteForceSignal entry = {.start = signal_start, .length = 0, .crc = 0};
        if(brute_force->db_data) {
            // The parser stopped on the byte at signal_end (the line terminator)
            // after examining it, so [start, signal_end] is what decided the result.
            const size_t signal_end = flipper_format_tell(ff);
            if(signal_end < signal_start || signal_end > brute_force->db_size) {
                error = InfraredErrorCodeFileOperationFailed;
                break;
            }
            const size_t stop = (signal_end < brute_force->db_size) ? signal_end + 1 :
                                                                      brute_force->db_size;
            entry.length = stop - signal_start;
            entry.crc = esp_rom_crc32_le(0, brute_force->db_data + signal_start, entry.length);
        }

        InfraredBruteForceRecord* record =
            InfraredBruteForceRecordDict_get(brute_force->records, signal_name);
        // Signals of buttons that are not registered (other buttons when the
        // CLI sends a single one) are not indexed; every occurrence of a
        // registered button is, duplicates included.
        if(record) {
            SignalPositionArray_push_back(record->signals, entry);
            (*indexed)++;
        }
    }

    infrared_signal_free(signal);
    furi_string_free(signal_name);
    return error;
}

// One full attempt: load (or choose SD), parse, check completeness.
// *retry tells whether repeating can help (I/O-dependent failure).
static InfraredErrorCode ir_bf_calculate_once(
    InfraredBruteForce* brute_force,
    Storage* storage,
    uint8_t* chunk_buf,
    bool* retry) {
    *retry = true;
    ir_bf_db_free(brute_force);
    ir_bf_clear_signals(brute_force);

    uint32_t file_crc = 0;
    const IrBfLoadResult load = ir_bf_db_load_once(brute_force, storage, chunk_buf, &file_crc);
    if(load == IrBfLoadIoError) return InfraredErrorCodeFileOperationFailed;
    if(load == IrBfLoadNoRam) FURI_LOG_W(TAG, "Database not copied to PSRAM, reading from SD");
    const bool in_ram = (brute_force->db_data != NULL);

    size_t name_lines = 0;
    if(in_ram) {
        IrBfNameCounter counter;
        ir_bf_name_counter_init(&counter);
        ir_bf_name_counter_feed(&counter, brute_force->db_data, brute_force->db_size);
        name_lines = counter.count;
    } else if(!ir_bf_count_names_in_file(brute_force, storage, chunk_buf, &name_lines)) {
        return InfraredErrorCodeFileOperationFailed;
    }

    FlipperFormat* ff = ir_bf_ff_open(brute_force, storage);
    if(!ff) return InfraredErrorCodeFileOperationFailed;

    size_t parsed = 0;
    size_t indexed = 0;
    InfraredErrorCode error = ir_bf_index(brute_force, ff, &parsed, &indexed);
    flipper_format_free(ff);

    // A verified RAM copy parses the same way every time: retrying cannot
    // change a parse result. On the SD path a read glitch might, so retry.
    if(INFRARED_ERROR_PRESENT(error)) {
        *retry = !in_ram;
        return error;
    }

    // The per-signal CRCs were taken from PSRAM while parsing. The whole copy
    // must still match the CRC of the bytes read from the card, otherwise a
    // change after the verify pass could have been locked into the index.
    if(in_ram && esp_rom_crc32_le(0, brute_force->db_data, brute_force->db_size) != file_crc) {
        FURI_LOG_E(TAG, "PSRAM copy changed while indexing");
        return InfraredErrorCodeFileOperationFailed; // *retry stays true
    }

    if(parsed != name_lines) {
        FURI_LOG_E(
            TAG,
            "%s: %u name lines but %u signals parsed",
            brute_force->db_filename,
            (unsigned)name_lines,
            (unsigned)parsed);
        *retry = !in_ram;
        return InfraredErrorCodeSignalNameNotFound;
    }

    FURI_LOG_I(
        TAG,
        "%s: %u signals, %u indexed, %s",
        brute_force->db_filename,
        (unsigned)parsed,
        (unsigned)indexed,
        in_ram ? "PSRAM copy verified" : "SD");
    return InfraredErrorCodeNone;
}

/* ---- Per-signal integrity check before transmit (RAM mode) ---- */

static bool ir_bf_signal_restore(InfraredBruteForce* brute_force, const InfraredBruteForceSignal* s) {
    bool restored = false;
    uint8_t* buf = ir_bf_internal_alloc(s->length);
    if(!buf) return false;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    for(uint32_t attempt = 0; attempt < IR_BF_DB_ATTEMPTS && !restored; attempt++) {
        if(attempt) furi_delay_ms(IR_BF_DB_RETRY_DELAY_MS);
        File* file = storage_file_alloc(storage);
        bool ok = storage_file_open(file, brute_force->db_filename, FSAM_READ, FSOM_OPEN_EXISTING) &&
                  storage_file_seek(file, s->start, true);
        if(ok) {
            ok = (storage_file_read(file, buf, s->length) == s->length) &&
                 (storage_file_get_error(file) == FSE_OK);
        }
        storage_file_free(file);
        // Only accept bytes identical to what was indexed
        if(ok && esp_rom_crc32_le(0, buf, s->length) == s->crc) {
            memcpy(brute_force->db_data + s->start, buf, s->length);
            restored = true;
        }
    }
    furi_record_close(RECORD_STORAGE);
    free(buf);
    return restored;
}

static bool ir_bf_signal_verify(InfraredBruteForce* brute_force, const InfraredBruteForceSignal* s) {
    if(s->start > brute_force->db_size || s->length > brute_force->db_size - s->start) return false;
    if(esp_rom_crc32_le(0, brute_force->db_data + s->start, s->length) == s->crc) return true;

    FURI_LOG_E(TAG, "RAM copy of signal @%u corrupted, re-reading from SD", (unsigned)s->start);
    if(ir_bf_signal_restore(brute_force, s)) return true;
    FURI_LOG_E(TAG, "Signal @%u could not be restored, not sent", (unsigned)s->start);
    return false;
}

/* ---- Public API ---- */

InfraredBruteForce* infrared_brute_force_alloc(void) {
    InfraredBruteForce* brute_force = malloc(sizeof(InfraredBruteForce));
    brute_force->ff = NULL;
    brute_force->db_filename = NULL;
    brute_force->current_signal = NULL;
    brute_force->is_started = false;
    brute_force->db_data = NULL;
    brute_force->db_size = 0;
    brute_force->current_record.index = 0;
    SignalPositionArray_init(brute_force->current_record.signals);
    brute_force->current_record_name = furi_string_alloc();
    InfraredBruteForceRecordDict_init(brute_force->records);
    return brute_force;
}

void infrared_brute_force_free(InfraredBruteForce* brute_force) {
    furi_check(brute_force);
    infrared_brute_force_stop(brute_force); // no-op when not started
    ir_bf_db_free(brute_force);
    InfraredBruteForceRecordDict_clear(brute_force->records);
    furi_string_free(brute_force->current_record_name);
    free(brute_force);
}

void infrared_brute_force_set_db_filename(InfraredBruteForce* brute_force, const char* db_filename) {
    furi_check(brute_force);
    furi_check(!brute_force->is_started);
    // Neither the RAM copy nor the offsets of another database may be used
    // for this one: calculate_messages() must run again.
    ir_bf_db_free(brute_force);
    ir_bf_clear_signals(brute_force);
    brute_force->db_filename = db_filename;
}

InfraredErrorCode infrared_brute_force_calculate_messages(InfraredBruteForce* brute_force) {
    furi_check(brute_force);
    furi_check(!brute_force->is_started);
    furi_check(brute_force->db_filename);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    uint8_t* chunk_buf = ir_bf_internal_alloc(IR_BF_DB_IO_CHUNK);
    InfraredErrorCode error = InfraredErrorCodeFileOperationFailed;

    for(uint32_t attempt = 1; chunk_buf && attempt <= IR_BF_DB_ATTEMPTS; attempt++) {
        bool retry = false;
        error = ir_bf_calculate_once(brute_force, storage, chunk_buf, &retry);
        if(!INFRARED_ERROR_PRESENT(error) || !retry) break;
        FURI_LOG_W(
            TAG, "Attempt %u/%u failed (0x%08X)", (unsigned)attempt, IR_BF_DB_ATTEMPTS, (unsigned)error);
        if(attempt < IR_BF_DB_ATTEMPTS) furi_delay_ms(IR_BF_DB_RETRY_DELAY_MS);
    }

    if(INFRARED_ERROR_PRESENT(error)) {
        // Never leave a partial index or an unverified copy behind
        ir_bf_db_free(brute_force);
        ir_bf_clear_signals(brute_force);
    }

    free(chunk_buf);
    furi_record_close(RECORD_STORAGE);
    return error;
}

bool infrared_brute_force_start(
    InfraredBruteForce* brute_force,
    uint32_t index,
    uint32_t* record_count) {
    furi_check(brute_force);
    furi_check(!brute_force->is_started);
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
                // Shallow copy: the records dict is not modified while started
                brute_force->current_record = record->value;
            }
            break;
        }
    }

    if(*record_count) {
        Storage* storage = furi_record_open(RECORD_STORAGE);
        brute_force->ff = ir_bf_ff_open(brute_force, storage);
        if(brute_force->ff) {
            brute_force->current_signal = infrared_signal_alloc();
            brute_force->is_started = true;
            success = true;
        } else {
            furi_string_reset(brute_force->current_record_name);
            furi_record_close(RECORD_STORAGE);
        }
    }
    return success;
}

bool infrared_brute_force_is_started(const InfraredBruteForce* brute_force) {
    furi_check(brute_force);
    return brute_force->is_started;
}

uint32_t infrared_brute_force_get_signal_count(const InfraredBruteForce* brute_force) {
    furi_check(brute_force);
    if(!brute_force->is_started) return 0;
    return SignalPositionArray_size(brute_force->current_record.signals);
}

void infrared_brute_force_stop(InfraredBruteForce* brute_force) {
    furi_check(brute_force);
    // Idempotent: callers (e.g. the CLI) may stop after a failed start
    if(!brute_force->is_started) return;
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
    if(!brute_force->is_started) return false;

    if(signal_index >= SignalPositionArray_size(brute_force->current_record.signals)) return false;

    const InfraredBruteForceSignal* entry =
        SignalPositionArray_cget(brute_force->current_record.signals, signal_index);

    // RAM mode: the bytes must still be exactly the ones that were indexed
    if(brute_force->db_data && !ir_bf_signal_verify(brute_force, entry)) return false;

    if(!flipper_format_seek(brute_force->ff, entry->start, FlipperFormatOffsetFromStart))
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
    furi_check(brute_force);
    // Replacing a record would free the array current_record points into
    furi_check(!brute_force->is_started);
    InfraredBruteForceRecord value;
    ir_bf_record_init(&value);
    value.index = index;
    FuriString* key;
    key = furi_string_alloc_set(name);
    InfraredBruteForceRecordDict_set_at(brute_force->records, key, value);
    furi_string_free(key);
    ir_bf_record_clear(&value);
}

void infrared_brute_force_reset(InfraredBruteForce* brute_force) {
    furi_check(brute_force);
    infrared_brute_force_stop(brute_force); // no-op when not started
    InfraredBruteForceRecordDict_reset(brute_force->records);
    ir_bf_db_free(brute_force);
}
