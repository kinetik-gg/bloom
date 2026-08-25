if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.25)
    project(BloomPlatform LANGUAGES CXX)
    get_filename_component(
        BLOOM_REPOSITORY_SOURCE_DIR
        "${CMAKE_CURRENT_LIST_DIR}/../.."
        ABSOLUTE
    )
    list(APPEND CMAKE_MODULE_PATH "${BLOOM_REPOSITORY_SOURCE_DIR}/cmake")
    include(CTest)
    include(BloomQuality)
    include(BloomWarnings)

    # The shared quality module deliberately roots format discovery and clang-tidy header
    # filtering at PROJECT_SOURCE_DIR. A standalone platform build is still part of the Bloom
    # repository, so configure those gates against the real repository root and then restore the
    # standalone project's source directory.
    set(BLOOM_PLATFORM_STANDALONE_SOURCE_DIR "${PROJECT_SOURCE_DIR}")
    set(PROJECT_SOURCE_DIR "${BLOOM_REPOSITORY_SOURCE_DIR}")
    bloom_configure_quality_tools()
    set(PROJECT_SOURCE_DIR "${BLOOM_PLATFORM_STANDALONE_SOURCE_DIR}")

    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../core" "${CMAKE_CURRENT_BINARY_DIR}/core")
endif()

add_library(bloom_platform STATIC)

target_sources(
    bloom_platform
    PRIVATE
        staged_artifact.cpp
        staged_artifact_platform.hpp
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(bloom_platform PRIVATE staged_artifact_linux.cpp)
else()
    target_sources(bloom_platform PRIVATE staged_artifact_unsupported.cpp)
endif()

target_sources(
    bloom_platform
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES include/bloom/platform/staged_artifact.hpp
)

target_compile_features(bloom_platform PUBLIC cxx_std_20)
target_link_libraries(bloom_platform PUBLIC bloom_core)
bloom_enable_warnings(bloom_platform)

if(BUILD_TESTING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    include(BloomTesting)
    add_executable(bloom_platform_staged_artifact_test tests/staged_artifact_tests.cpp)
    target_link_libraries(bloom_platform_staged_artifact_test PRIVATE bloom_platform)
    bloom_enable_warnings(bloom_platform_staged_artifact_test)
    bloom_add_test(
        NAME bloom.platform.staged_artifact
        COMMAND bloom_platform_staged_artifact_test
        LABELS unit platform persistence output
    )
endif()
