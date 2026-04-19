#include "loader.h"
#include "loader_i.h"
#include <applications.h>
#include <flipper_application/flipper_application.h>
#include <flipper_application/api_hashtable/api_hashtable.h>
#include <storage/storage.h>
#include <toolbox/path.h>

extern const ElfApiInterface* const firmware_api_interface;

#define TAG "Loader"

#define LOADER_MAGIC_THREAD_VALUE 0xDEADBEEF

// API

static LoaderMessageLoaderStatusResult loader_start_internal(
    Loader* loader,
    const char* name,
    const char* args,
    FuriString* error_message) {
    LoaderMessage message;
    LoaderMessageLoaderStatusResult result;

    message.type = LoaderMessageTypeStartByName;
    message.start.name = name;
    message.start.args = args;
    message.start.error_message = error_message;
    message.api_lock = api_lock_alloc_locked();
    message.status_value = &result;
    furi_message_queue_put(loader->queue, &message, FuriWaitForever);
    api_lock_wait_unlock_and_free(message.api_lock);

    return result;
}

LoaderStatus loader_start(
    Loader* loader,
    const char* name,
    const char* args,
    FuriString* error_message) {
    furi_check(loader);
    furi_check(name);

    LoaderMessageLoaderStatusResult result =
        loader_start_internal(loader, name, args, error_message);
    return result.value;
}

LoaderStatus
    loader_start_with_gui_error(Loader* loader, const char* name, const char* args) {
    furi_check(loader);
    furi_check(name);

    FuriString* error_message = furi_string_alloc();
    LoaderMessageLoaderStatusResult result =
        loader_start_internal(loader, name, args, error_message);
    if(result.value != LoaderStatusOk) {
        FURI_LOG_E(TAG, "Start error: %s", furi_string_get_cstr(error_message));
    }
    furi_string_free(error_message);
    return result.value;
}

void loader_start_detached_with_gui_error(
    Loader* loader,
    const char* name,
    const char* args) {
    furi_check(loader);
    furi_check(name);

    LoaderMessage message = {
        .type = LoaderMessageTypeStartByNameDetachedWithGuiError,
        .start.name = strdup(name),
        .start.args = args ? strdup(args) : NULL,
    };
    furi_message_queue_put(loader->queue, &message, FuriWaitForever);
}

static void
    loader_generic_synchronous_request(Loader* loader, LoaderMessage* message) {
    furi_check(loader);
    message->api_lock = api_lock_alloc_locked();
    furi_message_queue_put(loader->queue, message, FuriWaitForever);
    api_lock_wait_unlock_and_free(message->api_lock);
}

bool loader_lock(Loader* loader) {
    LoaderMessageBoolResult result;
    LoaderMessage message = {
        .type = LoaderMessageTypeLock,
        .bool_value = &result,
    };
    loader_generic_synchronous_request(loader, &message);
    return result.value;
}

void loader_unlock(Loader* loader) {
    furi_check(loader);

    LoaderMessage message;
    message.type = LoaderMessageTypeUnlock;

    furi_message_queue_put(loader->queue, &message, FuriWaitForever);
}

bool loader_is_locked(Loader* loader) {
    LoaderMessageBoolResult result;
    LoaderMessage message = {
        .type = LoaderMessageTypeIsLocked,
        .bool_value = &result,
    };
    loader_generic_synchronous_request(loader, &message);
    return result.value;
}

void loader_show_menu(Loader* loader) {
    furi_check(loader);

    LoaderMessage message;
    message.type = LoaderMessageTypeShowMenu;

    furi_message_queue_put(loader->queue, &message, FuriWaitForever);
}

FuriPubSub* loader_get_pubsub(Loader* loader) {
    furi_check(loader);
    return loader->pubsub;
}

void loader_enqueue_launch(
    Loader* instance,
    const char* name,
    const char* args,
    LoaderDeferredLaunchFlag flags) {
    UNUSED(flags);
    /* ESP32 port: no deferred launch queue, just start directly */
    loader_start_detached_with_gui_error(instance, name, args);
}

bool loader_get_application_launch_path(Loader* instance, FuriString* path) {
    UNUSED(instance);
    UNUSED(path);
    /* ESP32 port: no FAP paths */
    return false;
}

// callbacks

