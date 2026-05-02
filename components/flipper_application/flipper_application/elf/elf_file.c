#include "elf_file.h"
#include "elf_file_i.h"

#include <storage/storage.h>
#include "elf_api_interface.h"
#include "../api_hashtable/api_hashtable.h"

#include <furi.h>

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif

#include <stdlib.h>
#include <string.h>

#define TAG "Elf"

#define ELF_NAME_BUFFER_LEN        32
#define SECTION_OFFSET(e, n)       ((e)->section_table + (n) * sizeof(Elf32_Shdr))
#define IS_FLAGS_SET(v, m)         (((v) & (m)) == (m))
#define RESOLVER_THREAD_YIELD_STEP 30

// #define ELF_DEBUG_LOG 1

#ifndef ELF_DEBUG_LOG
#undef FURI_LOG_D
#define FURI_LOG_D(...)
#endif

#ifdef ESP_PLATFORM
/* PSRAM-first allocation: keeps internal DRAM free for FreeRTOS objects,
 * I2C/SPI command links, etc. Falls back to default heap if PSRAM full. */
static inline void* elf_psram_malloc(size_t size) {
    void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(!p) p = malloc(size);
    return p;
}
#else
#define elf_psram_malloc(size) malloc(size)
#endif

#define ELF_INVALID_ADDRESS 0xFFFFFFFF

/* ESP32-S3: Convert PSRAM data bus address to instruction bus address.
 * Same physical memory, different virtual address:
 *   Data bus:        0x3C000000 - 0x3DFFFFFF
 *   Instruction bus: 0x42000000 - 0x43FFFFFF
 * Offset: 0x06000000 */
#ifdef ESP_PLATFORM
#define PSRAM_DATA_TO_INST(addr) \
    (((addr) >= 0x3C000000 && (addr) < 0x3E000000) ? ((addr) + 0x06000000) : (addr))
#else
#define PSRAM_DATA_TO_INST(addr) (addr)
#endif

/**************************************************************************************************/
/********************************************* Caches *********************************************/
/**************************************************************************************************/

static bool address_cache_get(AddressCache_t cache, int symEntry, Elf32_Addr* symAddr) {
    Elf32_Addr* addr = AddressCache_get(cache, symEntry);
    if(addr) {
        *symAddr = *addr;
        return true;
    } else {
        return false;
    }
}

static void address_cache_put(AddressCache_t cache, int symEntry, Elf32_Addr symAddr) {
    AddressCache_set_at(cache, symEntry, symAddr);
}

/**************************************************************************************************/
/********************************************** ELF ***********************************************/
/**************************************************************************************************/

static void elf_file_maybe_release_fd(ELFFile* elf) {
    if(elf->fd) {
        storage_file_free(elf->fd);
        elf->fd = NULL;
    }
}

static ELFSection* elf_file_get_section(ELFFile* elf, const char* name) {
    return ELFSectionDict_get(elf->sections, name);
}

static ELFSection* elf_file_get_or_put_section(ELFFile* elf, const char* name) {
    ELFSection* section_p = elf_file_get_section(elf, name);
    if(!section_p) {
        ELFSectionDict_set_at(
            elf->sections,
            strdup(name),
            (ELFSection){
                .data = NULL,
                .sec_idx = 0,
                .size = 0,
                .rel_count = 0,
                .rel_offset = 0,
            });
        section_p = elf_file_get_section(elf, name);
    }

    return section_p;
}

static bool elf_read_string_from_offset(ELFFile* elf, off_t offset, FuriString* name) {
    bool result = false;

    off_t old = storage_file_tell(elf->fd);

    do {
        if(!storage_file_seek(elf->fd, offset, true)) break;

        char buffer[ELF_NAME_BUFFER_LEN + 1];
        buffer[ELF_NAME_BUFFER_LEN] = 0;

        while(true) {
            size_t read = storage_file_read(elf->fd, buffer, ELF_NAME_BUFFER_LEN);
            furi_string_cat(name, buffer);
            if(strlen(buffer) < ELF_NAME_BUFFER_LEN) {
                result = true;
                break;
            }

            if(storage_file_get_error(elf->fd) != FSE_OK || read == 0) break;
        }

    } while(false);
    storage_file_seek(elf->fd, old, true);

    return result;
}

static bool elf_read_section_name(ELFFile* elf, off_t offset, FuriString* name) {
    return elf_read_string_from_offset(elf, elf->section_table_strings + offset, name);
}

static bool elf_read_symbol_name(ELFFile* elf, off_t offset, FuriString* name) {
    return elf_read_string_from_offset(elf, elf->symbol_table_strings + offset, name);
}

static bool elf_read_section_header(ELFFile* elf, size_t section_idx, Elf32_Shdr* section_header) {
    off_t offset = SECTION_OFFSET(elf, section_idx);
    return storage_file_seek(elf->fd, offset, true) &&
           storage_file_read(elf->fd, section_header, sizeof(Elf32_Shdr)) == sizeof(Elf32_Shdr);
}

static bool elf_read_section(
    ELFFile* elf,
    size_t section_idx,
    Elf32_Shdr* section_header,
    FuriString* name) {
    if(!elf_read_section_header(elf, section_idx, section_header)) {
        return false;
    }

    if(section_header->sh_name && !elf_read_section_name(elf, section_header->sh_name, name)) {
        return false;
    }

    return true;
}

