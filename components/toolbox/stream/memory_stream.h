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
 * valid and unmodified until the stream is freed. The buffer is never modified
 * through the stream: write returns 0, delete_and_insert returns false and
 * clean is a no-op (content and position are left unchanged).
 *
 * Read/seek/tell/eof semantics match string_stream (seek out of range clamps
 * and returns false, eof is position >= size), so a parser that works on a
 * string-backed FlipperFormat reads this one identically.
 *
 * @param data pointer to the buffer
 * @param size buffer size in bytes
 * @return Stream*
 */
Stream* memory_stream_alloc(const uint8_t* data, size_t size);

#ifdef __cplusplus
}
#endif
