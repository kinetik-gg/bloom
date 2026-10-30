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
        cache_directory.cpp
        destination_facts.cpp
        user_data_directory.cpp
        font_catalog.cpp
        staged_artifact.cpp
        staged_artifact_platform.hpp
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Fontconfig REQUIRED)
    target_sources(bloom_platform PRIVATE font_catalog_linux.cpp staged_artifact_linux.cpp)
    target_link_libraries(bloom_platform PUBLIC Fontconfig::Fontconfig)
elseif(APPLE)
    # CoreText supplies the system font catalogue; the staged-artifact provider is the POSIX/macOS
    # implementation (openat/renameatx_np/getentropy).
    target_sources(bloom_platform PRIVATE font_catalog_macos.cpp staged_artifact_macos.cpp)
    target_link_libraries(bloom_platform PUBLIC "-framework CoreText" "-framework CoreFoundation")
else()
    target_sources(bloom_platform PRIVATE font_catalog_unavailable.cpp staged_artifact_unsupported.cpp)
endif()

# ADR 0034: loopback sockets and owner-only secrets, split by backend like the staged-artifact
# provider (POSIX on Linux and macOS, Winsock 2 and DACLs on Windows).
if(WIN32)
    target_sources(bloom_platform PRIVATE loopback_listener_windows.cpp owner_only_secret_windows.cpp)
    target_link_libraries(bloom_platform PRIVATE ws2_32 advapi32 bcrypt)
else()
    target_sources(bloom_platform PRIVATE loopback_listener_posix.cpp owner_only_secret_posix.cpp)
endif()

target_sources(
    bloom_platform
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bloom/platform/cache_directory.hpp
            include/bloom/platform/destination_facts.hpp
            include/bloom/platform/user_data_directory.hpp
            include/bloom/platform/font_catalog.hpp
            include/bloom/platform/staged_artifact.hpp
            include/bloom/platform/loopback_listener.hpp
            include/bloom/platform/owner_only_secret.hpp
)

target_compile_features(bloom_platform PUBLIC cxx_std_20)
target_link_libraries(bloom_platform PUBLIC bloom_core)
bloom_enable_warnings(bloom_platform)

if(BUILD_TESTING AND (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR APPLE))
    include(BloomTesting)
    add_executable(bloom_platform_font_catalog_test tests/font_catalog_tests.cpp)
    target_compile_definitions(
        bloom_platform_font_catalog_test
        PRIVATE BLOOM_FONT_FIXTURE_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/../ui/kit/third_party/inter"
    )
    target_link_libraries(bloom_platform_font_catalog_test PRIVATE bloom_platform)
    bloom_enable_warnings(bloom_platform_font_catalog_test)
    bloom_add_test(
        NAME bloom.platform.font-catalog
        COMMAND bloom_platform_font_catalog_test
        LABELS unit platform text
    )
    add_executable(bloom_platform_staged_artifact_test tests/staged_artifact_tests.cpp)
    target_link_libraries(bloom_platform_staged_artifact_test PRIVATE bloom_platform)
    bloom_enable_warnings(bloom_platform_staged_artifact_test)
    bloom_add_test(
        NAME bloom.platform.staged_artifact
        COMMAND bloom_platform_staged_artifact_test
        LABELS unit platform persistence output
    )
    add_executable(bloom_platform_cache_directory_test tests/cache_directory_tests.cpp)
    target_link_libraries(bloom_platform_cache_directory_test PRIVATE bloom_platform)
    bloom_enable_warnings(bloom_platform_cache_directory_test)
    bloom_add_test(
        NAME bloom.platform.cache_directory
        COMMAND bloom_platform_cache_directory_test
        LABELS unit platform cache
    )
endif()

if(BUILD_TESTING)
    include(BloomTesting)
    # ADR 0034: on all three platforms.
    add_executable(bloom_platform_loopback_listener_test tests/loopback_listener_tests.cpp)
    target_link_libraries(bloom_platform_loopback_listener_test PRIVATE bloom_platform)
    if(WIN32)
        target_link_libraries(bloom_platform_loopback_listener_test PRIVATE ws2_32)
    endif()
    bloom_enable_warnings(bloom_platform_loopback_listener_test)
    bloom_add_test(
        NAME bloom.platform.loopback-listener
        COMMAND bloom_platform_loopback_listener_test
        LABELS unit platform mcp
        TIMEOUT 60
    )
    add_executable(bloom_platform_owner_only_secret_test tests/owner_only_secret_tests.cpp)
    target_link_libraries(bloom_platform_owner_only_secret_test PRIVATE bloom_platform)
    bloom_enable_warnings(bloom_platform_owner_only_secret_test)
    bloom_add_test(
        NAME bloom.platform.owner-only-secret
        COMMAND bloom_platform_owner_only_secret_test
        LABELS unit platform mcp
    )
endif()

# Process supervision has no font catalogue or filesystem publication dependency. Workers link
# this narrow target so codec trust domains do not inherit unused platform-library closures.
add_library(bloom_platform_process STATIC file_snapshot.cpp source_identity.cpp)
target_link_libraries(bloom_platform_process PUBLIC bloom_core)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Threads REQUIRED)
    target_sources(bloom_platform_process PRIVATE process_supervisor_linux.cpp)
    target_link_libraries(bloom_platform_process PRIVATE Threads::Threads)
elseif(APPLE)
    target_sources(bloom_platform_process PRIVATE process_supervisor_macos.cpp)
else()
    target_sources(bloom_platform_process PRIVATE process_supervisor_unsupported.cpp)
endif()
target_sources(bloom_platform_process PUBLIC FILE_SET HEADERS BASE_DIRS include
               FILES include/bloom/platform/process_supervisor.hpp include/bloom/platform/file_snapshot.hpp
                     include/bloom/platform/source_identity.hpp)
target_compile_features(bloom_platform_process PUBLIC cxx_std_20)
bloom_enable_warnings(bloom_platform_process)

if(BUILD_TESTING AND (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR APPLE))
    add_executable(bloom_platform_process_descriptor_test tests/process_descriptor_tests.cpp)
    target_link_libraries(bloom_platform_process_descriptor_test PRIVATE bloom_platform_process)
    bloom_enable_warnings(bloom_platform_process_descriptor_test)
    bloom_add_test(NAME bloom.platform.process_descriptors
                   COMMAND bloom_platform_process_descriptor_test LABELS unit platform process)
    add_executable(bloom_platform_source_identity_test tests/source_identity_tests.cpp)
    target_link_libraries(bloom_platform_source_identity_test PRIVATE bloom_platform_process)
    bloom_enable_warnings(bloom_platform_source_identity_test)
    bloom_add_test(NAME bloom.platform.source_identity
                   COMMAND bloom_platform_source_identity_test LABELS unit platform media)
endif()