static bool elf_read_symbol(ELFFile* elf, int n, Elf32_Sym* sym, FuriString* name) {
    /* Fast path: read from cached symbol table + string table (no SD card I/O) */
    if(elf->sym_cache && (size_t)n < elf->symbol_count) {
        *sym = elf->sym_cache[n];
        if(sym->st_name && elf->str_cache && sym->st_name < elf->str_cache_size) {
            furi_string_set(name, &elf->str_cache[sym->st_name]);
            return true;
        } else if(sym->st_name == 0) {
            /* Section symbol - read section name (still needs file I/O but rare) */
            Elf32_Shdr shdr;
            return elf_read_section(elf, sym->st_shndx, &shdr, name);
        }
    }

    /* Slow path: read from file */
    bool success = false;
    off_t old = storage_file_tell(elf->fd);
    off_t pos = elf->symbol_table + n * sizeof(Elf32_Sym);
    if(storage_file_seek(elf->fd, pos, true) &&
       storage_file_read(elf->fd, sym, sizeof(Elf32_Sym)) == sizeof(Elf32_Sym)) {
        if(sym->st_name)
            success = elf_read_symbol_name(elf, sym->st_name, name);
        else {
            Elf32_Shdr shdr;
            success = elf_read_section(elf, sym->st_shndx, &shdr, name);
        }
    }
    storage_file_seek(elf->fd, old, true);
    return success;
}

static ELFSection* elf_section_of(ELFFile* elf, int index) {
    ELFSectionDict_it_t it;
    for(ELFSectionDict_it(it, elf->sections); !ELFSectionDict_end_p(it); ELFSectionDict_next(it)) {
        ELFSectionDict_itref_t* itref = ELFSectionDict_ref(it);
        if(itref->value.sec_idx == index) {
            return &itref->value;
        }
    }

    return NULL;
}

static Elf32_Addr elf_address_of(ELFFile* elf, Elf32_Sym* sym, const char* sName) {
    if(sym->st_shndx == SHN_UNDEF) {
        /* Null symbol (index 0): value is 0, used for section-relative relocations */
        if(sName[0] == '\0') {
            return 0;
        }
        Elf32_Addr addr = 0;
        uint32_t hash = elf_symbolname_hash(sName);
        if(elf->api_interface->resolver_callback(elf->api_interface, hash, &addr)) {
            return addr;
        }
        FURI_LOG_E(TAG, "  API lookup FAILED: '%s' hash=0x%08lx", sName, (unsigned long)hash);
    } else {
        ELFSection* symSec = elf_section_of(elf, sym->st_shndx);
        if(symSec) {
            Elf32_Addr addr = ((Elf32_Addr)symSec->data) + sym->st_value;
            /* Convert addresses in executable sections to instruction bus.
             * STT_FUNC: named function symbols → code, needs instruction bus
             * STT_SECTION for .text: section-relative refs that may be function
             *   pointers (callbacks) → also needs instruction bus.
             * On ESP32-S3, instruction bus (0x42...) is also readable via icache
             * for data, so this is safe for both code and data access patterns. */
            if(ELF32_ST_TYPE(sym->st_info) == STT_FUNC) {
                addr = PSRAM_DATA_TO_INST(addr);
            } else if(ELF32_ST_TYPE(sym->st_info) == STT_SECTION) {
                if(sName[0] == '.' && sName[1] == 't' && sName[2] == 'e') {
                    addr = PSRAM_DATA_TO_INST(addr);
                }
            }
            return addr;
        }
        FURI_LOG_E(TAG, "  Section idx=%u not loaded for '%s' (bind=%u type=%u)",
            sym->st_shndx, sName, ELF32_ST_BIND(sym->st_info), ELF32_ST_TYPE(sym->st_info));
    }
    return ELF_INVALID_ADDRESS;
}

/**************************************************************************************************/
/*************************************** Xtensa Relocation ****************************************/
/**************************************************************************************************/

__attribute__((unused)) static const char* elf_reloc_type_to_str(int symt) {
#define STRCASE(name) \
    case name:        \
        return #name;
    switch(symt) {
        STRCASE(R_XTENSA_NONE)
        STRCASE(R_XTENSA_32)
        STRCASE(R_XTENSA_PLT)
        STRCASE(R_XTENSA_ASM_EXPAND)
        STRCASE(R_XTENSA_DIFF8)
        STRCASE(R_XTENSA_DIFF16)
        STRCASE(R_XTENSA_DIFF32)
        STRCASE(R_XTENSA_SLOT0_OP)
        STRCASE(R_XTENSA_SLOT0_ALT)
    default:
        return "R_<unknown>";
    }
#undef STRCASE
}

/**
 * @brief Handle R_XTENSA_SLOT0_OP relocation
 * This handles instruction-level fixups for Xtensa variable-length instructions.
 * The main cases are L32R (literal load) and CALL0/CALL4/CALL8/CALL12 (function calls).
 */
