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
    PRIVATE ocio_content_revision.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES include/bloom/color/ocio_content_revision.hpp
)

target_compile_features(bloom_color PUBLIC cxx_std_20)
target_link_libraries(bloom_color PUBLIC bloom_core)
bloom_enable_warnings(bloom_color)

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
endif()
