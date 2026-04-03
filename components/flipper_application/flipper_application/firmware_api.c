/**
 * @brief Stub firmware API interface for ESP32 port.
 * ESP32 does not load FAPs, but the API interface symbol is needed by code
 * that initializes the composite resolver.
 */
#include <flipper_application/elf/elf_api_interface.h>
#include <stdbool.h>
#include <stddef.h>

static bool firmware_api_stub_resolver(
    const ElfApiInterface* interface,
    uint32_t hash,
    Elf32_Addr* address) {
    (void)interface;
    (void)hash;
    (void)address;
    return false;
}

static const ElfApiInterface firmware_api = {
    .api_version_major = 0,
    .api_version_minor = 0,
    .resolver_callback = firmware_api_stub_resolver,
};

const ElfApiInterface* const firmware_api_interface = &firmware_api;