static bool elf_relocate_slot0(Elf32_Addr relAddr, Elf32_Addr symAddr, Elf32_Sword addend) {
    /* relAddr is a data-bus address (writable). But at runtime the CPU fetches
     * instructions from the instruction bus. PC-relative offsets must use
     * instruction-bus addresses for both PC and target. */
    Elf32_Addr instPC = PSRAM_DATA_TO_INST(relAddr);
    Elf32_Addr target = symAddr + addend;
    /* If target is also in PSRAM data-bus (internal code reference), convert */
    target = PSRAM_DATA_TO_INST(target);

    uint8_t* insn = (uint8_t*)relAddr;

    /* Xtensa instructions are either 2 bytes (narrow) or 3 bytes (wide).
     * Narrow instructions have bits [3:0] of byte 0 in {0x8..0xF} (bit 3 set).
     * Wide instructions have bits [3:0] of byte 0 in {0x0..0x7} (bit 3 clear). */

    if(insn[0] & 0x08) {
        /* Narrow (2-byte) instruction - branches like BEQZ.N, BNEZ.N, etc.
         * These have very short ranges and rarely appear in SLOT0_OP relocations.
         * For now log and skip. */
        FURI_LOG_D(TAG, "  SLOT0_OP narrow insn at 0x%08X, skipping", (unsigned int)relAddr);
        return true;
    }

    /* Wide (3-byte) instruction */
    uint8_t op0 = insn[0] & 0x0F;

    if(op0 == 0x01) {
        /* L32R instruction: loads 32-bit value from literal pool.
         * Xtensa-Encoding: vAddr = ((PC+3) & ~3) + 0xFFFC0000 + (imm16 << 2)
         * Damit ist der Offset IMMER negativ und im Bereich [-262144, -4] bytes
         * (256 KB rückwärts), kodiert als unsigned 16-bit imm16 ∈ [0, 65535].
         * Vorherige Validation prüfte signed 16-bit Range — das blockte
         * gültige Offsets > 128 KB rückwärts (siehe Doom mit ~134 KB .text).
         * Auf ESP32-S3 muss L32R die instruction-bus-Adresse benutzen. */
        Elf32_Addr l32r_target = PSRAM_DATA_TO_INST(symAddr + addend);
        Elf32_Addr pc_aligned = (instPC + 3) & ~3;
        int32_t offset = (int32_t)(l32r_target - pc_aligned);

        if(offset & 3) {
            FURI_LOG_E(TAG, "  L32R target not 4-byte aligned");
            return false;
        }

        int32_t units = offset >> 2;
        if(units < -65536 || units > -1) {
            FURI_LOG_E(TAG, "  L32R offset out of range: %d (units=%d)", (int)offset, (int)units);
            return false;
        }

        uint16_t imm16 = (uint16_t)(units & 0xFFFF);
        insn[1] = (uint8_t)(imm16 & 0xFF);
        insn[2] = (uint8_t)((imm16 >> 8) & 0xFF);
        FURI_LOG_D(TAG, "  L32R relocated, offset=%d", (int)offset);
        return true;
    }

    if(op0 == 0x05) {
        /* CALL0 instruction: PC-relative function call.
         * target = (PC & ~3) + (sign_ext(offset18) << 2) + 4
         * Both PC and target must be instruction-bus addresses. */
        Elf32_Addr pc_base = (instPC & ~3) + 4;
        int32_t offset = (int32_t)(target - pc_base);

        if(offset & 3) {
            FURI_LOG_E(TAG, "  CALL0 target not aligned");
            return false;
        }

        int32_t offset18 = offset >> 2;
        if(offset18 < -131072 || offset18 > 131071) {
            FURI_LOG_E(TAG, "  CALL0 offset out of range: %d", (int)offset18);
            return false;
        }

        insn[0] = (insn[0] & 0x3F) | (uint8_t)((offset18 & 0x03) << 6);
        insn[1] = (uint8_t)((offset18 >> 2) & 0xFF);
        insn[2] = (uint8_t)((offset18 >> 10) & 0xFF);
        FURI_LOG_D(TAG, "  CALL0 relocated, offset=%d", (int)offset18);
        return true;
    }

    if(op0 == 0x06) {
        /* Branch/Jump instructions with various sub-opcodes.
         * J (unconditional jump): op0=0x06, typically not common in SLOT0_OP.
         * For now handle J: offset18 encoding same as CALL but no +4 */
        uint8_t sub = (insn[0] >> 4) & 0x0F;
        if(sub == 0x00) {
            /* J instruction - PC-relative jump, use instruction bus addresses */
            int32_t offset = (int32_t)(target - (instPC + 4));
            int32_t offset18 = offset;

            if(offset18 < -131072 || offset18 > 131071) {
                FURI_LOG_E(TAG, "  J offset out of range");
                return false;
            }

            insn[0] = (insn[0] & 0x3F) | (uint8_t)((offset18 & 0x03) << 6);
            insn[1] = (uint8_t)((offset18 >> 2) & 0xFF);
            insn[2] = (uint8_t)((offset18 >> 10) & 0xFF);
            FURI_LOG_D(TAG, "  J relocated");
            return true;
        }
    }

    /* For other SLOT0_OP instruction types (conditional branches, etc.),
     * log a warning but don't fail - many are internal section references
     * that were already resolved by the partial linker. */
    FURI_LOG_D(
        TAG,
        "  SLOT0_OP: unhandled opcode 0x%02X at 0x%08X (op0=0x%X)",
        insn[0],
        (unsigned int)relAddr,
        op0);
    return true;
}

static bool
    elf_relocate_symbol(ELFFile* elf, Elf32_Addr relAddr, int type, Elf32_Addr symAddr, Elf32_Sword addend) {
    UNUSED(elf);

    switch(type) {
    case R_XTENSA_32:
        /* Xtensa R_XTENSA_32: result = S + A + existing_value
         * Unlike standard RELA where existing value is ignored,
         * Xtensa adds the pre-existing content to S + A.
         * This is critical for section-relative refs where the offset
         * is stored in the memory and the section base comes from S+A. */
        *((uint32_t*)relAddr) += symAddr + addend;
        FURI_LOG_D(TAG, "  R_XTENSA_32 relocated is 0x%08X", (unsigned int)*((uint32_t*)relAddr));
        break;
    case R_XTENSA_SLOT0_OP:
        return elf_relocate_slot0(relAddr, symAddr, addend);
    case R_XTENSA_ASM_EXPAND:
        /* Assembler expansion marker - no-op at link time */
        break;
    case R_XTENSA_DIFF8:
        *((uint8_t*)relAddr) += (uint8_t)(symAddr + addend);
        FURI_LOG_D(TAG, "  R_XTENSA_DIFF8 relocated");
        break;
    case R_XTENSA_DIFF16:
        *((uint16_t*)relAddr) += (uint16_t)(symAddr + addend);
        FURI_LOG_D(TAG, "  R_XTENSA_DIFF16 relocated");
        break;
    case R_XTENSA_DIFF32:
        *((uint32_t*)relAddr) += (uint32_t)(symAddr + addend);
        FURI_LOG_D(TAG, "  R_XTENSA_DIFF32 relocated");
        break;
    case R_XTENSA_NONE:
        break;
    default:
        FURI_LOG_E(TAG, "  Unsupported Xtensa relocation type %d", type);
        return false;
    }
    return true;
}

