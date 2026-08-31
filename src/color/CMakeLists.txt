if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.25)
    project(BloomColor LANGUAGES CXX)
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/../../cmake")
    include(CTest)
    include(BloomQuality)
    include(BloomWarnings)
    bloom_configure_quality_tools()
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../core" "${CMAKE_CURRENT_BINARY_DIR}/core")
endif()

add_library(bloom_color STATIC)

target_sources(
    bloom_color
    PRIVATE
        display_processor_identity.cpp
        display_processor_identity_ownership.cpp
        ocio_content_revision.cpp
        ocio_loose_content_revision.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bloom/color/bloom_neutral_builtin.hpp
            include/bloom/color/display_processor_identity.hpp
            include/bloom/color/ocio_content_revision.hpp
            include/bloom/color/ocio_loose_content_revision.hpp
)

target_compile_features(bloom_color PUBLIC cxx_std_20)
target_link_libraries(bloom_color PUBLIC bloom_core)
bloom_enable_warnings(bloom_color)

# bloom_color_ocio (issue #95): the in-process Bloom Neutral CPU display processor adapter.
# Deliberately a separate target from bloom_color above -- bloom_color stays dependency-light
# (bloom_core only); only this target links the heavier OpenColorIO dependency. Public headers
# expose Bloom value types and bloom_render image views only; no OCIO type, pointer, enum, or
# exception ever appears in a public header (contract "CPU Display Processor Boundary") -- the
# real OCIO types live only in ocio_internal.hpp, a private (non-FILE_SET, non-installed) header
# shared by this target's own translation units.
bloom_find_dependency(OpenColorIO)

# Configure-time embed of the immutable Bloom Neutral v1 payload into a C++ array -- no runtime
# filesystem read, per design decision 2. The generated file lives under the build tree, never the
# source tree, and is included only by ocio_builtin_registry.cpp.
set(bloom_neutral_v1_asset "${CMAKE_CURRENT_SOURCE_DIR}/../../assets/ocio/neutral-v1/config.ocio")
cmake_path(NORMAL_PATH bloom_neutral_v1_asset)
file(READ "${bloom_neutral_v1_asset}" bloom_neutral_v1_payload_hex HEX)
string(LENGTH "${bloom_neutral_v1_payload_hex}" bloom_neutral_v1_payload_hex_length)
math(EXPR bloom_neutral_v1_payload_byte_count "${bloom_neutral_v1_payload_hex_length} / 2")
if(NOT bloom_neutral_v1_payload_byte_count EQUAL 632)
    message(FATAL_ERROR
        "assets/ocio/neutral-v1/config.ocio is expected to be exactly 632 bytes (found "
        "${bloom_neutral_v1_payload_byte_count}); its bytes are frozen per "
        "assets/ocio/neutral-v1/provenance.md. If this asset genuinely changed, that is a "
        "contract violation -- a content change requires a new built-in URI, never an edit of "
        "this payload.")
endif()
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," BLOOM_NEUTRAL_V1_PAYLOAD_BYTES
    "${bloom_neutral_v1_payload_hex}")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/ocio_builtin_payload.inc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/ocio_builtin_payload.inc"
    @ONLY)

add_library(bloom_color_ocio STATIC)

target_sources(
    bloom_color_ocio
    PRIVATE
        ocio_builtin_registry.cpp
        ocio_cpu_display_processor.cpp
        ocio_cpu_display_frame.cpp
        ocio_internal.hpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bloom/color/ocio_builtin_registry.hpp
            include/bloom/color/ocio_cpu_display_processor.hpp
            include/bloom/color/ocio_cpu_display_frame.hpp
)