static void loader_menu_closed_callback(void* context) {
    Loader* loader = context;
    LoaderMessage message;
    message.type = LoaderMessageTypeMenuClosed;
    furi_message_queue_put(loader->queue, &message, FuriWaitForever);
}

static void loader_applications_closed_callback(void* context) {
    Loader* loader = context;
    LoaderMessage message;
    message.type = LoaderMessageTypeApplicationsClosed;
    furi_message_queue_put(loader->queue, &message, FuriWaitForever);
}

static void loader_thread_state_callback(
    FuriThread* thread,
    FuriThreadState thread_state,
    void* context) {
    UNUSED(thread);
    furi_assert(context);

    if(thread_state == FuriThreadStateStopped) {
        Loader* loader = context;
        LoaderMessage message;
        message.type = LoaderMessageTypeAppClosed;
        furi_message_queue_put(loader->queue, &message, FuriWaitForever);
    }
}

// implementation

static Loader* loader_alloc(void) {
    Loader* loader = malloc(sizeof(Loader));
    memset(loader, 0, sizeof(Loader));
    loader->pubsub = furi_pubsub_alloc();
    loader->queue = furi_message_queue_alloc(1, sizeof(LoaderMessage));
    loader->gui = furi_record_open(RECORD_GUI);
    loader->view_holder = view_holder_alloc();
    loader->loading = loading_alloc();
    view_holder_attach_to_gui(loader->view_holder, loader->gui);
    return loader;
}

static const FlipperInternalApplication*
    loader_find_application_by_name(const char* name) {
    const struct {
        const FlipperInternalApplication* list;
        const size_t count;
    } lists[] = {
        {FLIPPER_APPS, FLIPPER_APPS_COUNT},
        {FLIPPER_SETTINGS_APPS, FLIPPER_SETTINGS_APPS_COUNT},
        {FLIPPER_SYSTEM_APPS, FLIPPER_SYSTEM_APPS_COUNT},
        {FLIPPER_DEBUG_APPS, FLIPPER_DEBUG_APPS_COUNT},
    };

    for(size_t i = 0; i < COUNT_OF(lists); i++) {
        for(size_t j = 0; j < lists[i].count; j++) {
            if((strcmp(name, lists[i].list[j].name) == 0) ||
               (strcmp(name, lists[i].list[j].appid) == 0)) {
                return &lists[i].list[j];
            }
        }
    }

    // Check FLIPPER_ARCHIVE separately (not always in SYSTEM_APPS array)
    if((strcmp(name, FLIPPER_ARCHIVE.name) == 0) ||
       (strcmp(name, FLIPPER_ARCHIVE.appid) == 0)) {
        return &FLIPPER_ARCHIVE;
    }

    return NULL;
}

static void loader_start_internal_app(
    Loader* loader,
    const FlipperInternalApplication* app,
    const char* args) {
    FURI_LOG_I(TAG, "Starting %s", app->name);

    furi_assert(loader->app.args == NULL);
    if(args && strlen(args) > 0) {
        loader->app.args = strdup(args);
    }

    loader->app.thread =
        furi_thread_alloc_ex(app->name, app->stack_size, app->app, loader->app.args);

    furi_thread_set_appid(loader->app.thread, app->appid);
    furi_thread_set_state_context(loader->app.thread, loader);
    furi_thread_set_state_callback(loader->app.thread, loader_thread_state_callback);

    furi_thread_start(loader->app.thread);
}

static bool loader_is_external_fap_path(const char* name) {
    furi_check(name);

    const char* extension = strrchr(name, '.');
    return extension && (strcmp(extension, ".fap") == 0);
}