static bool elf_relocate(ELFFile* elf, ELFSection* s) {
    if(s->data) {
        size_t relEntries = s->rel_count;

        /* Read ALL relocation entries at once into RAM (bulk read).
         * This avoids thousands of individual 12-byte SD card reads. */
        size_t rela_size = relEntries * sizeof(Elf32_Rela);
        Elf32_Rela* rela_table = elf_psram_malloc(rela_size);
        if(!rela_table) {
            FURI_LOG_E(TAG, "Failed to alloc %u bytes for RELA table", (unsigned)rela_size);
            return false;
        }

        if(!storage_file_seek(elf->fd, s->rel_offset, true) ||
           storage_file_read(elf->fd, rela_table, rela_size) != rela_size) {
            FURI_LOG_E(TAG, "Failed to read RELA table");
            free(rela_table);
            return false;
        }

        int relocate_result = true;
        size_t resolved_ok = 0;
        size_t resolved_fail = 0;
        FuriString* symbol_name = furi_string_alloc();

        for(size_t relCount = 0; relCount < relEntries; relCount++) {
            if(relCount % RESOLVER_THREAD_YIELD_STEP == 0) {
                furi_delay_tick(1);
            }

            Elf32_Rela* rela = &rela_table[relCount];
            Elf32_Addr symAddr;

            int symEntry = ELF32_R_SYM(rela->r_info);
            int relType = ELF32_R_TYPE(rela->r_info);
            Elf32_Addr relAddr = ((Elf32_Addr)s->data) + rela->r_offset;

            if(!address_cache_get(elf->relocation_cache, symEntry, &symAddr)) {
                Elf32_Sym sym;
                furi_string_reset(symbol_name);
                if(!elf_read_symbol(elf, symEntry, &sym, symbol_name)) {
                    FURI_LOG_E(TAG, "  symbol read fail for entry %d", symEntry);
                    furi_string_free(symbol_name);
                    free(rela_table);
                    return false;
                }

                symAddr = elf_address_of(elf, &sym, furi_string_get_cstr(symbol_name));
                address_cache_put(elf->relocation_cache, symEntry, symAddr);

                if(symAddr == ELF_INVALID_ADDRESS) {
                    FURI_LOG_E(TAG, "  UNRESOLVED sym[%d] '%s'", symEntry, furi_string_get_cstr(symbol_name));
                }
            }

            if(symAddr != ELF_INVALID_ADDRESS) {
                if(!elf_relocate_symbol(elf, relAddr, relType, symAddr, rela->r_addend)) {
                    relocate_result = false;
                } else {
                    resolved_ok++;
                }
            } else {
                resolved_fail++;
                relocate_result = false;
            }
        }
        furi_string_free(symbol_name);
        free(rela_table);

        FURI_LOG_I(TAG, "Relocation: %u OK, %u FAILED out of %u",
            (unsigned)resolved_ok, (unsigned)resolved_fail, (unsigned)relEntries);

        return relocate_result;
    } else {
        FURI_LOG_D(TAG, "Section not loaded");
    }

    return false;
}

/**************************************************************************************************/
/************************************ Internal FAP interfaces *************************************/
/**************************************************************************************************/
typedef enum {
    SectionTypeUnused = 1 << 0,
    SectionTypeData = 1 << 1,
    SectionTypeRelData = 1 << 2,
    SectionTypeSymTab = 1 << 3,
    SectionTypeStrTab = 1 << 4,
    SectionTypeDebugLink = 1 << 5,
} SectionType;

static bool elf_load_debug_link(ELFFile* elf, Elf32_Shdr* section_header) {
    elf->debug_link_info.debug_link_size = section_header->sh_size;
    elf->debug_link_info.debug_link = malloc(section_header->sh_size);

    return storage_file_seek(elf->fd, section_header->sh_offset, true) &&
           storage_file_read(elf->fd, elf->debug_link_info.debug_link, section_header->sh_size) ==
               section_header->sh_size;
}

static bool str_prefix(const char* str, const char* prefix) {
    return strncmp(prefix, str, strlen(prefix)) == 0;
}

typedef enum {
    ELFLoadSectionResultSuccess,
    ELFLoadSectionResultNoMemory,
    ELFLoadSectionResultError,
} ELFLoadSectionResult;

typedef struct {
    SectionType type;
    ELFLoadSectionResult result;
} SectionTypeInfo;

