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
    # Render::run's injected-provider contract plus the native headless and sequence-range render
    # integration: an owner-provided GpuExportProvider is reused across renders and never retired by
    # the call, while a locally created one is retired with proof. ZLIB backs the test's own
    # from-scratch PNG reader, so the scripted routes independently decode the published PNG payloads
    # and prove the genuine GPU output-colour (two-payload) arm. The production packaged GPU shader
    # tools are staged beside the executable exactly as the desktop/CLI/MCP roots stage them, so the
    # PNG output-colour command is prepared through the real packaged resolver -- never PATH.
    include(BloomGpuShaderTools)
    bloom_find_dependency(ZLIB)
    # The helper roots the qualified zlib search at the prefix (and caches ZLIB_LIBRARY), but its
    # find_dependency() form does not guarantee the namespaced target exists in this directory
    # scope; mirror the host targets' own explicit guard so ZLIB::ZLIB is always defined here.
    if(NOT TARGET ZLIB::ZLIB)
        find_package(ZLIB REQUIRED)
    endif()
    add_executable(bloom_scripting_render_injected_provider_test
                   tests/render_injected_provider_test.cpp)
    target_link_libraries(bloom_scripting_render_injected_provider_test
                          PRIVATE bloom_scripting ZLIB::ZLIB)
    target_include_directories(bloom_scripting_render_injected_provider_test PRIVATE
        "${PROJECT_SOURCE_DIR}/tests/support"
        "${PROJECT_SOURCE_DIR}/src/runtime/include" "${PROJECT_SOURCE_DIR}/src/core/include"
        "${PROJECT_SOURCE_DIR}/src/document/include" "${PROJECT_SOURCE_DIR}/src/render/include")
    bloom_package_gpu_shader_tools(bloom_scripting_render_injected_provider_test)
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

    # Scripted export route-proof writers: the same production Render::run facade, opt-in
    # --route-proof-dir, publishing only after every parity assertion passed. The sequence/range
    # route exercises the real FrameRangeRunner image-sequence mode; the headless route the
    # single-frame mode. Both share the run nonce written by the gpu_route_proofs fixture.
    set(BLOOM_GPU_ROUTE_PROOF_DIR "${PROJECT_BINARY_DIR}/gpu-route-proofs")
    bloom_add_test(
        NAME bloom.scripting.export-route-proof.headless
        COMMAND bloom_scripting_render_injected_provider_test
                --route-proof-dir "${BLOOM_GPU_ROUTE_PROOF_DIR}" --route headless
        LABELS unit scripting host gpu contract
        ENVIRONMENT ${_bloom_render_injected_test_environment}
        TIMEOUT 300
    )
    set_tests_properties(bloom.scripting.export-route-proof.headless PROPERTIES
        SKIP_RETURN_CODE 77
        FIXTURES_REQUIRED gpu_route_proofs
        FIXTURES_SETUP gpu_route_proof_writers)

    bloom_add_test(
        NAME bloom.scripting.export-route-proof.sequence-range
        COMMAND bloom_scripting_render_injected_provider_test
                --route-proof-dir "${BLOOM_GPU_ROUTE_PROOF_DIR}" --route sequence
        LABELS unit scripting host gpu contract
        ENVIRONMENT ${_bloom_render_injected_test_environment}
        TIMEOUT 300
    )
    set_tests_properties(bloom.scripting.export-route-proof.sequence-range PROPERTIES
        SKIP_RETURN_CODE 77
        FIXTURES_REQUIRED gpu_route_proofs
        FIXTURES_SETUP gpu_route_proof_writers)
endif()

if(BLOOM_BUILD_PYTHON)
    add_subdirectory(python)
endif()
