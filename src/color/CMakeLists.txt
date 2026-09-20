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

# bloom_color_gpu_shader: the runtime GLSL -> validated SPIR-V compile adapter. It is Qt-, Vulkan-,
# and OCIO-free, owns the isolated child-process compile through the narrow process-supervision
# target, and returns the render-owned portable artifact header without linking the Vulkan backend.
add_library(bloom_color_gpu_shader STATIC)

target_sources(
    bloom_color_gpu_shader
    PRIVATE
        gpu_shader_compiler.cpp
        gpu_shader_compiler_process.cpp
        gpu_shader_compiler_process.hpp
        gpu_shader_tool_resolver.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bloom/color/gpu_shader_compiler.hpp
            include/bloom/color/gpu_shader_tool_resolver.hpp
)

target_compile_features(bloom_color_gpu_shader PUBLIC cxx_std_20)
target_include_directories(bloom_color_gpu_shader PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../render/include")
target_link_libraries(bloom_color_gpu_shader PUBLIC bloom_core bloom_platform_process)
bloom_enable_warnings(bloom_color_gpu_shader)

if(BUILD_TESTING)
    include(BloomTesting)
    add_executable(
        bloom_color_gpu_shader_test_helper
        tests/gpu_shader_compiler_test_helper.cpp
    )
    target_compile_features(bloom_color_gpu_shader_test_helper PRIVATE cxx_std_20)
    bloom_enable_warnings(bloom_color_gpu_shader_test_helper)

    add_executable(
        bloom_color_gpu_shader_test
        tests/gpu_shader_compiler_tests.cpp
    )
    target_compile_definitions(
        bloom_color_gpu_shader_test
        PRIVATE
            "BLOOM_GPU_SHADER_TEST_HELPER=\"$<TARGET_FILE:bloom_color_gpu_shader_test_helper>\""
    )
    target_link_libraries(bloom_color_gpu_shader_test PRIVATE bloom_color_gpu_shader)
    add_dependencies(bloom_color_gpu_shader_test bloom_color_gpu_shader_test_helper)
    bloom_enable_warnings(bloom_color_gpu_shader_test)

    if(BLOOM_DEPENDENCY_PREFIX)
        set(bloom_gpu_shader_test_environment
            "BLOOM_GLSLANG_VALIDATOR=${BLOOM_DEPENDENCY_PREFIX}/bin/glslangValidator"
            "BLOOM_SPIRV_VAL=${BLOOM_DEPENDENCY_PREFIX}/bin/spirv-val"
        )
    else()
        set(bloom_gpu_shader_test_environment "")
    endif()
    bloom_add_test(
        NAME bloom.color.gpu-shader-compiler
        COMMAND bloom_color_gpu_shader_test
        LABELS unit color gpu
        ENVIRONMENT ${bloom_gpu_shader_test_environment}
    )
    set_tests_properties(bloom.color.gpu-shader-compiler PROPERTIES SKIP_RETURN_CODE 77)

    add_executable(
        bloom_color_gpu_shader_tool_resolver_test
        tests/gpu_shader_tool_resolver_tests.cpp
    )
    target_link_libraries(bloom_color_gpu_shader_tool_resolver_test PRIVATE bloom_color_gpu_shader)
    bloom_enable_warnings(bloom_color_gpu_shader_tool_resolver_test)
    bloom_add_test(
        NAME bloom.color.gpu-shader-tool-resolver
        COMMAND bloom_color_gpu_shader_tool_resolver_test
        LABELS unit color gpu
        ENVIRONMENT ${bloom_gpu_shader_test_environment}
    )
    set_tests_properties(bloom.color.gpu-shader-tool-resolver PROPERTIES SKIP_RETURN_CODE 77)
endif()

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
        ocio_cpu_color_space_processor.cpp
        ocio_cpu_file_transform_processor.cpp
        lut_file.cpp
        lut_worker_protocol.hpp
        lut_worker_ipc.hpp
        lut_preflight.hpp
        lut_resource_budget.hpp
        ocio_cpu_display_processor.cpp
        ocio_cpu_input_processor.cpp
        ocio_cpu_display_frame.cpp
        ocio_gpu_program.cpp
        ocio_gpu_program_extract.cpp
        ocio_gpu_program_extract.hpp
        ocio_gpu_program_serialize.cpp
        ocio_gpu_program_serialize.hpp
        ocio_gpu_program_sampling.cpp
        ocio_gpu_program_transport.hpp
        ocio_gpu_program_worker.cpp
        ocio_gpu_program_worker.hpp
        ocio_internal.hpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bloom/color/ocio_builtin_registry.hpp
            include/bloom/color/ocio_cpu_color_space_processor.hpp
            include/bloom/color/ocio_cpu_file_transform_processor.hpp
            include/bloom/color/ocio_cpu_display_processor.hpp
            include/bloom/color/ocio_cpu_input_processor.hpp
            include/bloom/color/ocio_cpu_display_frame.hpp
            include/bloom/color/ocio_gpu_program.hpp
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
        bloom_color_ocio_cpu_color_space_processor_test
        tests/ocio_cpu_color_space_processor_tests.cpp
    )
    target_link_libraries(bloom_color_ocio_cpu_color_space_processor_test PRIVATE bloom_color_ocio)
    bloom_enable_warnings(bloom_color_ocio_cpu_color_space_processor_test)
    bloom_add_test(
        NAME bloom.color.ocio-cpu-color-space-processor
        COMMAND bloom_color_ocio_cpu_color_space_processor_test
        LABELS unit color ocio
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

# Provider-neutral video colour conversion; no codec library enters this target.
add_library(bloom_color_video STATIC video_colour.cpp)
target_include_directories(bloom_color_video PUBLIC include)
target_link_libraries(bloom_color_video PUBLIC bloom_core)
bloom_enable_warnings(bloom_color_video)
bloom_enable_strict_floating_point(bloom_color_video)

# External LUT parsing and execution never enter the host process. The worker shares the trusted
# OCIO shader-reflection codec with the host so neither duplicates the extraction logic; it only
# ever links OCIO and the portable render value header, never a native device.
add_executable(bloom-color-worker lut_worker.cpp lut_file.cpp ocio_gpu_program_extract.cpp
    ocio_gpu_program_serialize.cpp)
target_link_libraries(bloom-color-worker PRIVATE bloom_core bloom_platform_process OpenColorIO::OpenColorIO)
target_include_directories(bloom-color-worker PRIVATE include "${CMAKE_CURRENT_SOURCE_DIR}/../render/include")
bloom_enable_warnings(bloom-color-worker)
bloom_enable_strict_floating_point(bloom-color-worker)
target_link_libraries(bloom_color_ocio PRIVATE bloom_platform_process)
target_compile_definitions(bloom_color_ocio PRIVATE BLOOM_COLOR_WORKER="$<TARGET_FILE:bloom-color-worker>")
add_dependencies(bloom_color_ocio bloom-color-worker)
install(TARGETS bloom-color-worker RUNTIME DESTINATION libexec/bloom)
if(BUILD_TESTING)
    add_executable(bloom_color_ocio_file_transform_test tests/ocio_cpu_file_transform_processor_tests.cpp)
    target_link_libraries(bloom_color_ocio_file_transform_test PRIVATE bloom_color_ocio OpenColorIO::OpenColorIO)
    bloom_enable_warnings(bloom_color_ocio_file_transform_test)
    bloom_add_test(NAME bloom.color.ocio-file-transform COMMAND bloom_color_ocio_file_transform_test LABELS unit color ocio security)

    # Reusable OCIO GPU program extraction (S0). Links the real OCIO CPU oracle and, when the
    # pinned prefix tools are present, compiles the wrapper fixture offline with glslangValidator
    # and spirv-val. No native device is touched; that is the next lane.
    find_program(bloom_ocio_gpu_glslang_validator
        NAMES glslangValidator
        HINTS "${BLOOM_DEPENDENCY_PREFIX}/bin"
        NO_DEFAULT_PATH)
    find_program(bloom_ocio_gpu_spirv_val
        NAMES spirv-val
        HINTS "${BLOOM_DEPENDENCY_PREFIX}/bin"
        NO_DEFAULT_PATH)
    add_executable(bloom_color_ocio_gpu_program_test
        tests/ocio_gpu_program_tests.cpp
        tests/ocio_gpu_program_transport_tests.cpp)
    target_link_libraries(bloom_color_ocio_gpu_program_test PRIVATE bloom_color_ocio OpenColorIO::OpenColorIO)
    target_include_directories(bloom_color_ocio_gpu_program_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(bloom_color_ocio_gpu_program_test PRIVATE
        "BLOOM_OCIO_GPU_WRAPPER_FIXTURE=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/ocio_gpu_wrapper_fixture.comp\"")
    if(bloom_ocio_gpu_glslang_validator AND bloom_ocio_gpu_spirv_val)
        get_filename_component(bloom_ocio_gpu_tools_dir "${bloom_ocio_gpu_glslang_validator}" DIRECTORY)
        target_compile_definitions(bloom_color_ocio_gpu_program_test PRIVATE
            "BLOOM_GPUSHADER_TOOLS_DIR=\"${bloom_ocio_gpu_tools_dir}\"")
    endif()
    bloom_enable_warnings(bloom_color_ocio_gpu_program_test)
    bloom_add_test(
        NAME bloom.color.ocio-gpu-program
        COMMAND bloom_color_ocio_gpu_program_test
        TIMEOUT 120
        LABELS unit color ocio gpu)

    # CPU-only proof for the versioned CPU fast-power parity adapter: the real neutral GammaOp CST
    # must receive the ssePower `pow` redirection with the generated body left verbatim, and a
    # non-GammaOp program must not. The device parity proof is the runtime precision native test.
    add_executable(bloom_color_ocio_gpu_precision_test
        tests/ocio_gpu_precision_tests.cpp)
    target_link_libraries(bloom_color_ocio_gpu_precision_test PRIVATE bloom_color_ocio OpenColorIO::OpenColorIO)
    bloom_enable_warnings(bloom_color_ocio_gpu_precision_test)
    bloom_add_test(
        NAME bloom.color.ocio-gpu-precision
        COMMAND bloom_color_ocio_gpu_precision_test
        TIMEOUT 120
        LABELS unit color ocio gpu)

    # Native GpuOcioProgram proof. Compiles the complete wrapper+OCIO GLSL with the pinned tools
    # off the device owner thread, then runs the real Vulkan pipeline and compares against the real
    # CPU colour processors. Skips explicitly (77) when no compatible device is present.
    add_executable(bloom_color_ocio_gpu_program_native_test
        tests/ocio_gpu_program_native_tests.cpp
        tests/ocio_gpu_program_native_helpers.cpp
        tests/ocio_gpu_program_native_file_transform_tests.cpp)
    target_link_libraries(bloom_color_ocio_gpu_program_native_test PRIVATE bloom_color_ocio OpenColorIO::OpenColorIO)
    if(bloom_ocio_gpu_glslang_validator AND bloom_ocio_gpu_spirv_val)
        target_compile_definitions(bloom_color_ocio_gpu_program_native_test PRIVATE
            "BLOOM_GPUSHADER_TOOLS_DIR=\"${bloom_ocio_gpu_tools_dir}\"")
    endif()
    bloom_enable_warnings(bloom_color_ocio_gpu_program_native_test)
    bloom_add_test(
        NAME bloom.color.ocio-gpu-program-native
        COMMAND bloom_color_ocio_gpu_program_native_test
        TIMEOUT 300
        LABELS unit color ocio gpu)
    # The test exits 77 when no compatible device is present; report that as a skip, not a pass.
    set_tests_properties(bloom.color.ocio-gpu-program-native PROPERTIES SKIP_RETURN_CODE 77)

    # The resource/lifecycle native tests exercise the real Vulkan backend and the test-only fault
    # setters; register them only when bloom_render compiled the real backend. A CPU-stub build
    # still builds and runs the extraction and no-resource native tests (which skip explicitly).
    if(BLOOM_RENDER_VULKAN_AVAILABLE)
        add_executable(bloom_color_ocio_gpu_program_resources_native_test
            tests/ocio_gpu_program_resources_native_tests.cpp)
        target_link_libraries(bloom_color_ocio_gpu_program_resources_native_test PRIVATE bloom_color_ocio OpenColorIO::OpenColorIO)
        target_include_directories(bloom_color_ocio_gpu_program_resources_native_test
            PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../render")
        target_link_libraries(bloom_color_ocio_gpu_program_resources_native_test
            PRIVATE bloom_render_ocio_test_faults)
        if(bloom_ocio_gpu_glslang_validator AND bloom_ocio_gpu_spirv_val)
            target_compile_definitions(bloom_color_ocio_gpu_program_resources_native_test PRIVATE
                "BLOOM_GPUSHADER_TOOLS_DIR=\"${bloom_ocio_gpu_tools_dir}\"")
    endif()
    bloom_enable_warnings(bloom_color_ocio_gpu_program_resources_native_test)
    bloom_add_test(
        NAME bloom.color.ocio-gpu-program-resources-native
        COMMAND bloom_color_ocio_gpu_program_resources_native_test
        TIMEOUT 300
        LABELS unit color ocio gpu)
    set_tests_properties(bloom.color.ocio-gpu-program-resources-native PROPERTIES
        SKIP_RETURN_CODE 77)

    add_executable(bloom_color_ocio_gpu_program_lifecycle_native_test
        tests/ocio_gpu_program_lifecycle_native_tests.cpp)
    target_link_libraries(bloom_color_ocio_gpu_program_lifecycle_native_test PRIVATE bloom_color_ocio OpenColorIO::OpenColorIO)
    target_include_directories(bloom_color_ocio_gpu_program_lifecycle_native_test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../render")
    target_link_libraries(bloom_color_ocio_gpu_program_lifecycle_native_test
        PRIVATE bloom_render_ocio_test_faults)
    if(bloom_ocio_gpu_glslang_validator AND bloom_ocio_gpu_spirv_val)
        target_compile_definitions(bloom_color_ocio_gpu_program_lifecycle_native_test PRIVATE
            "BLOOM_GPUSHADER_TOOLS_DIR=\"${bloom_ocio_gpu_tools_dir}\"")
    endif()
    bloom_enable_warnings(bloom_color_ocio_gpu_program_lifecycle_native_test)
    bloom_add_test(
        NAME bloom.color.ocio-gpu-program-lifecycle-native
        COMMAND bloom_color_ocio_gpu_program_lifecycle_native_test
        TIMEOUT 300
        LABELS unit color ocio gpu)
    set_tests_properties(bloom.color.ocio-gpu-program-lifecycle-native PROPERTIES
        SKIP_RETURN_CODE 77)
    endif()
endif()
