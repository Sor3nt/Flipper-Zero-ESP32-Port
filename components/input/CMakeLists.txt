# Target-specific input driver selection
if(NOT DEFINED FLIPPER_BOARD)
    set(FLIPPER_BOARD "waveshare_c6_1.9" CACHE STRING "Target board")
endif()

set(TARGET_INPUT_DIR "${CMAKE_CURRENT_LIST_DIR}/../../targets/${FLIPPER_BOARD}")

if(NOT EXISTS "${TARGET_INPUT_DIR}/target_input.c")
    message(FATAL_ERROR "No target_input.c found for board '${FLIPPER_BOARD}' at ${TARGET_INPUT_DIR}")
endif()

set(INPUT_SRCS "input.c" "${TARGET_INPUT_DIR}/target_input.c")

# Optional per-board BMI270 IMU (tilt-to-wake). Only boards that ship the driver
# (currently m5stick_s3) get it compiled in; other boards build unchanged.
if(EXISTS "${TARGET_INPUT_DIR}/bmi270.c")
    list(APPEND INPUT_SRCS "${TARGET_INPUT_DIR}/bmi270.c")
endif()

idf_component_register(
    SRCS ${INPUT_SRCS}
    INCLUDE_DIRS "." "${CMAKE_CURRENT_LIST_DIR}/../../targets"
    REQUIRES furi
    PRIV_REQUIRES furi_hal
)
