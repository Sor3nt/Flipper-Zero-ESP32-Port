#include "dialogs_message.h"
#include <gui/modules/file_browser.h>
#include <toolbox/api_lock.h>
#include <esp_rom_sys.h>

typedef struct {
    FuriApiLock lock;
    bool result;
} DialogsAppFileBrowserContext;

static void dialogs_file_browser_trace(const char* step) {
    esp_rom_printf(
        "\r\n[DFB] %s free=%u\r\n",
        step,
        (unsigned)furi_thread_get_stack_space(furi_thread_get_current_id()));
}

static void dialogs_app_file_browser_back_callback(void* context) {
    furi_assert(context);
    DialogsAppFileBrowserContext* file_browser_context = context;
    file_browser_context->result = false;
    api_lock_unlock(file_browser_context->lock);
}

static void dialogs_app_file_browser_callback(void* context) {
    furi_assert(context);
    DialogsAppFileBrowserContext* file_browser_context = context;
    file_browser_context->result = true;
    api_lock_unlock(file_browser_context->lock);
}

bool dialogs_app_process_module_file_browser(const DialogsAppMessageDataFileBrowser* data) {
    bool ret = false;
    dialogs_file_browser_trace("start");
    Gui* gui = furi_record_open(RECORD_GUI);

    DialogsAppFileBrowserContext* file_browser_context =
        malloc(sizeof(DialogsAppFileBrowserContext));
    file_browser_context->lock = api_lock_alloc_locked();

    ViewHolder* view_holder = view_holder_alloc();
    view_holder_attach_to_gui(view_holder, gui);
    view_holder_set_back_callback(
        view_holder, dialogs_app_file_browser_back_callback, file_browser_context);

    FileBrowser* file_browser = file_browser_alloc(data->result_path);
    dialogs_file_browser_trace("browser_alloc");
    file_browser_set_callback(
        file_browser, dialogs_app_file_browser_callback, file_browser_context);
    file_browser_configure(
        file_browser,
        data->extension,
        data->base_path,
        data->skip_assets,
        data->hide_dot_files,
        data->file_icon,
        data->hide_ext);
    file_browser_set_item_callback(file_browser, data->item_callback, data->item_callback_context);
    file_browser_start(file_browser, data->preselected_filename);
    dialogs_file_browser_trace("browser_started");

    view_holder_set_view(view_holder, file_browser_get_view(file_browser));
    api_lock_wait_unlock(file_browser_context->lock);
    dialogs_file_browser_trace("wait_done");

    ret = file_browser_context->result;

    view_holder_set_view(view_holder, NULL);
    file_browser_stop(file_browser);

    file_browser_free(file_browser);
    view_holder_free(view_holder);

    api_lock_free(file_browser_context->lock);
    free(file_browser_context);

    furi_record_close(RECORD_GUI);
    dialogs_file_browser_trace("end");

    return ret;
}
