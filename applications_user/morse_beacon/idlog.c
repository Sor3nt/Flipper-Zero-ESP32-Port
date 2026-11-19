#include "idlog.h"

#include <furi.h>
#include <furi_hal.h>
#include <storage/storage.h>
#include <stdarg.h>

#define MORSE_LOG_PATH     APP_DATA_PATH("id_log.txt")
#define MORSE_LOG_OLD_PATH APP_DATA_PATH("id_log.old.txt")

/* An interval beacon writes around a megabyte of log a day, so the file must
 * not grow without bound. Rotating beats trimming here: dropping old lines in
 * place would mean rewriting the whole file on the SD card for every append. */
#define MORSE_LOG_MAX_BYTES (256 * 1024)

void morse_log(const char* format, ...) {
    Storage* storage = furi_record_open(RECORD_STORAGE);

    FileInfo info;
    if(storage_common_stat(storage, MORSE_LOG_PATH, &info) == FSE_OK &&
       info.size >= MORSE_LOG_MAX_BYTES) {
        storage_common_remove(storage, MORSE_LOG_OLD_PATH);
        storage_common_rename(storage, MORSE_LOG_PATH, MORSE_LOG_OLD_PATH);
    }

    File* file = storage_file_alloc(storage);
    if(storage_file_open(file, MORSE_LOG_PATH, FSAM_WRITE, FSOM_OPEN_APPEND)) {
        DateTime dt;
        furi_hal_rtc_get_datetime(&dt);

        FuriString* line = furi_string_alloc();
        furi_string_printf(
            line,
            "%04u-%02u-%02u %02u:%02u:%02u ",
            dt.year,
            dt.month,
            dt.day,
            dt.hour,
            dt.minute,
            dt.second);

        FuriString* body = furi_string_alloc();
        va_list args;
        va_start(args, format);
        furi_string_vprintf(body, format, args);
        va_end(args);

        furi_string_cat(line, body);
        furi_string_cat(line, "\n");
        storage_file_write(file, furi_string_get_cstr(line), furi_string_size(line));

        furi_string_free(body);
        furi_string_free(line);
    }
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}
