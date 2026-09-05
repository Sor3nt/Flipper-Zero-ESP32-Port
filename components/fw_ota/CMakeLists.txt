# Gemeinsamer OTA-Kern (Image-Inspektion, esp_ota-Flash, FW-Marker,
# Post-Reboot-Flags). Genutzt vom WiFi-Self-Update (wlan_app), vom RPC-Handler
# (qT-Embed/qFlipper "system_update") und vom OTA-Updater-System-App.
idf_component_register(
    SRCS "fw_ota.c"
    INCLUDE_DIRS "." ".."
    REQUIRES furi storage toolbox app_update esp_partition esp_app_format bootloader_support esp_system soc
)