target_compile_features(bloom_color_ocio PUBLIC cxx_std_20)
target_link_libraries(bloom_color_ocio PUBLIC bloom_color bloom_render PRIVATE OpenColorIO::OpenColorIO)
# The generated embed header (ocio_builtin_registry.cpp's #include "ocio_builtin_payload.inc").
# OpenColorIO::OpenColorIO's own INTERFACE_INCLUDE_DIRECTORIES are consumed as SYSTEM headers
# automatically -- CMake treats an IMPORTED target's interface include directories as SYSTEM for
# consuming targets by default, matching the intake contract's warning policy ("all third-party
# targets are private implementation dependencies and treated as system headers for Bloom warning
# policy without suppressing Bloom adapter warnings") without any extra property here.
target_include_directories(bloom_color_ocio PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(
    bloom_color_ocio
    PRIVATE "BLOOM_COLOR_OCIO_TARGET_TRIPLE=\"${CMAKE_SYSTEM_PROCESSOR}-${CMAKE_SYSTEM_NAME}\""
)
bloom_enable_warnings(bloom_color_ocio)

if(BUILD_TESTING)
    include(BloomTesting)
    add_executable(
        bloom_color_ocio_content_revision_test
        tests/ocio_content_revision_tests.cpp
    )
    target_link_libraries(bloom_color_ocio_content_revision_test PRIVATE bloom_color)
    bloom_enable_warnings(bloom_color_ocio_content_revision_test)
    bloom_add_test(
        NAME bloom.color.ocio-content-revision
        COMMAND bloom_color_ocio_content_revision_test
        LABELS unit color identity
    )

    add_executable(
        bloom_color_ocio_loose_content_revision_test
        tests/ocio_loose_content_revision_tests.cpp
    )
    target_link_libraries(bloom_color_ocio_loose_content_revision_test PRIVATE bloom_color)
    bloom_enable_warnings(bloom_color_ocio_loose_content_revision_test)
    bloom_add_test(
        NAME bloom.color.ocio-loose-content-revision
        COMMAND bloom_color_ocio_loose_content_revision_test
        TIMEOUT 120
        LABELS unit color identity
    )

    add_executable(
        bloom_color_display_processor_identity_test
        tests/display_processor_identity_tests.cpp
    )
    target_link_libraries(bloom_color_display_processor_identity_test PRIVATE bloom_color)
    bloom_enable_warnings(bloom_color_display_processor_identity_test)
    bloom_add_test(
        NAME bloom.color.display-processor-identity
        COMMAND bloom_color_display_processor_identity_test
        LABELS unit color identity codec
    )

    add_executable(
        bloom_color_ocio_builtin_registry_test
        tests/ocio_builtin_registry_tests.cpp
    )
    target_link_libraries(bloom_color_ocio_builtin_registry_test PRIVATE bloom_color_ocio)
    bloom_enable_warnings(bloom_color_ocio_builtin_registry_test)
    bloom_add_test(
        NAME bloom.color.ocio-builtin-registry
        COMMAND bloom_color_ocio_builtin_registry_test
        LABELS unit color ocio
    )

    add_executable(
        bloom_color_ocio_cpu_display_processor_test
        tests/ocio_cpu_display_processor_tests.cpp
    )
    target_link_libraries(bloom_color_ocio_cpu_display_processor_test PRIVATE bloom_color_ocio)
    bloom_enable_warnings(bloom_color_ocio_cpu_display_processor_test)
    bloom_add_test(
        NAME bloom.color.ocio-cpu-display-processor
        COMMAND bloom_color_ocio_cpu_display_processor_test
        LABELS unit color ocio identity
    )

    add_executable(
        bloom_color_ocio_cpu_display_frame_test
        tests/ocio_cpu_display_frame_tests.cpp
    )
    target_link_libraries(bloom_color_ocio_cpu_display_frame_test PRIVATE bloom_color_ocio)
    bloom_enable_warnings(bloom_color_ocio_cpu_display_frame_test)
    bloom_add_test(
        NAME bloom.color.ocio-cpu-display-frame
        COMMAND bloom_color_ocio_cpu_display_frame_test
        LABELS unit color ocio render
    )
endif()
