add_library(bloom_openh264_shim SHARED openh264_shim.cpp)
set_target_properties(
    bloom_openh264_shim
    PROPERTIES
        OUTPUT_NAME openh264
        SOVERSION 8
        VERSION 8.0.0
        LIBRARY_OUTPUT_DIRECTORY "${BLOOM_OPENH264_SHIM_OUTPUT_DIR}")
target_link_options(
    bloom_openh264_shim PRIVATE
    "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exports.map")
bloom_enable_warnings(bloom_openh264_shim)
