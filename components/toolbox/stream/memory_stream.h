#pragma once
#include <stdlib.h>
#include <stdint.h>
#include "stream.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Allocate a read-only stream over an existing memory buffer.
 *
 * The stream does NOT copy or take ownership of @p data: the buffer must stay
 * valid and unmodified until the stream is freed. Write, clean and
 * delete_and_insert operations are rejected, so the buffer is never modified
 * through the stream.
 *
 * Seek/tell/eof semantics match string_stream, so any parser that works on a
 * string-backed FlipperFormat works identically on this one.
 *
 * @param data pointer to the buffer
 * @param size buffer size in bytes
 * @return Stream*
 */
Stream* memory_stream_alloc(const uint8_t* data, size_t size);

#ifdef __cplusplus
}
#endif
