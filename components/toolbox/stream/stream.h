#pragma once

#include <stdarg.h>
#include <storage/storage.h>
#include <furi.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Stream Stream;

typedef enum {
    StreamOffsetFromCurrent,
    StreamOffsetFromStart,
    StreamOffsetFromEnd,
} StreamOffset;

typedef enum {
    StreamDirectionForward,
    StreamDirectionBackward,
} StreamDirection;

typedef bool (*StreamWriteCB)(Stream* stream, const void* context);

void stream_free(Stream* stream);
void stream_clean(Stream* stream);
bool stream_eof(Stream* stream);
bool stream_seek(Stream* stream, int32_t offset, StreamOffset offset_type);
bool stream_seek_to_char(Stream* stream, char c, StreamDirection direction);
size_t stream_tell(Stream* stream);
size_t stream_size(Stream* stream);
size_t stream_write(Stream* stream, const uint8_t* data, size_t size);
size_t stream_read(Stream* stream, uint8_t* data, size_t count);
bool stream_delete_and_insert(
    Stream* stream,
    size_t delete_size,
    StreamWriteCB write_callback,
    const void* context);
bool stream_read_line(Stream* stream, FuriString* str_result);
bool stream_rewind(Stream* stream);
size_t stream_write_char(Stream* stream, char c);
size_t stream_write_string(Stream* stream, FuriString* string);
size_t stream_write_cstring(Stream* stream, const char* string);
size_t stream_write_format(Stream* stream, const char* format, ...)
    _ATTRIBUTE((__format__(__printf__, 2, 3)));
size_t stream_write_vaformat(Stream* stream, const char* format, va_list args);
bool stream_insert(Stream* stream, const uint8_t* data, size_t size);
bool stream_insert_char(Stream* stream, char c);
bool stream_insert_string(Stream* stream, FuriString* string);
bool stream_insert_cstring(Stream* stream, const char* string);
bool stream_insert_format(Stream* stream, const char* format, ...)
    _ATTRIBUTE((__format__(__printf__, 2, 3)));
bool stream_insert_vaformat(Stream* stream, const char* format, va_list args);
bool stream_delete_and_insert_char(Stream* stream, size_t delete_size, char c);
bool stream_delete_and_insert_string(Stream* stream, size_t delete_size, FuriString* string);
bool stream_delete_and_insert_cstring(Stream* stream, size_t delete_size, const char* string);
bool stream_delete_and_insert_format(Stream* stream, size_t delete_size, const char* format, ...)
    _ATTRIBUTE((__format__(__printf__, 3, 4)));
bool stream_delete_and_insert_vaformat(
    Stream* stream,
    size_t delete_size,
    const char* format,
    va_list args);
bool stream_delete(Stream* stream, size_t size);
size_t stream_copy(Stream* stream_from, Stream* stream_to, size_t size);
size_t stream_copy_full(Stream* stream_from, Stream* stream_to);
bool stream_split(Stream* stream, Stream* stream_left, Stream* stream_right);
size_t stream_load_from_file(Stream* stream, Storage* storage, const char* path);
size_t stream_save_to_file(Stream* stream, Storage* storage, const char* path, FS_OpenMode mode);
void stream_dump_data(Stream* stream);

#ifdef __cplusplus
}
#endif
