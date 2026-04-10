/**
 * @file elf_file.h
 * ELF file loader
 */
#pragma once

#include <storage/storage.h>

#include "elf.h"
#include "elf_api_interface.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ELFFile ELFFile;

typedef struct {
    const char* name;
    uint32_t address;
} ELFMemoryMapEntry;

typedef struct {
    uint32_t debug_link_size;
    uint8_t* debug_link;
} ELFDebugLinkInfo;

typedef struct {
    uint32_t mmap_entry_count;
    ELFMemoryMapEntry* mmap_entries;
    ELFDebugLinkInfo debug_link_info;
    off_t entry;
} ELFDebugInfo;

typedef enum {
    ELFFileLoadStatusSuccess = 0,
    ELFFileLoadStatusUnspecifiedError,
    ELFFileLoadStatusMissingImports,
} ELFFileLoadStatus;

typedef enum {
    ElfProcessSectionResultNotFound,
    ElfProcessSectionResultCannotProcess,
    ElfProcessSectionResultSuccess,
} ElfProcessSectionResult;

typedef enum {
    ElfLoadSectionTableResultError,
    ElfLoadSectionTableResultNoMemory,
    ElfLoadSectionTableResultSuccess,
} ElfLoadSectionTableResult;

typedef bool(ElfProcessSection)(File* file, size_t offset, size_t size, void* context);

ELFFile* elf_file_alloc(Storage* storage, const ElfApiInterface* api_interface);
void elf_file_free(ELFFile* elf_file);
bool elf_file_open(ELFFile* elf_file, const char* path);
ElfLoadSectionTableResult elf_file_load_section_table(ELFFile* elf_file);
ELFFileLoadStatus elf_file_load_sections(ELFFile* elf_file);
void elf_file_call_init(ELFFile* elf);
bool elf_file_is_init_complete(ELFFile* elf);
void* elf_file_get_entry_point(ELFFile* elf_file);
void elf_file_call_fini(ELFFile* elf);
const ElfApiInterface* elf_file_get_api_interface(ELFFile* elf_file);
void elf_file_init_debug_info(ELFFile* elf_file, ELFDebugInfo* debug_info);
void elf_file_clear_debug_info(ELFDebugInfo* debug_info);

ElfProcessSectionResult elf_process_section(
    ELFFile* elf_file,
    const char* name,
    ElfProcessSection* process_section,
    void* context);

#ifdef __cplusplus
}
#endif