static ELFLoadSectionResult
    elf_load_section_data(ELFFile* elf, ELFSection* section, Elf32_Shdr* section_header) {
    if(section_header->sh_size == 0) {
        FURI_LOG_D(TAG, "No data for section");
        return ELFLoadSectionResultSuccess;
    }

#ifdef ESP_PLATFORM
    /* Force PSRAM allocation so PSRAM_DATA_TO_INST (0x3C->0x42) works.
     * Internal DRAM (0x3FC...) has a different instruction bus mapping
     * that our simple offset conversion doesn't handle. */
    section->data = heap_caps_aligned_alloc(
        section_header->sh_addralign,
        section_header->sh_size,
        MALLOC_CAP_SPIRAM);
    if(!section->data) {
        /* Fallback to any available memory */
        section->data = aligned_malloc(section_header->sh_size, section_header->sh_addralign);
    }
#else
    section->data = aligned_malloc(section_header->sh_size, section_header->sh_addralign);
#endif
    section->size = section_header->sh_size;

    if(section_header->sh_type == SHT_NOBITS) {
        // BSS section, no data to load. heap_caps_aligned_alloc(SPIRAM)
        // does NOT zero memory — only the aligned_malloc fallback does (it
        // uses calloc via memmgr.h's redefine). Zero explicitly so static
        // BSS variables in FAPs are NULL/0 as the C standard demands.
        memset(section->data, 0, section_header->sh_size);
        return ELFLoadSectionResultSuccess;
    }

    if((!storage_file_seek(elf->fd, section_header->sh_offset, true)) ||
       (storage_file_read(elf->fd, section->data, section_header->sh_size) !=
        section_header->sh_size)) {
        FURI_LOG_E(TAG, "    seek/read fail");
        return ELFLoadSectionResultError;
    }

    FURI_LOG_D(TAG, "0x%p", section->data);
    return ELFLoadSectionResultSuccess;
}

static SectionTypeInfo elf_preload_section(
    ELFFile* elf,
    size_t section_idx,
    Elf32_Shdr* section_header,
    FuriString* name_string) {
    const char* name = furi_string_get_cstr(name_string);
    SectionTypeInfo info;

#ifdef ELF_DEBUG_LOG
    FuriString* flags_string = furi_string_alloc();
    if(section_header->sh_flags & SHF_WRITE) furi_string_cat(flags_string, "W");
    if(section_header->sh_flags & SHF_ALLOC) furi_string_cat(flags_string, "A");
    if(section_header->sh_flags & SHF_EXECINSTR) furi_string_cat(flags_string, "X");
    if(section_header->sh_flags & SHF_MERGE) furi_string_cat(flags_string, "M");
    if(section_header->sh_flags & SHF_STRINGS) furi_string_cat(flags_string, "S");
    if(section_header->sh_flags & SHF_INFO_LINK) furi_string_cat(flags_string, "I");
    if(section_header->sh_flags & SHF_LINK_ORDER) furi_string_cat(flags_string, "L");

    FURI_LOG_I(
        TAG,
        "Section %s: type: %ld, flags: %s",
        name,
        section_header->sh_type,
        furi_string_get_cstr(flags_string));
    furi_string_free(flags_string);
#endif

    // ignore Xtensa-specific metadata sections
    if(str_prefix(name, ".xtensa.") || str_prefix(name, ".rela.xtensa.") ||
       str_prefix(name, ".xt.") || str_prefix(name, ".rela.xt.") ||
       strcmp(name, ".comment") == 0) {
        FURI_LOG_D(TAG, "Ignoring metadata section '%s'", name);

        info.type = SectionTypeUnused;
        info.result = ELFLoadSectionResultSuccess;
        return info;
    }

    // Load allocable section
    if(section_header->sh_flags & SHF_ALLOC) {
        ELFSection* section_p = elf_file_get_or_put_section(elf, name);
        section_p->sec_idx = section_idx;

        if(section_header->sh_type == SHT_PREINIT_ARRAY) {
            furi_assert(elf->preinit_array == NULL);
            elf->preinit_array = section_p;
        } else if(section_header->sh_type == SHT_INIT_ARRAY) {
            furi_assert(elf->init_array == NULL);
            elf->init_array = section_p;
        } else if(section_header->sh_type == SHT_FINI_ARRAY) {
            furi_assert(elf->fini_array == NULL);
            elf->fini_array = section_p;
        }

        info.type = SectionTypeData;
        info.result = elf_load_section_data(elf, section_p, section_header);

        if(info.result != ELFLoadSectionResultSuccess) {
            FURI_LOG_E(TAG, "Error loading section '%s'", name);
        }

        return info;
    }

    // Load RELA link info section (Xtensa uses SHT_RELA)
    if(section_header->sh_flags & SHF_INFO_LINK) {
        info.type = SectionTypeRelData;

        if(str_prefix(name, ".rela")) {
            name = name + strlen(".rela");
            ELFSection* section_p = elf_file_get_or_put_section(elf, name);
            section_p->rel_count = section_header->sh_size / sizeof(Elf32_Rela);
            section_p->rel_offset = section_header->sh_offset;
            info.result = ELFLoadSectionResultSuccess;
        } else if(str_prefix(name, ".rel")) {
            // Fallback for SHT_REL (shouldn't happen on Xtensa, but be safe)
            name = name + strlen(".rel");
            ELFSection* section_p = elf_file_get_or_put_section(elf, name);
            section_p->rel_count = section_header->sh_size / sizeof(Elf32_Rel);
            section_p->rel_offset = section_header->sh_offset;
            info.result = ELFLoadSectionResultSuccess;
        } else {
            FURI_LOG_E(TAG, "Unknown link info section '%s'", name);
            info.result = ELFLoadSectionResultError;
        }

        return info;
    }

    // Load symbol table
    if(strcmp(name, ".symtab") == 0) {
        FURI_LOG_D(TAG, "Found .symtab section");
        elf->symbol_table = section_header->sh_offset;
        elf->symbol_count = section_header->sh_size / sizeof(Elf32_Sym);

        /* Cache entire symbol table in RAM for fast lookup */
        elf->sym_cache = elf_psram_malloc(section_header->sh_size);
        if(elf->sym_cache) {
            if(!storage_file_seek(elf->fd, section_header->sh_offset, true) ||
               storage_file_read(elf->fd, elf->sym_cache, section_header->sh_size) !=
                   section_header->sh_size) {
                free(elf->sym_cache);
                elf->sym_cache = NULL;
            }
        }

        info.type = SectionTypeSymTab;
        info.result = ELFLoadSectionResultSuccess;
        return info;
    }

    // Load string table
    if(strcmp(name, ".strtab") == 0) {
        FURI_LOG_D(TAG, "Found .strtab section");
        elf->symbol_table_strings = section_header->sh_offset;

        /* Cache entire string table in RAM */
        elf->str_cache_size = section_header->sh_size;
        elf->str_cache = elf_psram_malloc(section_header->sh_size);
        if(elf->str_cache) {
            if(!storage_file_seek(elf->fd, section_header->sh_offset, true) ||
               storage_file_read(elf->fd, elf->str_cache, section_header->sh_size) !=
                   section_header->sh_size) {
                free(elf->str_cache);
                elf->str_cache = NULL;
                elf->str_cache_size = 0;
            }
        }

        info.type = SectionTypeStrTab;
        info.result = ELFLoadSectionResultSuccess;
        return info;
    }

    // Load debug link section
    if(strcmp(name, ".gnu_debuglink") == 0) {
        FURI_LOG_D(TAG, "Found .gnu_debuglink section");
        info.type = SectionTypeDebugLink;

        if(elf_load_debug_link(elf, section_header)) {
            info.result = ELFLoadSectionResultSuccess;
            return info;
        } else {
            info.result = ELFLoadSectionResultError;
            return info;
        }
    }

    info.type = SectionTypeUnused;
    info.result = ELFLoadSectionResultSuccess;
    return info;
}

