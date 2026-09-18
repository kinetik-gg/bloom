# No Qt or codec dependency belongs in the host-side contract library.
add_library(bloom_media_provider STATIC contract.cpp records.cpp encode.cpp)
target_include_directories(bloom_media_provider PUBLIC include)
target_compile_features(bloom_media_provider PUBLIC cxx_std_20)
target_link_libraries(bloom_media_provider PUBLIC bloom_core)
bloom_enable_warnings(bloom_media_provider)
if(BUILD_TESTING)
    include(BloomTesting)
    add_executable(bloom_media_contract_test tests/contract_tests.cpp)
    target_link_libraries(bloom_media_contract_test PRIVATE bloom_media_provider)
    bloom_enable_warnings(bloom_media_contract_test)
    bloom_add_test(NAME bloom.media.contract COMMAND bloom_media_contract_test LABELS unit media)
endif()

target_sources(bloom_media_provider PRIVATE registry.cpp)
if(BUILD_TESTING)
    add_executable(bloom_media_registry_test tests/registry_tests.cpp)
    target_link_libraries(bloom_media_registry_test PRIVATE bloom_media_provider)
    bloom_enable_warnings(bloom_media_registry_test)
    bloom_add_test(NAME bloom.media.registry COMMAND bloom_media_registry_test LABELS unit media)
endif()

target_sources(bloom_media_provider PRIVATE protocol.cpp)
if(BUILD_TESTING)
    add_executable(bloom_media_protocol_test tests/protocol_tests.cpp)
    target_link_libraries(bloom_media_protocol_test PRIVATE bloom_media_provider)
    bloom_enable_warnings(bloom_media_protocol_test)
    bloom_add_test(NAME bloom.media.protocol COMMAND bloom_media_protocol_test LABELS unit media security)
endif()

# Host facade only. bloom_runtime (the scheduler) depends on core, not media; the evaluator's
# separate media-image dependency cannot cycle back through this target.
add_library(bloom_media_openh264_runtime STATIC openh264_runtime.cpp)
target_include_directories(bloom_media_openh264_runtime PUBLIC include)
target_link_libraries(bloom_media_openh264_runtime PUBLIC bloom_platform_process bloom_platform)
bloom_enable_warnings(bloom_media_openh264_runtime)
if(BUILD_TESTING)
    add_executable(bloom_media_openh264_runtime_test tests/openh264_runtime_tests.cpp)
    target_compile_definitions(
        bloom_media_openh264_runtime_test
        PRIVATE BLOOM_OPENH264_RUNTIME_TEST_ROOT="${CMAKE_BINARY_DIR}/openh264-runtime-tests"
                BLOOM_OPENH264_LICENSE_FIXTURE="${BLOOM_DEPENDENCY_PREFIX}/../superbuild/downloads/BINARY_LICENSE.txt")
    target_link_libraries(bloom_media_openh264_runtime_test PRIVATE bloom_media_openh264_runtime)
    bloom_enable_warnings(bloom_media_openh264_runtime_test)
    bloom_add_test(NAME bloom.media.openh264-runtime COMMAND bloom_media_openh264_runtime_test
                   LABELS unit media security)
endif()

add_library(bloom_media_worker_host STATIC worker_pool.cpp encode_session.cpp ffmpeg_launch.cpp)
target_link_libraries(bloom_media_worker_host PUBLIC bloom_media_provider bloom_platform_process
                      bloom_media_openh264_runtime bloom_runtime)
bloom_enable_warnings(bloom_media_worker_host)

if(BUILD_TESTING)
    # Compile the fallback on Linux too: platform support is an explicit typed result.
    add_executable(bloom_media_unsupported_process_test tests/unsupported_process_tests.cpp
                   "${PROJECT_SOURCE_DIR}/src/platform/process_supervisor_unsupported.cpp")
    target_include_directories(bloom_media_unsupported_process_test PRIVATE
                               "${PROJECT_SOURCE_DIR}/src/platform/include")
    target_compile_features(bloom_media_unsupported_process_test PRIVATE cxx_std_20)
    bloom_enable_warnings(bloom_media_unsupported_process_test)
    bloom_add_test(NAME bloom.media.unsupported-process COMMAND bloom_media_unsupported_process_test
                   LABELS unit media platform)
endif()
