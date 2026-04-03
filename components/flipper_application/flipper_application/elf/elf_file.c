#include "elf_file.h"

#include <furi.h>

#include <stdlib.h>
#include <string.h>

#define TAG "Elf"

struct ELFFile {
    Storage* storage;
    File* fd;
    Elf32_Ehdr header;
    Elf32_Shdr* section_headers;
    char* section_names;
    size_t section_names_size;
    const ElfApiInterface* api_interface;
};

static void elf_file_release_fd(ELFFile* elf_file) {
    if(elf_file->fd) {
        storage_file_free(elf_file->fd);
        elf_file->fd = NULL;
    }
}

static void elf_file_clear_cache(ELFFile* elf_file) {
    free(elf_file->section_headers);
    elf_file->section_headers = NULL;

    free(elf_file->section_names);
    elf_file->section_names = NULL;
    elf_file->section_names_size = 0;
}

static bool elf_file_read_exact(File* file, void* buffer, size_t size) {
    return storage_file_read(file, buffer, size) == size;
}

static bool elf_file_is_valid_header(const Elf32_Ehdr* header) {
    return (header->e_ident[EI_MAG0] == ELFMAG0) && (header->e_ident[EI_MAG1] == ELFMAG1) &&
           (header->e_ident[EI_MAG2] == ELFMAG2) && (header->e_ident[EI_MAG3] == ELFMAG3) &&
           (header->e_ident[EI_CLASS] == ELFCLASS32) &&
           (header->e_ident[EI_DATA] == ELFDATA2LSB) && (header->e_type == ET_REL);
}

ELFFile* elf_file_alloc(Storage* storage, const ElfApiInterface* api_interface) {
    furi_check(storage);

    ELFFile* elf_file = calloc(1, sizeof(ELFFile));
    if(!elf_file) {
        return NULL;
    }

    elf_file->storage = storage;
    elf_file->api_interface = api_interface;

    return elf_file;
}

void elf_file_free(ELFFile* elf_file) {
    if(!elf_file) return;

    elf_file_clear_cache(elf_file);
    elf_file_release_fd(elf_file);
    free(elf_file);
}

bool elf_file_open(ELFFile* elf_file, const char* path) {
    furi_check(elf_file);
    furi_check(path);

    elf_file_clear_cache(elf_file);
    elf_file_release_fd(elf_file);
    memset(&elf_file->header, 0, sizeof(elf_file->header));

    elf_file->fd = storage_file_alloc(elf_file->storage);
    if(!elf_file->fd) {
        return false;
    }

    if(!storage_file_open(elf_file->fd, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        elf_file_release_fd(elf_file);
        return false;
    }

    if(!elf_file_read_exact(elf_file->fd, &elf_file->header, sizeof(elf_file->header)) ||
       !elf_file_is_valid_header(&elf_file->header) ||
       (elf_file->header.e_shentsize < sizeof(Elf32_Shdr)) || (elf_file->header.e_shnum == 0) ||
       (elf_file->header.e_shoff == 0) ||
       (elf_file->header.e_shstrndx >= elf_file->header.e_shnum)) {
        FURI_LOG_W(TAG, "Invalid ELF header for %s", path);
        elf_file_release_fd(elf_file);
        memset(&elf_file->header, 0, sizeof(elf_file->header));
        return false;
    }

    return true;
}

ElfLoadSectionTableResult elf_file_load_section_table(ELFFile* elf_file) {
    furi_check(elf_file);
    furi_check(elf_file->fd);

    if(elf_file->section_headers) {
        return ElfLoadSectionTableResultSuccess;
    }

    elf_file->section_headers = calloc(elf_file->header.e_shnum, sizeof(Elf32_Shdr));
    if(!elf_file->section_headers) {
        return ElfLoadSectionTableResultNoMemory;
    }

    for(size_t index = 0; index < elf_file->header.e_shnum; index++) {
        const uint32_t offset = elf_file->header.e_shoff + index * elf_file->header.e_shentsize;
        if(!storage_file_seek(elf_file->fd, offset, true) ||
           !elf_file_read_exact(elf_file->fd, &elf_file->section_headers[index], sizeof(Elf32_Shdr))) {
            elf_file_clear_cache(elf_file);
            return ElfLoadSectionTableResultError;
        }
    }

    const Elf32_Shdr* string_header = &elf_file->section_headers[elf_file->header.e_shstrndx];
    elf_file->section_names = calloc(1, string_header->sh_size + 1);
    if(!elf_file->section_names) {
        elf_file_clear_cache(elf_file);
        return ElfLoadSectionTableResultNoMemory;
    }

    elf_file->section_names_size = string_header->sh_size;
    if(!storage_file_seek(elf_file->fd, string_header->sh_offset, true) ||
       !elf_file_read_exact(elf_file->fd, elf_file->section_names, string_header->sh_size)) {
        elf_file_clear_cache(elf_file);
        return ElfLoadSectionTableResultError;
    }

    return ElfLoadSectionTableResultSuccess;
}

ELFFileLoadStatus elf_file_load_sections(ELFFile* elf_file) {
    furi_check(elf_file);

    return ELFFileLoadStatusUnspecifiedError;
}

void elf_file_call_init(ELFFile* elf) {
    UNUSED(elf);
}

bool elf_file_is_init_complete(ELFFile* elf) {
    UNUSED(elf);

    return false;
}

void* elf_file_get_entry_point(ELFFile* elf_file) {
    furi_check(elf_file);

    return NULL;
}

void elf_file_call_fini(ELFFile* elf) {
    UNUSED(elf);
}

const ElfApiInterface* elf_file_get_api_interface(ELFFile* elf_file) {
    furi_check(elf_file);

    return elf_file->api_interface;
}

ElfProcessSectionResult elf_process_section(
    ELFFile* elf_file,
    const char* name,
    ElfProcessSection* process_section,
    void* context) {
    furi_check(elf_file);
    furi_check(name);
    furi_check(process_section);

    if(elf_file_load_section_table(elf_file) != ElfLoadSectionTableResultSuccess) {
        return ElfProcessSectionResultCannotProcess;
    }

    for(size_t index = 0; index < elf_file->header.e_shnum; index++) {
        const Elf32_Shdr* section = &elf_file->section_headers[index];
        if(section->sh_name >= elf_file->section_names_size) {
            continue;
        }

        const char* section_name = elf_file->section_names + section->sh_name;
        if(strcmp(section_name, name) == 0) {
            return process_section(elf_file->fd, section->sh_offset, section->sh_size, context) ?
                       ElfProcessSectionResultSuccess :
                       ElfProcessSectionResultCannotProcess;
        }
    }

    return ElfProcessSectionResultNotFound;
}
