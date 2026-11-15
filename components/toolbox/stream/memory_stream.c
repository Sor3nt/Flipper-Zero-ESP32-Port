#include "stream.h"
#include "stream_i.h"
#include "memory_stream.h"

#include <string.h>
#include <core/check.h>
#include <core/common_defines.h>

typedef struct {
    Stream stream_base;
    const uint8_t* data;
    size_t size;
    size_t index;
} MemoryStream;

static void memory_stream_free(MemoryStream* stream);
static bool memory_stream_eof(MemoryStream* stream);
static void memory_stream_clean(MemoryStream* stream);
static bool memory_stream_seek(MemoryStream* stream, int32_t offset, StreamOffset offset_type);
static size_t memory_stream_tell(MemoryStream* stream);
static size_t memory_stream_size(MemoryStream* stream);
static size_t memory_stream_write(MemoryStream* stream, const uint8_t* data, size_t size);
static size_t memory_stream_read(MemoryStream* stream, uint8_t* data, size_t size);
static bool memory_stream_delete_and_insert(
    MemoryStream* stream,
    size_t delete_size,
    StreamWriteCB write_callback,
    const void* ctx);

static const StreamVTable memory_stream_vtable = {
    .free = (StreamFreeFn)memory_stream_free,
    .eof = (StreamEOFFn)memory_stream_eof,
    .clean = (StreamCleanFn)memory_stream_clean,
    .seek = (StreamSeekFn)memory_stream_seek,
    .tell = (StreamTellFn)memory_stream_tell,
    .size = (StreamSizeFn)memory_stream_size,
    .write = (StreamWriteFn)memory_stream_write,
    .read = (StreamReadFn)memory_stream_read,
    .delete_and_insert = (StreamDeleteAndInsertFn)memory_stream_delete_and_insert,
};

Stream* memory_stream_alloc(const uint8_t* data, size_t size) {
    furi_check(data || size == 0);
    MemoryStream* stream = malloc(sizeof(MemoryStream));
    stream->data = data;
    stream->size = size;
    stream->index = 0;
    stream->stream_base.vtable = &memory_stream_vtable;
    return (Stream*)stream;
}

static void memory_stream_free(MemoryStream* stream) {
    // The buffer is owned by the caller
    free(stream);
}

static bool memory_stream_eof(MemoryStream* stream) {
    return stream->index >= stream->size;
}

static void memory_stream_clean(MemoryStream* stream) {
    // Read-only: content cannot be deleted
    UNUSED(stream);
}

static bool memory_stream_seek(MemoryStream* stream, int32_t offset, StreamOffset offset_type) {
    int64_t base = 0;
    switch(offset_type) {
    case StreamOffsetFromStart:
        base = 0;
        break;
    case StreamOffsetFromCurrent:
        base = (int64_t)stream->index;
        break;
    case StreamOffsetFromEnd:
        base = (int64_t)stream->size;
        break;
    }

    const int64_t target = base + offset;
    bool result = true;
    if(target < 0) {
        stream->index = 0;
        result = false;
    } else if(target > (int64_t)stream->size) {
        stream->index = stream->size;
        result = false;
    } else {
        stream->index = (size_t)target;
    }
    return result;
}

static size_t memory_stream_tell(MemoryStream* stream) {
    return stream->index;
}

static size_t memory_stream_size(MemoryStream* stream) {
    return stream->size;
}

static size_t memory_stream_write(MemoryStream* stream, const uint8_t* data, size_t size) {
    // Read-only
    UNUSED(stream);
    UNUSED(data);
    UNUSED(size);
    return 0;
}

static size_t memory_stream_read(MemoryStream* stream, uint8_t* data, size_t size) {
    const size_t remaining = stream->size - stream->index;
    const size_t to_read = MIN(size, remaining);
    if(to_read) {
        memcpy(data, stream->data + stream->index, to_read);
        stream->index += to_read;
    }
    return to_read;
}

static bool memory_stream_delete_and_insert(
    MemoryStream* stream,
    size_t delete_size,
    StreamWriteCB write_callback,
    const void* ctx) {
    // Read-only
    UNUSED(stream);
    UNUSED(delete_size);
    UNUSED(write_callback);
    UNUSED(ctx);
    return false;
}
