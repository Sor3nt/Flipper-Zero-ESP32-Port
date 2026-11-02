if(NOT DEFINED FLIPPER_BOARD OR FLIPPER_BOARD STREQUAL "")
    set(_flipper_board "")

    if(DEFINED BUILD_DIR AND EXISTS "${BUILD_DIR}/CMakeCache.txt")
        file(STRINGS "${BUILD_DIR}/CMakeCache.txt" _flipper_board_cache
            REGEX "^FLIPPER_BOARD:[^=]*=")
        if(_flipper_board_cache)
            string(REGEX REPLACE "^FLIPPER_BOARD:[^=]*=" ""
                _flipper_board "${_flipper_board_cache}")
        endif()
    endif()

    if(NOT _flipper_board AND DEFINED ENV{FLIPPER_BOARD})
        set(_flipper_board "$ENV{FLIPPER_BOARD}")
    endif()

    if(NOT _flipper_board)
        if(IDF_TARGET STREQUAL "esp32s3")
            set(_flipper_board "smoochie")
        else()
            set(_flipper_board "waveshare_c6_1.9")
        endif()
    endif()

    set(FLIPPER_BOARD "${_flipper_board}")
endif()
