# CPython is an optional host prerequisite. Explicit OFF avoids discovery; an explicit ON
# requires headers and embedding support instead of silently dropping the feature.
set(Python3_FIND_STRATEGY LOCATION)
set(Python3_FIND_UNVERSIONED_NAMES FIRST)
if(NOT DEFINED BLOOM_BUILD_PYTHON)
    find_package(Python3 3.10 QUIET COMPONENTS Interpreter Development.Module Development.Embed)
    option(BLOOM_BUILD_PYTHON "Build the Python bridge and embedded interpreter" ${Python3_FOUND})
endif()
if(BLOOM_BUILD_PYTHON)
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter Development.Module Development.Embed)
    if(Python3_SOABI MATCHES "[0-9]t")
        message(FATAL_ERROR "Bloom Python v1 requires a GIL-enabled CPython interpreter")
    endif()
endif()

add_library(bloom_scripting STATIC)

target_sources(
    bloom_scripting
    PRIVATE
        json_script.cpp
        operation_registry.cpp
        render.cpp
        session.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bloom/scripting/events.hpp
            include/bloom/scripting/facade.hpp
            include/bloom/scripting/json_script.hpp
            include/bloom/scripting/operation_registry.hpp
            include/bloom/scripting/query.hpp
            include/bloom/scripting/render.hpp
            include/bloom/scripting/session.hpp
            include/bloom/scripting/tasks.hpp
            include/bloom/scripting/value.hpp
)

target_compile_features(bloom_scripting PUBLIC cxx_std_20)
target_link_libraries(
    bloom_scripting
    PUBLIC bloom_commands bloom_core bloom_document bloom_host bloom_runtime
)
bloom_enable_warnings(bloom_scripting)

if(BUILD_TESTING)
    include(BloomTesting)
    add_executable(bloom_scripting_live_session_test tests/live_session_tests.cpp)
    target_link_libraries(bloom_scripting_live_session_test PRIVATE bloom_scripting)
    bloom_enable_warnings(bloom_scripting_live_session_test)
    bloom_add_test(NAME bloom.scripting.live-session COMMAND bloom_scripting_live_session_test
        LABELS unit scripting host commands document)
    add_executable(bloom_scripting_operation_schema_test tests/operation_schema_tests.cpp)
    target_link_libraries(bloom_scripting_operation_schema_test PRIVATE bloom_scripting)
    bloom_enable_warnings(bloom_scripting_operation_schema_test)
    bloom_add_test(
        NAME bloom.scripting.operation-schemas
        COMMAND bloom_scripting_operation_schema_test
        LABELS unit scripting commands document
    )
    add_executable(bloom_scripting_facade_test tests/facade_tests.cpp)
    target_link_libraries(bloom_scripting_facade_test PRIVATE bloom_scripting)
    bloom_enable_warnings(bloom_scripting_facade_test)
    bloom_add_test(
        NAME bloom.scripting.facade
        COMMAND bloom_scripting_facade_test
        LABELS unit scripting host commands document
    )
    # Render::run's injected-provider contract plus the native headless render integration: an
    # owner-provided GpuExportProvider is reused across renders and never retired by the call, while
    # a locally created one is retired with proof. OpenEXR lets the test reopen the published EXR
    # payloads for strict CPU/process and encoded-output parity.
    bloom_find_dependency(OpenEXR)
    add_executable(bloom_scripting_render_injected_provider_test
                   tests/render_injected_provider_test.cpp)
    target_link_libraries(bloom_scripting_render_injected_provider_test
                          PRIVATE bloom_scripting OpenEXR::OpenEXR)
    bloom_enable_warnings(bloom_scripting_render_injected_provider_test)
    # CTest runs the actual GPU case when the qualified prefix provides a loader (the test skips
    # honestly when no real device is present, so the CPU stub stays tested on GPU-less hosts).
    set(_bloom_render_injected_test_environment)
    if(UNIX
       AND NOT APPLE
       AND BLOOM_RENDER_ENABLE_VULKAN
       AND DEFINED BLOOM_DEPENDENCY_MODE
       AND BLOOM_DEPENDENCY_MODE STREQUAL "qualified")
        find_package(VulkanLoader QUIET CONFIG PATHS "${BLOOM_DEPENDENCY_PREFIX}" NO_DEFAULT_PATH)
        if(TARGET Vulkan::Loader)
            list(APPEND _bloom_render_injected_test_environment
                 "BLOOM_TEST_VULKAN_LOADER=$<TARGET_FILE:Vulkan::Loader>")
        endif()
    endif()
    bloom_add_test(
        NAME bloom.scripting.render-injected-provider
        COMMAND bloom_scripting_render_injected_provider_test
        LABELS unit scripting host rendering
        ENVIRONMENT ${_bloom_render_injected_test_environment}
        TIMEOUT 180
    )
endif()

if(BLOOM_BUILD_PYTHON)
    add_subdirectory(python)
endif()
