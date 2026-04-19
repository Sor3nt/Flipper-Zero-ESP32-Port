idf_component_register(
    SRCS
        "archive/archive.c"
        "archive/helpers/archive_apps.c"
        "archive/helpers/archive_browser.c"
        "archive/helpers/archive_favorites.c"
        "archive/helpers/archive_files.c"
        "archive/scenes/archive_scene.c"
        "archive/scenes/archive_scene_browser.c"
        "archive/scenes/archive_scene_delete.c"
        "archive/scenes/archive_scene_info.c"
        "archive/scenes/archive_scene_new_dir.c"
        "archive/scenes/archive_scene_rename.c"
        "archive/scenes/archive_scene_show.c"
        "archive/views/archive_browser_view.c"
    INCLUDE_DIRS "."
    REQUIRES furi furi_hal gui toolbox assets storage loader dialogs flipper_application
)