static LoaderStatus loader_start_external_fap(
    Loader* loader,
    const char* path,
    const char* args,
    FuriString* error_message) {
    LoaderStatus status = LoaderStatusErrorInternal;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperApplication* app = flipper_application_alloc(storage, firmware_api_interface);

    do {
        if(!app) {
            if(error_message) {
                furi_string_set(error_message, "Failed to allocate FAP loader");
            }
            break;
        }

        FURI_LOG_I(TAG, "Loading FAP %s", path);

        FlipperApplicationPreloadStatus preload_status =
            flipper_application_preload(app, path);

        if(preload_status != FlipperApplicationPreloadStatusSuccess) {
            const FlipperApplicationManifest* manifest = flipper_application_get_manifest(app);
            if(error_message) {
                furi_string_printf(
                    error_message,
                    "Preload failed for \"%s\": %s",
                    manifest->name[0] ? manifest->name : path,
                    flipper_application_preload_status_to_string(preload_status));
            }
            FURI_LOG_E(
                TAG,
                "Preload failed for %s: %s",
                path,
                flipper_application_preload_status_to_string(preload_status));

            if(preload_status == FlipperApplicationPreloadStatusApiTooOld ||
               preload_status == FlipperApplicationPreloadStatusApiTooNew) {
                status = LoaderStatusErrorInternal;
            }
            break;
        }

        FURI_LOG_I(TAG, "Mapping FAP to memory");

        FlipperApplicationLoadStatus load_status =
            flipper_application_map_to_memory(app);

        if(load_status != FlipperApplicationLoadStatusSuccess) {
            if(error_message) {
                furi_string_printf(
                    error_message,
                    "Load failed: %s",
                    flipper_application_load_status_to_string(load_status));
            }
            FURI_LOG_E(
                TAG,
                "Load failed for %s: %s",
                path,
                flipper_application_load_status_to_string(load_status));
            break;
        }

        FURI_LOG_I(TAG, "Starting FAP thread");

        loader->app.fap = app;
        FuriThread* thread = flipper_application_alloc_thread(app, args);

        if(!thread) {
            if(error_message) {
                furi_string_set(error_message, "Failed to allocate FAP thread");
            }
            FURI_LOG_E(TAG, "Failed to allocate thread for %s", path);
            loader->app.fap = NULL;
            break;
        }

        FuriString* app_name = furi_string_alloc();
        path_extract_filename_no_ext(path, app_name);
        furi_thread_set_appid(thread, furi_string_get_cstr(app_name));
        furi_string_free(app_name);

        loader->app.thread = thread;

        /* Start the FAP thread (internal apps use loader_start_app_thread,
         * but for FAPs we start directly here) */
        furi_thread_set_state_callback(thread, loader_thread_state_callback);
        furi_thread_set_state_context(thread, loader);
        furi_thread_start(thread);

        FURI_LOG_I(TAG, "FAP thread started");
        status = LoaderStatusOk;

    } while(0);

    if(status != LoaderStatusOk) {
        if(app) {
            flipper_application_free(app);
        }
    }

    furi_record_close(RECORD_STORAGE);
    return status;
}

static void loader_do_menu_show(Loader* loader) {
    if(!loader->loader_menu) {
        loader->loader_menu =
            loader_menu_alloc(loader_menu_closed_callback, loader);
    }
}

static void loader_do_menu_closed(Loader* loader) {
    if(loader->loader_menu) {
        loader_menu_free(loader->loader_menu);
        loader->loader_menu = NULL;
    }
}

static void loader_do_applications_show(Loader* loader) {
    if(!loader->loader_applications) {
        loader->loader_applications =
            loader_applications_alloc(loader_applications_closed_callback, loader);
    }
}

static void loader_do_applications_closed(Loader* loader) {
    if(loader->loader_applications) {
        loader_applications_free(loader->loader_applications);
        loader->loader_applications = NULL;
    }
}

static bool loader_do_is_locked(Loader* loader) {
    return loader->app.thread != NULL;
}

static LoaderMessageLoaderStatusResult loader_do_start_by_name(
    Loader* loader,
    const char* name,
    const char* args,
    FuriString* error_message) {
    LoaderMessageLoaderStatusResult status;
    status.value = LoaderStatusOk;

    esp_rom_printf("\r\n[LDR] start_by_name name='%s'\r\n", name ? name : "(null)");

    if(name == NULL) return status;

    do {
        if(loader_do_is_locked(loader)) {
            status.value = LoaderStatusErrorAppStarted;
            if(error_message) {
                furi_string_set(error_message, "Loader is locked");
            }
            FURI_LOG_E(TAG, "Loader is locked");
            break;
        }

        if(strcmp(name, LOADER_APPLICATIONS_NAME) == 0) {
            loader_do_applications_show(loader);
            break;
        }

        LoaderEvent event;
        event.type = LoaderEventTypeApplicationBeforeLoad;
        furi_pubsub_publish(loader->pubsub, &event);

        const FlipperInternalApplication* app =
            loader_find_application_by_name(name);
        esp_rom_printf("[LDR] find_app('%s')=%p\r\n", name, (void*)app);
        if(app) {
            esp_rom_printf("[LDR] found name='%s' appid='%s' stack=%u\r\n",
                app->name, app->appid, (unsigned)app->stack_size);
            loader_start_internal_app(loader, app, args);
            break;
        }

        if(loader_is_external_fap_path(name)) {
            status.value = loader_start_external_fap(loader, name, args, error_message);
            break;
        }

        status.value = LoaderStatusErrorUnknownApp;
        if(error_message) {
            furi_string_printf(
                error_message, "Application \"%s\" not found", name);
        }
        FURI_LOG_E(TAG, "Application \"%s\" not found", name);
    } while(false);

    return status;
}

