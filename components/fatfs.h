#pragma once

#include <stdint.h>
#include <ff.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _MAX_SS
#define _MAX_SS FF_MAX_SS
#endif

typedef FF_DIR DIR;

static inline FRESULT fatfs_compat_read(FIL* file, void* buff, UINT bytes_to_read, uint16_t* bytes_read) {
    UINT bytes_read_tmp = 0;
    const FRESULT result = f_read(file, buff, bytes_to_read, &bytes_read_tmp);
    if(bytes_read != NULL) {
        *bytes_read = (uint16_t)bytes_read_tmp;
    }
    return result;
}

static inline FRESULT fatfs_compat_write(
    FIL* file,
    const void* buff,
    UINT bytes_to_write,
    uint16_t* bytes_written) {
    UINT bytes_written_tmp = 0;
    const FRESULT result = f_write(file, buff, bytes_to_write, &bytes_written_tmp);
    if(bytes_written != NULL) {
        *bytes_written = (uint16_t)bytes_written_tmp;
    }
    return result;
}

static inline FRESULT fatfs_compat_mkfs(
    const TCHAR* path,
    BYTE format,
    UINT allocation_unit_size,
    void* work_area,
    UINT work_area_size) {
    const MKFS_PARM options = {
        .fmt = format,
        .n_fat = 0,
        .align = 0,
        .n_root = 0,
        .au_size = allocation_unit_size,
    };
    return f_mkfs(path, &options, work_area, work_area_size);
}

#define f_read(file, buff, bytes_to_read, bytes_read) \
    fatfs_compat_read((file), (buff), (bytes_to_read), (bytes_read))

#define f_write(file, buff, bytes_to_write, bytes_written) \
    fatfs_compat_write((file), (buff), (bytes_to_write), (bytes_written))

#define f_mkfs(path, format, allocation_unit_size, work_area, work_area_size) \
    fatfs_compat_mkfs((path), (format), (allocation_unit_size), (work_area), (work_area_size))

extern FATFS fatfs_object;

void fatfs_init(void);

#ifdef __cplusplus
}
#endif
