/**
 * @file plugin_manager.c
 * @brief Stub plugin manager for ESP32 port.
 *
 * ESP32 port does not support loading external FAP plugins.
 * This stub allows code that references the plugin manager to compile.
 */

#include "plugin_manager.h"
#include <furi.h>
#include <stdlib.h>

struct PluginManager {
    const char* application_id;
    uint32_t api_version;
};

PluginManager* plugin_manager_alloc(
    const char* application_id,
    uint32_t api_version,
    const ElfApiInterface* api_interface) {
    UNUSED(api_interface);
    PluginManager* manager = malloc(sizeof(PluginManager));
    manager->application_id = application_id;
    manager->api_version = api_version;
    return manager;
}

void plugin_manager_free(PluginManager* manager) {
    free(manager);
}

PluginManagerError plugin_manager_load_single(PluginManager* manager, const char* path) {
    UNUSED(manager);
    UNUSED(path);
    return PluginManagerErrorLoaderError;
}

PluginManagerError plugin_manager_load_all(PluginManager* manager, const char* path) {
    UNUSED(manager);
    UNUSED(path);
    return PluginManagerErrorNone;
}

uint32_t plugin_manager_get_count(PluginManager* manager) {
    UNUSED(manager);
    return 0;
}

const FlipperAppPluginDescriptor* plugin_manager_get(PluginManager* manager, uint32_t index) {
    UNUSED(manager);
    UNUSED(index);
    return NULL;
}

const void* plugin_manager_get_ep(PluginManager* manager, uint32_t index) {
    UNUSED(manager);
    UNUSED(index);
    return NULL;
}
