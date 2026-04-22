idf_build_get_property(target IDF_TARGET)

if(${target} STREQUAL "esp32s3")
    set(tusb_mcu "OPT_MCU_ESP32S3")
elseif(${target} STREQUAL "esp32s2")
    set(tusb_mcu "OPT_MCU_ESP32S2")
elseif(${target} STREQUAL "esp32p4")
    set(tusb_mcu "OPT_MCU_ESP32P4")
elseif(${target} STREQUAL "esp32h4")
    set(tusb_mcu "OPT_MCU_ESP32H4")
elseif(${target} STREQUAL "esp32s31")
    set(tusb_mcu "OPT_MCU_ESP32S31")
endif()

set(compile_options
    "-DCFG_TUSB_MCU=${tusb_mcu}"
    )

set(includes_private
    "src/"
    "src/device"
    "lib/networking" # For RNDIS definitions
    )

set(includes_public
    "src/"
    )

set(srcs
    "src/class/cdc/cdc_device.c"
    "src/class/hid/hid_device.c"
    "src/class/midi/midi_device.c"
    "src/class/msc/msc_device.c"
    "src/class/vendor/vendor_device.c"
    "src/class/audio/audio_device.c"
    "src/class/video/video_device.c"
    "src/class/bth/bth_device.c"
    "src/class/usbtmc/usbtmc_device.c"
    # NET class
    "src/class/net/ecm_rndis_device.c"
    "lib/networking/rndis_reports.c"
    "src/class/net/ncm_device.c"
    # DFU
    "src/class/dfu/dfu_device.c"
    "src/class/dfu/dfu_rt_device.c"
    # Common, device-mode related
    "src/portable/synopsys/dwc2/dcd_dwc2.c"
    "src/portable/synopsys/dwc2/dwc2_common.c"
    "src/common/tusb_fifo.c"
    "src/device/usbd_control.c"
    "src/device/usbd.c"
    "src/tusb.c"
    )

set(requirements_private
    esp_netif   # required by rndis_reports.c: #include "netif/ethernet.h"
    )

if(${target} STREQUAL "esp32p4")
    list(APPEND requirements_private
                esp_mm # required by dcd_dwc2.c: #include "esp_cache.h"
        )
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS ${includes_public}
                       PRIV_INCLUDE_DIRS ${includes_private}
                       PRIV_REQUIRES  ${requirements_private}
                       REQUIRES freertos
                       )

target_compile_options(${COMPONENT_LIB} PUBLIC ${compile_options})

# when no builtin class driver is enabled, an uint8_t data compared with `BUILTIN_DRIVER_COUNT` will always be false
set_source_files_properties("src/device/usbd.c" PROPERTIES COMPILE_FLAGS "-Wno-type-limits")
