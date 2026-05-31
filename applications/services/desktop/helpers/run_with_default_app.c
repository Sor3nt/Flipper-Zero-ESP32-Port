#include <loader/loader.h>
#include <storage/storage.h>

void run_with_default_app(const char* path) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FileInfo info;
    bool is_dir = (storage_common_stat(storage, path, &info) == FSE_OK) &&
                  (info.flags & FSF_DIRECTORY);
    furi_record_close(RECORD_STORAGE);

    Loader* loader = furi_record_open(RECORD_LOADER);
    if(is_dir) {
        loader_start_detached_with_gui_error(loader, "Archive", NULL);
    } else {
        loader_start_detached_with_gui_error(loader, "Archive", path);
    }
    furi_record_close(RECORD_LOADER);
}
