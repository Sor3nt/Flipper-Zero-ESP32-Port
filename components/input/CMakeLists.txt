# Target-specific input driver selection
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/flipper_board.cmake")

set(TARGET_INPUT_DIR "${CMAKE_CURRENT_LIST_DIR}/../../targets/${FLIPPER_BOARD}")

if(NOT EXISTS "${TARGET_INPUT_DIR}/target_input.c")
    message(FATAL_ERROR "No target_input.c found for board '${FLIPPER_BOARD}' at ${TARGET_INPUT_DIR}")
endif()

idf_component_register(
    SRCS
        "input.c"
        "${TARGET_INPUT_DIR}/target_input.c"
    INCLUDE_DIRS "." "${CMAKE_CURRENT_LIST_DIR}/../../targets"
    REQUIRES furi
    PRIV_REQUIRES furi_hal esp_rom loader
)
