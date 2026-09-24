#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct Storage Storage;
typedef struct File File;
enum { FSAM_WRITE,FSOM_CREATE_NEW,FSAM_READ,FSOM_OPEN_EXISTING,FSOM_CREATE_ALWAYS };
typedef struct { unsigned unused; } FileInfo;
enum { FSE_OK, FSE_NOT_EXIST };
typedef int FS_Error;
FS_Error storage_file_get_error(File* file);
uint64_t storage_file_size(File* file);
bool storage_file_seek(File* file,uint32_t offset,bool from_start);
int storage_common_stat(Storage* storage,const char* path,FileInfo* info);
int storage_common_remove(Storage* storage,const char* path);
int storage_common_rename(Storage* storage,const char* from,const char* to);
File* storage_file_alloc(Storage* storage);
bool storage_file_open(File* file,const char* path,int access,int mode);
size_t storage_file_write(File* file,const void* buffer,size_t length);
size_t storage_file_read(File* file,void* buffer,size_t length);
bool storage_file_sync(File* file);
bool storage_file_close(File* file);
void storage_file_free(File* file);
bool storage_simply_mkdir(Storage* storage,const char* path);
