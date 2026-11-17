idf_build_get_property(target IDF_TARGET)

set(srcs)

set(public_include "include")

if(CONFIG_SOC_GPSPI_SUPPORTED)
    list(APPEND srcs "src/sdspi_crc.c"
                     "src/sdspi_host.c"
                     "src/sdspi_transaction.c")
endif()

if(${target} STREQUAL "linux")
    set(requires "")
    set(priv_requires esp_timer)
else()
    set(requires sdmmc esp_driver_spi esp_driver_gpio)
    set(priv_requires esp_timer)
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS ${public_include}
                       REQUIRES "${requires}"
                       PRIV_REQUIRES "${priv_requires}"
                      )

# LOCAL: this component overrides ESP-IDF's sdspi for EVERY board, but the
# "skip the idle-MISO ready poll" change in sdspi_host.c is only meant for boards
# whose shared MISO idles low (see the note at the top of that file). Everywhere
# else keep the stock IDF behaviour: on boards with a working MISO pull-up a single
# 40 ms timeout (a card busy with a long write/erase, e.g. under USB mass storage)
# would otherwise disable the poll until the next reboot.
set(SDSPI_IDLE_MISO_LOW_BOARDS fatalcore)
if(DEFINED FLIPPER_BOARD AND FLIPPER_BOARD IN_LIST SDSPI_IDLE_MISO_LOW_BOARDS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SDSPI_LOCAL_SKIP_IDLE_READY_POLL=1)
endif()
