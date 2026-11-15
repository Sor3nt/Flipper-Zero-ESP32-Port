# Asset pack name -> icon table, generated from assets_icons.h so it always
# matches the compiled icons (see gen_icon_names.py).
set(ICON_NAMES_C "${CMAKE_CURRENT_BINARY_DIR}/assets_icon_names.c")
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_LIST_DIR}/assets_icons.h"
        "${CMAKE_CURRENT_LIST_DIR}/gen_icon_names.py")
    execute_process(
        COMMAND ${PYTHON} "${CMAKE_CURRENT_LIST_DIR}/gen_icon_names.py" "${ICON_NAMES_C}"
        RESULT_VARIABLE ICON_NAMES_RESULT
        OUTPUT_VARIABLE ICON_NAMES_OUT
        ERROR_VARIABLE ICON_NAMES_ERR)
    if(NOT ICON_NAMES_RESULT EQUAL 0)
        message(FATAL_ERROR "Icon name table generation failed:\n${ICON_NAMES_OUT}\n${ICON_NAMES_ERR}")
    endif()
endif()

idf_component_register(
    SRCS "assets_icons.c" "${ICON_NAMES_C}"
    INCLUDE_DIRS "."
    REQUIRES gui
)