static bool loader_do_lock(Loader* loader) {
    if(loader->app.thread) {
        return false;
    }
    loader->app.thread = (FuriThread*)LOADER_MAGIC_THREAD_VALUE;
    return true;
}

static void loader_do_unlock(Loader* loader) {
    furi_check(loader->app.thread == (FuriThread*)LOADER_MAGIC_THREAD_VALUE);
    loader->app.thread = NULL;
}

static void loader_do_app_closed(Loader* loader) {
    furi_assert(loader->app.thread);

    furi_thread_join(loader->app.thread);
    FURI_LOG_I(
        TAG, "App returned: %li", furi_thread_get_return_code(loader->app.thread));

    if(loader->app.args) {
        free(loader->app.args);
        loader->app.args = NULL;
    }

    if(loader->app.fap) {
        flipper_application_free(loader->app.fap);
        loader->app.fap = NULL;
    } else {
        furi_thread_free(loader->app.thread);
    }
    loader->app.thread = NULL;

    FURI_LOG_I(
        TAG, "Application stopped. Free heap: %zu", memmgr_get_free_heap());

    LoaderEvent event;
    event.type = LoaderEventTypeApplicationStopped;
    furi_pubsub_publish(loader->pubsub, &event);

    // Emit queue empty since we don't have a deferred launch queue
    LoaderEvent empty_event;
    empty_event.type = LoaderEventTypeNoMoreAppsInQueue;
    furi_pubsub_publish(loader->pubsub, &empty_event);
}

// app

int32_t loader_srv(void* p) {
    UNUSED(p);
    Loader* loader = loader_alloc();
    furi_record_create(RECORD_LOADER, loader);

    FURI_LOG_I(TAG, "Loader service started");

    LoaderMessage message;
    while(true) {
        if(furi_message_queue_get(loader->queue, &message, FuriWaitForever) ==
           FuriStatusOk) {
            switch(message.type) {
            case LoaderMessageTypeStartByName: {
                LoaderMessageLoaderStatusResult status = loader_do_start_by_name(
                    loader,
                    message.start.name,
                    message.start.args,
                    message.start.error_message);
                *(message.status_value) = status;
                api_lock_unlock(message.api_lock);
                break;
            }
            case LoaderMessageTypeStartByNameDetachedWithGuiError: {
                FuriString* error_message = furi_string_alloc();
                LoaderMessageLoaderStatusResult status = loader_do_start_by_name(
                    loader,
                    message.start.name,
                    message.start.args,
                    error_message);
                if(status.value != LoaderStatusOk) {
                    FURI_LOG_E(
                        TAG,
                        "Detached start error: %s",
                        furi_string_get_cstr(error_message));
                }
                if(message.start.name) free((void*)message.start.name);
                if(message.start.args) free((void*)message.start.args);
                furi_string_free(error_message);
                break;
            }
            case LoaderMessageTypeShowMenu:
                loader_do_menu_show(loader);
                break;
            case LoaderMessageTypeMenuClosed:
                loader_do_menu_closed(loader);
                break;
            case LoaderMessageTypeApplicationsClosed:
                loader_do_applications_closed(loader);
                break;
            case LoaderMessageTypeIsLocked:
                message.bool_value->value = loader_do_is_locked(loader);
                api_lock_unlock(message.api_lock);
                break;
            case LoaderMessageTypeAppClosed:
                loader_do_app_closed(loader);
                break;
            case LoaderMessageTypeLock:
                message.bool_value->value = loader_do_lock(loader);
                api_lock_unlock(message.api_lock);
                break;
            case LoaderMessageTypeUnlock:
                loader_do_unlock(loader);
                break;
            }
        }
    }

    return 0;
}

void loader_on_system_start(void) {
}
