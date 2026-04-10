#include "api_hashtable.h"

#include <furi.h>

#define TAG "ApiHashtable"

static bool api_table_logged = false;

bool elf_resolve_from_hashtable(
    const ElfApiInterface* interface,
    uint32_t hash,
    Elf32_Addr* address) {
    furi_check(interface);
    furi_check(address);

    const HashtableApiInterface* hashtable_interface =
        (const HashtableApiInterface*)interface;

    const struct sym_entry* begin = hashtable_interface->table_begin;
    const struct sym_entry* end = hashtable_interface->table_end;
    size_t table_size = end - begin;

    /* Log table info and run self-test once */
    if(!api_table_logged) {
        FURI_LOG_I(TAG, "API table: %u entries, begin=%p end=%p",
            (unsigned)table_size, begin, end);
        if(table_size > 0) {
            FURI_LOG_I(TAG, "  first: hash=0x%08lx addr=0x%08lx",
                (unsigned long)begin->hash, (unsigned long)begin->address);
            FURI_LOG_I(TAG, "  last:  hash=0x%08lx addr=0x%08lx",
                (unsigned long)(end - 1)->hash, (unsigned long)(end - 1)->address);
            /* Verify sorting - check a few entries */
            bool sorted = true;
            for(size_t i = 1; i < table_size && i < 10; i++) {
                if(begin[i].hash < begin[i-1].hash) {
                    FURI_LOG_E(TAG, "  TABLE NOT SORTED at idx %u: 0x%08lx > 0x%08lx",
                        (unsigned)i, (unsigned long)begin[i-1].hash, (unsigned long)begin[i].hash);
                    sorted = false;
                }
            }
            if(sorted) {
                FURI_LOG_I(TAG, "  Table sort: OK (checked first 10)");
            }
            /* Self-test: compute hash for "strcmp" and look it up */
            uint32_t test_hash = elf_symbolname_hash("strcmp");
            FURI_LOG_I(TAG, "  Self-test: hash('strcmp')=0x%08lx", (unsigned long)test_hash);
        }
        api_table_logged = true;
    }

    /* Binary search over sorted table */
    const struct sym_entry* lo = begin;
    const struct sym_entry* hi = end;

    while(lo < hi) {
        const struct sym_entry* mid = lo + (hi - lo) / 2;
        if(mid->hash < hash) {
            lo = mid + 1;
        } else if(mid->hash > hash) {
            hi = mid;
        } else {
            *address = mid->address;
            return true;
        }
    }

    FURI_LOG_W(TAG, "Symbol hash 0x%08lx not found in %u entries",
        (unsigned long)hash, (unsigned)table_size);
    return false;
}

uint32_t elf_symbolname_hash(const char* s) {
    furi_check(s);

    uint32_t h = 0x1505;
    for(unsigned char c = *s; c != '\0'; c = *++s) {
        h = (h << 5) + h + c;
    }
    return h;
}
