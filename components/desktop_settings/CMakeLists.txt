# The real Desktop settings app is built as esp32_fam_app_desktop_settings from
# applications/settings/desktop_settings. This component stays for FAM discovery only.
idf_component_register(
    SRCS "desktop_settings_component.c"
    INCLUDE_DIRS "."
)
