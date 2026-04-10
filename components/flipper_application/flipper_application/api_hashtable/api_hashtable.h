#pragma once

#include <flipper_application/elf/elf_api_interface.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Symbol table entry
 */
struct sym_entry {
    uint32_t hash;
    uint32_t address;
};

/**
 * @brief HashtableApiInterface is an implementation of ElfApiInterface
 * that uses a pre-sorted hash table to resolve function addresses.
 */
typedef struct {
    ElfApiInterface base;
    const struct sym_entry* table_begin;
    const struct sym_entry* table_end;
} HashtableApiInterface;

/**
 * @brief Resolver for API entries using a pre-sorted table with hashes
 * @param interface pointer to HashtableApiInterface
 * @param hash gnu hash of function name
 * @param address output for function address
 * @return true if the table contains a function
 */
bool elf_resolve_from_hashtable(
    const ElfApiInterface* interface,
    uint32_t hash,
    Elf32_Addr* address);

/**
 * @brief Calculate GNU hash for a symbol name
 * @param s symbol name string
 * @return hash value
 */
uint32_t elf_symbolname_hash(const char* s);

#ifdef __cplusplus
}
#endif
