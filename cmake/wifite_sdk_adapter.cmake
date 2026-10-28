# Keep the modified SDK translation unit in the build directory, and compile
# it with the original wpa_supplicant target's definitions/includes.
idf_component_get_property(_wps_lib wpa_supplicant COMPONENT_LIB)
idf_component_get_property(_wps_dir wpa_supplicant COMPONENT_DIR)
set(_wps_original "${_wps_dir}/esp_supplicant/src/esp_wps.c")
set(_wps_generated "${CMAKE_BINARY_DIR}/wifite_sdk/esp_wps.c")
execute_process(COMMAND "${PYTHON}" "${CMAKE_SOURCE_DIR}/tools/wifite_sdk_adapter.py"
    "${_wps_original}" "${_wps_generated}"
    RESULT_VARIABLE _wps_rc OUTPUT_VARIABLE _wps_out ERROR_VARIABLE _wps_err)
if(NOT _wps_rc EQUAL 0)
    message(FATAL_ERROR "WIFITE SDK adapter failed: ${_wps_err}")
endif()
get_target_property(_wps_sources ${_wps_lib} SOURCES)
set(_wps_found FALSE)
set(_wps_updated "")
foreach(_source IN LISTS _wps_sources)
    if(_source MATCHES "(^|/)esp_supplicant/src/esp_wps\\.c$")
        list(APPEND _wps_updated "${_wps_generated}")
        set(_wps_found TRUE)
    else()
        list(APPEND _wps_updated "${_source}")
    endif()
endforeach()
if(NOT _wps_found)
    message(FATAL_ERROR "Cannot locate the SDK WPS source on its component target")
endif()
set_property(TARGET ${_wps_lib} PROPERTY SOURCES "${_wps_updated}")
target_include_directories(${_wps_lib} PRIVATE
    "${CMAKE_SOURCE_DIR}/components/wifi/wifite")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_wps_original}" "${CMAKE_SOURCE_DIR}/tools/wifite_sdk_adapter.py")
message(STATUS "${_wps_out}")
