#pragma once

#include "stream.h"

#ifdef __cplusplus
extern "C" {
#endif

Stream* file_stream_alloc(Storage* storage);

bool file_stream_open(
    Stream* stream,
    const char* path,
    FS_AccessMode access_mode,
    FS_OpenMode open_mode);

bool file_stream_close(Stream* stream);
FS_Error file_stream_get_error(Stream* stream);

#ifdef __cplusplus
}
#endif
