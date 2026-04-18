idf_build_get_property(target IDF_TARGET)
set(IDF_VERSION "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}.${IDF_VERSION_PATCH}")

set(srcs
    "descriptors_control.c"
    "tinyusb.c"
    "usb_descriptors.c"
    "tinyusb_task.c"
    )

set(priv_req "")

# USB-PHY driver
# For IDF 6.x and later, usb_phy driver is moved from usb to esp_hw_support
if(IDF_VERSION VERSION_LESS "6.0.0")
    list(APPEND priv_req usb)
else()
    list(APPEND priv_req esp_hw_support)
endif()

if(CONFIG_TINYUSB_CDC_ENABLED)
    list(APPEND srcs
        "cdc.c"
        "tinyusb_cdc_acm.c"
        )
    if(CONFIG_VFS_SUPPORT_IO)
        list(APPEND srcs
            "tinyusb_console.c"
            "vfs_tinyusb.c"
            )
    endif() # CONFIG_VFS_SUPPORT_IO
endif() # CONFIG_TINYUSB_CDC_ENABLED

if(CONFIG_TINYUSB_MSC_ENABLED)
    list(APPEND srcs
        "tinyusb_msc.c"
        "storage_spiflash.c"
        )
    if(CONFIG_SOC_SDMMC_HOST_SUPPORTED)
        list(APPEND srcs
            "storage_sdmmc.c"
            )
    endif() # CONFIG_SOC_SDMMC_HOST_SUPPORTED
endif() # CONFIG_TINYUSB_MSC_ENABLED


if(CONFIG_TINYUSB_NET_MODE_NCM)
    list(APPEND srcs
         "tinyusb_net.c"
         )
endif() # CONFIG_TINYUSB_NET_MODE_NCM

# Software VBUS monitoring is available only on esp32p4
if(${target} STREQUAL "esp32p4")
    list(APPEND srcs "tinyusb_vbus_monitor.c")

    # VBUS monitoring on ESP32-P4 requires GPIO driver
    if(IDF_VERSION VERSION_GREATER_EQUAL "5.3")
        list(APPEND priv_req esp_driver_gpio)
    else()
        list(APPEND priv_req driver)
    endif()
endif() # esp32p4

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "include_private"
                       PRIV_REQUIRES ${priv_req}
                       REQUIRES fatfs vfs
                       )

# Determine whether tinyusb is fetched from component registry or from local path
idf_build_get_property(build_components BUILD_COMPONENTS)
if(tinyusb IN_LIST build_components)
    set(tinyusb_name tinyusb) # Local component
else()
    set(tinyusb_name espressif__tinyusb) # Managed component
endif()

# Pass tusb_config.h from this component to TinyUSB
idf_component_get_property(tusb_lib ${tinyusb_name} COMPONENT_LIB)
target_include_directories(${tusb_lib} PRIVATE "include")