static bool elf_relocate_section(ELFFile* elf, ELFSection* section) {
    if(section->rel_count) {
        if(section->data == NULL) {
            /* Section war nicht alloziert (kein SHF_ALLOC, z.B. .debug_*).
             * Ihre Rela-Einträge sind nur für externe Tools (Debugger,
             * gdb-stub) relevant und müssen zur Laufzeit nicht aufgelöst
             * werden. Skip statt fehlschlagen — sonst bricht der Loader
             * jeden FAP ab, der mit Debug-Symbolen gebaut wurde. */
            FURI_LOG_D(TAG, "Skipping relocation: section not allocated");
            return true;
        }
        FURI_LOG_D(TAG, "Relocating section");
        return elf_relocate(elf, section);
    } else {
        FURI_LOG_D(TAG, "No relocation index"); /* Not an error */
    }
    return true;
}

static void elf_file_call_section_list(ELFSection* section, bool reverse_order) {
    if(section && section->size) {
        const uint32_t* start = section->data;
        const uint32_t* end = section->data + section->size;

        if(reverse_order) {
            while(end > start) {
                end--;
                ((void (*)(void))(*end))();
            }
        } else {
            while(start < end) {
                ((void (*)(void))(*start))();
                start++;
            }
        }
    }
}

/**************************************************************************************************/
/********************************************* Public *********************************************/
/**************************************************************************************************/

ELFFile* elf_file_alloc(Storage* storage, const ElfApiInterface* api_interface) {
    ELFFile* elf = malloc(sizeof(ELFFile));
    memset(elf, 0, sizeof(ELFFile));
    elf->fd = storage_file_alloc(storage);
    elf->api_interface = api_interface;
    ELFSectionDict_init(elf->sections);
    elf->init_array_called = false;
    return elf;
}

void elf_file_free(ELFFile* elf) {
    if(elf->init_array_called) {
        FURI_LOG_W(TAG, "Init array was called, but fini array wasn't");
        elf_file_call_section_list(elf->fini_array, true);
    }

    // free sections data
    {
        ELFSectionDict_it_t it;
        for(ELFSectionDict_it(it, elf->sections); !ELFSectionDict_end_p(it);
            ELFSectionDict_next(it)) {
            const ELFSectionDict_itref_t* itref = ELFSectionDict_cref(it);
#ifdef ESP_PLATFORM
            if(itref->value.data) heap_caps_free(itref->value.data);
#else
            aligned_free(itref->value.data);
#endif
            free((void*)itref->key);
        }

        ELFSectionDict_clear(elf->sections);
    }

    if(elf->debug_link_info.debug_link) {
        free(elf->debug_link_info.debug_link);
    }

    if(elf->sym_cache) free(elf->sym_cache);
    if(elf->str_cache) free(elf->str_cache);

    elf_file_maybe_release_fd(elf);
    free(elf);
}

