/**
 * @brief Stub NFC application API interface for ESP32 port.
 */
#include <flipper_application/elf/elf_api_interface.h>
#include <stdbool.h>
#include <stddef.h>

static bool nfc_app_api_stub_resolver(
    const ElfApiInterface* interface,
    uint32_t hash,
    Elf32_Addr* address) {
    (void)interface;
    (void)hash;
    (void)address;
    return false;
}

static const ElfApiInterface nfc_app_api = {
    .api_version_major = 0,
    .api_version_minor = 0,
    .resolver_callback = nfc_app_api_stub_resolver,
};

const ElfApiInterface* const nfc_application_api_interface = &nfc_app_api;
