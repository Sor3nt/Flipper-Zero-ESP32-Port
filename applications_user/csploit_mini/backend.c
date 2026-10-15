/* Optional versioned lookup: the app stays loadable on pi11 without the
 * extension. */
#include "backend.h"
#include <flipper_application/elf/elf_api_interface.h>
extern const ElfApiInterface *const firmware_api_interface;
const CsNetApi *cs_backend(void) {
  const char *name = "cs_net_get_api";
  uint32_t hash = 5381, address = 0;
  while (*name)
    hash = hash * 33 + (unsigned char)*name++;
  if (!firmware_api_interface->resolver_callback(firmware_api_interface, hash,
                                                 &address))
    return NULL;
  const CsNetApi *(*get_api)(void) = (void *)(uintptr_t)address;
  const CsNetApi *api = get_api();
  return api && api->abi == CS_NET_ABI && api->size >= sizeof(CsNetApi) ? api
                                                                        : NULL;
}