bool elf_file_open(ELFFile* elf, const char* path) {
    Elf32_Ehdr h;
    Elf32_Shdr sH;

    FURI_LOG_I(TAG, "Opening ELF file: %s", path);

    if(!storage_file_open(elf->fd, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        FURI_LOG_E(TAG, "Failed to open file: %s", path);
        return false;
    }

    if(!storage_file_seek(elf->fd, 0, true)) {
        FURI_LOG_E(TAG, "Failed to seek to start");
        return false;
    }

    uint16_t bytes_read = storage_file_read(elf->fd, &h, sizeof(h));
    if(bytes_read != sizeof(h)) {
        FURI_LOG_E(TAG, "Failed to read ELF header: got %u, expected %u", bytes_read, (unsigned)sizeof(h));
        return false;
    }

    FURI_LOG_I(
        TAG,
        "ELF header: magic=%02X%c%c%c class=%u data=%u type=%u machine=%u",
        h.e_ident[EI_MAG0],
        h.e_ident[EI_MAG1],
        h.e_ident[EI_MAG2],
        h.e_ident[EI_MAG3],
        h.e_ident[EI_CLASS],
        h.e_ident[EI_DATA],
        h.e_type,
        h.e_machine);

    FURI_LOG_I(
        TAG,
        "ELF: entry=0x%lX shoff=0x%lX shnum=%u shstrndx=%u",
        (unsigned long)h.e_entry,
        (unsigned long)h.e_shoff,
        h.e_shnum,
        h.e_shstrndx);

    /* Validate ELF header */
    if(h.e_ident[EI_MAG0] != ELFMAG0 || h.e_ident[EI_MAG1] != ELFMAG1 ||
       h.e_ident[EI_MAG2] != ELFMAG2 || h.e_ident[EI_MAG3] != ELFMAG3) {
        FURI_LOG_E(TAG, "Invalid ELF magic");
        return false;
    }

    if(h.e_ident[EI_CLASS] != ELFCLASS32) {
        FURI_LOG_E(TAG, "Not 32-bit ELF (class=%u)", h.e_ident[EI_CLASS]);
        return false;
    }

    if(h.e_ident[EI_DATA] != ELFDATA2LSB) {
        FURI_LOG_E(TAG, "Not little-endian ELF (data=%u)", h.e_ident[EI_DATA]);
        return false;
    }

    if(h.e_type != ET_REL) {
        FURI_LOG_E(TAG, "Not relocatable ELF (type=%u, expected %u)", h.e_type, ET_REL);
        return false;
    }

    if(h.e_shoff == 0 || h.e_shnum == 0) {
        FURI_LOG_E(TAG, "No section headers (shoff=%lu, shnum=%u)", (unsigned long)h.e_shoff, h.e_shnum);
        return false;
    }

    if(h.e_shstrndx >= h.e_shnum) {
        FURI_LOG_E(TAG, "Invalid shstrndx=%u (shnum=%u)", h.e_shstrndx, h.e_shnum);
        return false;
    }

    off_t shstr_offset = h.e_shoff + h.e_shstrndx * sizeof(sH);
    FURI_LOG_I(TAG, "Reading section string table header at offset 0x%lX", (unsigned long)shstr_offset);

    if(!storage_file_seek(elf->fd, shstr_offset, true) ||
       storage_file_read(elf->fd, &sH, sizeof(Elf32_Shdr)) != sizeof(Elf32_Shdr)) {
        FURI_LOG_E(TAG, "Failed to read section string table header");
        return false;
    }

    FURI_LOG_I(TAG, "Section string table: offset=0x%lX size=%lu", (unsigned long)sH.sh_offset, (unsigned long)sH.sh_size);

    elf->entry = h.e_entry;
    elf->sections_count = h.e_shnum;
    elf->section_table = h.e_shoff;
    elf->section_table_strings = sH.sh_offset;

    FURI_LOG_I(TAG, "ELF file opened OK: %u sections", (unsigned)elf->sections_count);
    return true;
}

ElfLoadSectionTableResult elf_file_load_section_table(ELFFile* elf) {
    SectionType loaded_sections = 0;
    FuriString* name = furi_string_alloc();
    ElfLoadSectionTableResult result = ElfLoadSectionTableResultSuccess;

    FURI_LOG_D(TAG, "Scan ELF sections...");

    for(size_t section_idx = 1; section_idx < elf->sections_count; section_idx++) {
        Elf32_Shdr section_header;

        furi_string_reset(name);
        if(!elf_read_section(elf, section_idx, &section_header, name)) {
            loaded_sections = 0;
            break;
        }

        FURI_LOG_D(
            TAG, "Preloading data for section #%d %s", section_idx, furi_string_get_cstr(name));
        SectionTypeInfo section_type_info =
            elf_preload_section(elf, section_idx, &section_header, name);
        loaded_sections |= section_type_info.type;

        if(section_type_info.result != ELFLoadSectionResultSuccess) {
            if(section_type_info.result == ELFLoadSectionResultNoMemory) {
                FURI_LOG_E(TAG, "Not enough memory");
                result = ElfLoadSectionTableResultNoMemory;
            } else if(section_type_info.result == ELFLoadSectionResultError) {
                FURI_LOG_E(TAG, "Error loading section");
                result = ElfLoadSectionTableResultError;
            }

            loaded_sections = 0;
            break;
        }
    }

    furi_string_free(name);

    if(result != ElfLoadSectionTableResultSuccess) {
        return result;
    } else {
        bool sections_valid =
            IS_FLAGS_SET(loaded_sections, SectionTypeSymTab | SectionTypeStrTab);
        if(sections_valid) {
            return ElfLoadSectionTableResultSuccess;
        } else {
            FURI_LOG_E(TAG, "No valid sections found");
            return ElfLoadSectionTableResultError;
        }
    }
}

ElfProcessSectionResult elf_process_section(
    ELFFile* elf,
    const char* name,
    ElfProcessSection* process_section,
    void* context) {
    FURI_LOG_I(TAG, "Looking for section '%s' in %u sections", name, (unsigned)elf->sections_count);

    ElfProcessSectionResult result = ElfProcessSectionResultNotFound;
    FuriString* section_name = furi_string_alloc();
    Elf32_Shdr section_header;

    // find section
    for(size_t section_idx = 1; section_idx < elf->sections_count; section_idx++) {
        furi_string_reset(section_name);
        if(!elf_read_section(elf, section_idx, &section_header, section_name)) {
            FURI_LOG_E(TAG, "Failed to read section #%u", (unsigned)section_idx);
            break;
        }

        FURI_LOG_D(TAG, "  Section #%u: '%s'", (unsigned)section_idx, furi_string_get_cstr(section_name));

        if(furi_string_cmp(section_name, name) == 0) {
            FURI_LOG_I(TAG, "Found section '%s' at idx %u, offset=0x%lX size=%lu",
                name, (unsigned)section_idx,
                (unsigned long)section_header.sh_offset,
                (unsigned long)section_header.sh_size);
            result = ElfProcessSectionResultCannotProcess;
            break;
        }
    }

    if(result == ElfProcessSectionResultNotFound) {
        FURI_LOG_W(TAG, "Section '%s' not found", name);
    } else {
        if(process_section(elf->fd, section_header.sh_offset, section_header.sh_size, context)) {
            FURI_LOG_I(TAG, "Section '%s' processed OK", name);
            result = ElfProcessSectionResultSuccess;
        } else {
            FURI_LOG_E(TAG, "Section '%s' processing failed", name);
            result = ElfProcessSectionResultCannotProcess; //-V1048
        }
    }

    furi_string_free(section_name);

    return result;
}

ELFFileLoadStatus elf_file_load_sections(ELFFile* elf) {
    furi_check(elf->fd != NULL);
    ELFFileLoadStatus status = ELFFileLoadStatusSuccess;
    ELFSectionDict_it_t it;

    AddressCache_init(elf->relocation_cache);

    for(ELFSectionDict_it(it, elf->sections); !ELFSectionDict_end_p(it); ELFSectionDict_next(it)) {
        ELFSectionDict_itref_t* itref = ELFSectionDict_ref(it);
        FURI_LOG_D(TAG, "Relocating section '%s'", itref->key);
        if(!elf_relocate_section(elf, &itref->value)) {
            FURI_LOG_E(TAG, "Error relocating section '%s'", itref->key);
            status = ELFFileLoadStatusMissingImports;
        }
    }

    /* Fixing up entry point */
    if(status == ELFFileLoadStatusSuccess) {
        ELFSection* text_section = elf_file_get_section(elf, ".text");

        if(text_section == NULL) {
            FURI_LOG_E(TAG, "No .text section found");
            status = ELFFileLoadStatusUnspecifiedError;
        } else {
            elf->entry += (uint32_t)text_section->data;
            /* Convert to instruction bus address for code execution */
            elf->entry = PSRAM_DATA_TO_INST(elf->entry);
            FURI_LOG_I(TAG, "Entry point: 0x%08lX", (unsigned long)elf->entry);
        }
    }

    FURI_LOG_D(TAG, "Relocation cache size: %u", AddressCache_size(elf->relocation_cache));
    AddressCache_clear(elf->relocation_cache);

    {
        size_t total_size = 0;
        for(ELFSectionDict_it(it, elf->sections); !ELFSectionDict_end_p(it);
            ELFSectionDict_next(it)) {
            ELFSectionDict_itref_t* itref = ELFSectionDict_ref(it);
            total_size += itref->value.size;
        }
        FURI_LOG_I(TAG, "Total size of loaded sections: %zu", total_size);
    }

    elf_file_maybe_release_fd(elf);
    return status;
}

void elf_file_call_init(ELFFile* elf) {
    furi_check(!elf->init_array_called);
    elf_file_call_section_list(elf->preinit_array, false);
    elf_file_call_section_list(elf->init_array, false);
    elf->init_array_called = true;
}

bool elf_file_is_init_complete(ELFFile* elf) {
    return elf->init_array_called;
}

void* elf_file_get_entry_point(ELFFile* elf) {
    furi_check(elf->init_array_called);
    return (void*)elf->entry;
}

void elf_file_call_fini(ELFFile* elf) {
    furi_check(elf->init_array_called);
    elf_file_call_section_list(elf->fini_array, true);
    elf->init_array_called = false;
}

const ElfApiInterface* elf_file_get_api_interface(ELFFile* elf_file) {
    return elf_file->api_interface;
}

void elf_file_init_debug_info(ELFFile* elf, ELFDebugInfo* debug_info) {
    // set entry
    debug_info->entry = elf->entry;

    // copy debug info
    memcpy(&debug_info->debug_link_info, &elf->debug_link_info, sizeof(ELFDebugLinkInfo));

    // init mmap
    debug_info->mmap_entry_count = ELFSectionDict_size(elf->sections);
    debug_info->mmap_entries = malloc(sizeof(ELFMemoryMapEntry) * debug_info->mmap_entry_count);
    uint32_t mmap_entry_idx = 0;

    ELFSectionDict_it_t it;
    for(ELFSectionDict_it(it, elf->sections); !ELFSectionDict_end_p(it); ELFSectionDict_next(it)) {
        const ELFSectionDict_itref_t* itref = ELFSectionDict_cref(it);

        const void* data_ptr = itref->value.data;
        if(data_ptr) {
            ELFMemoryMapEntry* entry = &debug_info->mmap_entries[mmap_entry_idx];
            entry->address = (uint32_t)data_ptr;
            entry->name = itref->key;
            mmap_entry_idx++;
        }
    }
}

void elf_file_clear_debug_info(ELFDebugInfo* debug_info) {
    memset(&debug_info->debug_link_info, 0, sizeof(ELFDebugLinkInfo));

    if(debug_info->mmap_entries) {
        free(debug_info->mmap_entries);
        debug_info->mmap_entries = NULL;
    }

    debug_info->mmap_entry_count = 0;
}
