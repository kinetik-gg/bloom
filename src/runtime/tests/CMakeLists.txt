add_executable(bloom_runtime_task_test task_scheduler_tests.cpp)
add_executable(bloom_runtime_regression_test task_scheduler_regression_tests.cpp)
add_executable(bloom_runtime_gpu_task_test task_gpu_executor_tests.cpp)
add_executable(bloom_runtime_registry_test node_definition_registry_tests.cpp)
add_executable(bloom_runtime_snapshot_compiler_test snapshot_compiler_tests.cpp)
add_executable(bloom_runtime_cpu_composition_evaluator_test cpu_composition_evaluator_tests.cpp)
add_executable(bloom_runtime_animation_sampling_test animation_sampling_tests.cpp)
add_executable(bloom_runtime_curve_compilation_test curve_compilation_tests.cpp)
add_executable(bloom_runtime_qualified_display_preparation_test qualified_display_preparation_tests.cpp)
add_executable(bloom_runtime_value_graph_test value_graph_tests.cpp)
add_executable(bloom_runtime_value_conversion_test value_conversion_tests.cpp)
add_executable(bloom_runtime_string_node_test string_node_tests.cpp)
add_executable(bloom_runtime_value_math_test value_math_tests.cpp)
add_executable(bloom_runtime_data_block_reader_test data_block_reader_tests.cpp)
add_executable(bloom_runtime_input_color_context_test input_color_context_tests.cpp)

target_link_libraries(bloom_runtime_task_test PRIVATE bloom_runtime)
target_link_libraries(bloom_runtime_regression_test PRIVATE bloom_runtime)
target_link_libraries(bloom_runtime_gpu_task_test PRIVATE bloom_runtime)
target_link_libraries(bloom_runtime_registry_test PRIVATE bloom_runtime_compiler)
target_link_libraries(
    bloom_runtime_snapshot_compiler_test
    PRIVATE bloom_runtime_compiler bloom_runtime_evaluator
)
target_link_libraries(
    bloom_runtime_cpu_composition_evaluator_test
    PRIVATE bloom_runtime_evaluator
)
target_link_libraries(bloom_runtime_animation_sampling_test PRIVATE bloom_runtime_evaluator)
target_link_libraries(bloom_runtime_curve_compilation_test PRIVATE bloom_runtime_compiler)
target_link_libraries(
    bloom_runtime_qualified_display_preparation_test
    PRIVATE bloom_runtime_evaluator
)
target_link_libraries(bloom_runtime_value_graph_test PRIVATE bloom_runtime_evaluator)
target_link_libraries(bloom_runtime_value_conversion_test PRIVATE bloom_runtime_evaluator)
target_link_libraries(bloom_runtime_string_node_test PRIVATE bloom_runtime_evaluator)
target_link_libraries(bloom_runtime_value_math_test PRIVATE bloom_runtime_evaluator)
target_link_libraries(bloom_runtime_data_block_reader_test PRIVATE bloom_runtime_evaluator)
target_link_libraries(bloom_runtime_input_color_context_test PRIVATE bloom_runtime_evaluator)
target_include_directories(bloom_runtime_regression_test PRIVATE ..)
target_include_directories(bloom_runtime_gpu_task_test PRIVATE ..)
target_include_directories(bloom_runtime_snapshot_compiler_test PRIVATE ..)
bloom_enable_warnings(bloom_runtime_task_test)
bloom_enable_warnings(bloom_runtime_regression_test)
bloom_enable_warnings(bloom_runtime_gpu_task_test)
bloom_enable_warnings(bloom_runtime_registry_test)
bloom_enable_warnings(bloom_runtime_snapshot_compiler_test)
bloom_enable_warnings(bloom_runtime_cpu_composition_evaluator_test)
bloom_enable_warnings(bloom_runtime_animation_sampling_test)
bloom_enable_warnings(bloom_runtime_curve_compilation_test)
bloom_enable_warnings(bloom_runtime_qualified_display_preparation_test)
bloom_enable_warnings(bloom_runtime_value_graph_test)
bloom_enable_warnings(bloom_runtime_value_conversion_test)
bloom_enable_warnings(bloom_runtime_string_node_test)
bloom_enable_warnings(bloom_runtime_value_math_test)
bloom_enable_warnings(bloom_runtime_data_block_reader_test)
bloom_enable_warnings(bloom_runtime_input_color_context_test)
bloom_enable_strict_floating_point(bloom_runtime_value_graph_test)
bloom_enable_strict_floating_point(bloom_runtime_value_conversion_test)
bloom_enable_strict_floating_point(bloom_runtime_value_math_test)
bloom_enable_strict_floating_point(bloom_runtime_cpu_composition_evaluator_test)
bloom_enable_strict_floating_point(bloom_runtime_animation_sampling_test)
bloom_enable_strict_floating_point(bloom_runtime_qualified_display_preparation_test)

include(BloomTesting)

bloom_add_test(
    NAME bloom.runtime.tasks
    COMMAND bloom_runtime_task_test
    LABELS runtime unit
    TIMEOUT 30
)

bloom_add_test(
    NAME bloom.runtime.regressions
    COMMAND bloom_runtime_regression_test
    LABELS runtime unit
    TIMEOUT 30
)

bloom_add_test(
    NAME bloom.runtime.gpu_tasks
    COMMAND bloom_runtime_gpu_task_test
    LABELS runtime unit gpu
    TIMEOUT 30
)

bloom_add_test(
    NAME bloom.runtime.node_definition_registry
    COMMAND bloom_runtime_registry_test
    LABELS runtime unit
    TIMEOUT 30
)

bloom_add_test(
    NAME bloom.runtime.snapshot_compiler
    COMMAND bloom_runtime_snapshot_compiler_test
    LABELS runtime unit
    TIMEOUT 30
)

bloom_add_test(
    NAME bloom.runtime.value_graph
    COMMAND bloom_runtime_value_graph_test
    LABELS runtime unit primitives
    TIMEOUT 30
)

bloom_add_test(
    NAME bloom.runtime.data_block_reader
    COMMAND bloom_runtime_data_block_reader_test
    LABELS runtime unit data
    TIMEOUT 30
)

bloom_add_test(
    NAME bloom.runtime.value_conversions
    COMMAND bloom_runtime_value_conversion_test
    LABELS runtime unit primitives text
    TIMEOUT 30
)

bloom_add_test(
    NAME bloom.runtime.string_nodes
    COMMAND bloom_runtime_string_node_test
    LABELS runtime unit text
    TIMEOUT 30
)

bloom_add_test(
    NAME bloom.runtime.value_math
    COMMAND bloom_runtime_value_math_test
    LABELS runtime unit primitives
    TIMEOUT 30
)

bloom_add_test(
    NAME bloom.runtime.cpu_composition_evaluator
    COMMAND bloom_runtime_cpu_composition_evaluator_test
    LABELS runtime unit render
    TIMEOUT 30
)

bloom_add_test(
    NAME bloom.runtime.animation_sampling
    COMMAND bloom_runtime_animation_sampling_test
    LABELS runtime unit primitives
    TIMEOUT 30
)

bloom_add_test(
    NAME bloom.runtime.curve_compilation
    COMMAND bloom_runtime_curve_compilation_test
    LABELS runtime unit
    TIMEOUT 30
)

bloom_add_test(
    NAME bloom.runtime.qualified_display_preparation
    COMMAND bloom_runtime_qualified_display_preparation_test
    LABELS runtime unit render color
    TIMEOUT 30
)

add_executable(bloom_image_source_test ${PROJECT_SOURCE_DIR}/tests/integration/media_image_source_tests.cpp)
target_include_directories(bloom_image_source_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/.. ${PROJECT_SOURCE_DIR}/src/media/image/tests)
bloom_find_dependency(OpenEXR)
target_link_libraries(bloom_image_source_test PRIVATE bloom_runtime_evaluator bloom_commands OpenEXR::OpenEXR)
bloom_enable_warnings(bloom_image_source_test)
bloom_add_test(NAME bloom.runtime.image-source COMMAND bloom_image_source_test LABELS unit runtime media)

add_executable(bloom_runtime_shape_sources_test ${PROJECT_SOURCE_DIR}/tests/shape_sources_tests.cpp)
target_link_libraries(bloom_runtime_shape_sources_test PRIVATE bloom_runtime_compiler bloom_runtime_evaluator bloom_commands)
bloom_enable_warnings(bloom_runtime_shape_sources_test)
bloom_enable_strict_floating_point(bloom_runtime_shape_sources_test)
bloom_add_test(NAME bloom.runtime.shape_sources COMMAND bloom_runtime_shape_sources_test LABELS unit rendering)

add_executable(bloom_composition_source_test composition_source_tests.cpp)
target_link_libraries(bloom_composition_source_test PRIVATE bloom_runtime_evaluator bloom_runtime_compiler bloom_media_audio)
bloom_enable_warnings(bloom_composition_source_test)
bloom_add_test(NAME bloom.runtime.composition-source COMMAND bloom_composition_source_test LABELS unit)

add_executable(bloom_runtime_memory_budget_ledger_test memory_budget_ledger_tests.cpp)
target_link_libraries(bloom_runtime_memory_budget_ledger_test PRIVATE bloom_runtime)
bloom_enable_warnings(bloom_runtime_memory_budget_ledger_test)
bloom_add_test(NAME bloom.runtime.memory_budget_ledger COMMAND bloom_runtime_memory_budget_ledger_test
              LABELS unit runtime TIMEOUT 30)

bloom_add_test(
    NAME bloom.runtime.input-color-context
    COMMAND bloom_runtime_input_color_context_test
    LABELS unit runtime media color
)

bloom_find_dependency(OpenColorIO)
add_executable(bloom_runtime_image_effect_test image_effect_tests.cpp)
target_include_directories(bloom_runtime_image_effect_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(bloom_runtime_image_effect_test PRIVATE bloom_runtime_evaluator OpenColorIO::OpenColorIO)
bloom_enable_warnings(bloom_runtime_image_effect_test)
bloom_enable_strict_floating_point(bloom_runtime_image_effect_test)
bloom_add_test(NAME bloom.runtime.image-effects COMMAND bloom_runtime_image_effect_test LABELS unit runtime color)

# Bounded runtime qualification for the embedded Bloom Neutral v1 display operation. The hardware
# body is compiled unconditionally and depends only on runtime create() availability, so the same
# target runs against the CPU stub; --loader/--require-device are supplied by the local supervisor
# for a native run. CTest runs it with no arguments, which prints an explicit skip on absent hardware.
add_executable(
    bloom_runtime_gpu_neutral_display_qualification_test
    gpu_neutral_display_qualification_tests.cpp
)
target_link_libraries(
    bloom_runtime_gpu_neutral_display_qualification_test
    PRIVATE bloom_runtime_evaluator
)
bloom_enable_warnings(bloom_runtime_gpu_neutral_display_qualification_test)
bloom_enable_strict_floating_point(bloom_runtime_gpu_neutral_display_qualification_test)
bloom_add_test(
    NAME bloom.runtime.gpu_neutral_display_qualification
    COMMAND bloom_runtime_gpu_neutral_display_qualification_test
    LABELS runtime unit gpu color
    TIMEOUT 120
)

# The GPU Neutral V1 display and GPU-resident Neutral V1 display tests need the independent OCIO CPU
# oracle from src/color, which src/render may not depend on. Their sources therefore live in this
# runtime test directory (runtime may depend on both render and color); the registered CTest names
# (bloom.render.gpu_neutral_display, bloom.render.gpu_resident_display) and labels are unchanged.
#
# The neutral display test also includes the render module's checked-in SPIR-V array directly, so it
# needs the render Vulkan include root, plus the private fence-override seam under
# BLOOM_RENDER_TEST_VULKAN_BACKEND. A sibling directory's BLOOM_RENDER_VULKAN_AVAILABLE is not visible
# here, so repeat the same mode-aware probe src/ui/tests uses.
set(BLOOM_RUNTIME_RENDER_VULKAN_AVAILABLE OFF)
if(BLOOM_RENDER_ENABLE_VULKAN AND DEFINED BLOOM_DEPENDENCY_MODE)
    if(BLOOM_DEPENDENCY_MODE STREQUAL "qualified")
        find_package(VulkanHeaders QUIET CONFIG
            PATHS "${BLOOM_DEPENDENCY_PREFIX}" NO_DEFAULT_PATH)
        find_package(VulkanMemoryAllocator QUIET CONFIG
            PATHS "${BLOOM_DEPENDENCY_PREFIX}" NO_DEFAULT_PATH)
    elseif(BLOOM_DEPENDENCY_MODE STREQUAL "developer-system")
        find_package(VulkanHeaders QUIET CONFIG)
        find_package(VulkanMemoryAllocator QUIET CONFIG)
    endif()
    if(TARGET Vulkan::Headers AND TARGET GPUOpen::VulkanMemoryAllocator)
        set(BLOOM_RUNTIME_RENDER_VULKAN_AVAILABLE ON)
    endif()
endif()

add_executable(bloom_render_gpu_neutral_display_test gpu_neutral_display_tests.cpp)
target_link_libraries(bloom_render_gpu_neutral_display_test PRIVATE bloom_render bloom_color_ocio)
target_include_directories(bloom_render_gpu_neutral_display_test
    PRIVATE "${PROJECT_SOURCE_DIR}/src/render/vulkan")
target_compile_definitions(bloom_render_gpu_neutral_display_test
    PRIVATE "BLOOM_NEUTRAL_DISPLAY_SPV_SHA256=\"fc0a97d36c42aafd3cfce56dcfdd7ff3c75d3ec7fd0e52a83c7f272c54feb3f9\"")
if(BLOOM_RUNTIME_RENDER_VULKAN_AVAILABLE)
    target_compile_definitions(bloom_render_gpu_neutral_display_test
        PRIVATE BLOOM_RENDER_TEST_VULKAN_BACKEND=1)
endif()
bloom_enable_warnings(bloom_render_gpu_neutral_display_test)
bloom_enable_strict_floating_point(bloom_render_gpu_neutral_display_test)
if(NOT BLOOM_RUNTIME_RENDER_VULKAN_AVAILABLE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # The GPU-only fixtures and cases are compiled out with the backend define, so in a CPU-stub
    # build they are legitimately unused; the CPU-oracle cases still run. This is the same
    # unused-function accommodation the media executor test targets use, and no assertion changes.
    # Placed after bloom_enable_warnings so it is not re-enabled by -Wall, and GNU/Clang-only so
    # MSVC never sees the option.
    target_compile_options(bloom_render_gpu_neutral_display_test PRIVATE -Wno-unused-function)
endif()
bloom_add_test(
    NAME bloom.render.gpu_neutral_display
    COMMAND bloom_render_gpu_neutral_display_test
    LABELS unit gpu color
)

add_executable(bloom_render_gpu_resident_display_test gpu_resident_display_tests.cpp)
target_link_libraries(bloom_render_gpu_resident_display_test PRIVATE bloom_render bloom_color_ocio)
bloom_enable_warnings(bloom_render_gpu_resident_display_test)
bloom_enable_strict_floating_point(bloom_render_gpu_resident_display_test)
bloom_add_test(
    NAME bloom.render.gpu_resident_display
    COMMAND bloom_render_gpu_resident_display_test
    LABELS unit gpu color
)

# GPU Neutral display preview product: adopts a successful native packed readback into the existing
# display-only product. The adoption/pointer cases run without hardware; the qualification-driven
# cases take --loader/--require-device from the local supervisor and skip cleanly otherwise.
add_executable(
    bloom_runtime_gpu_preview_display_product_test
    gpu_preview_display_product_tests.cpp
)
target_link_libraries(
    bloom_runtime_gpu_preview_display_product_test
    PRIVATE bloom_runtime_evaluator
)
bloom_enable_warnings(bloom_runtime_gpu_preview_display_product_test)
bloom_enable_strict_floating_point(bloom_runtime_gpu_preview_display_product_test)
bloom_add_test(
    NAME bloom.runtime.gpu_preview_display_product
    COMMAND bloom_runtime_gpu_preview_display_product_test
    LABELS runtime unit gpu color
    TIMEOUT 120
)

# GPU preview display service: dedicated service thread, native device/pipeline owner, stable final
# submitGpu handle, and bounded lifecycle. Non-device cases run unconditionally; native qualification,
# provenance/parity, deadline retirement, and the paired benchmark are driven with
# --loader/--require-device/--benchmark by the local supervisor.
add_executable(
    bloom_runtime_gpu_preview_display_service_test
    gpu_preview_display_service_tests.cpp
    gpu_preview_display_service_lifecycle_tests.cpp
)
target_link_libraries(
    bloom_runtime_gpu_preview_display_service_test
    PRIVATE bloom_runtime_evaluator
)
target_include_directories(bloom_runtime_gpu_preview_display_service_test PRIVATE ..)
bloom_enable_warnings(bloom_runtime_gpu_preview_display_service_test)
bloom_enable_strict_floating_point(bloom_runtime_gpu_preview_display_service_test)
bloom_add_test(
    NAME bloom.runtime.gpu_preview_display_service
    COMMAND bloom_runtime_gpu_preview_display_service_test
    LABELS runtime unit gpu color
    TIMEOUT 300
)

# Resident-route service dispatch: the narrow estimatePixels regression and the additive resident
# overload's disabled/status contract run without hardware. The real enabled Wayland acceptance is in
# the UI test tree; --loader/--require-device are supplied by the local supervisor.
add_executable(
    bloom_runtime_gpu_preview_display_service_resident_test
    gpu_preview_display_service_resident_tests.cpp
)
target_link_libraries(
    bloom_runtime_gpu_preview_display_service_resident_test
    PRIVATE bloom_runtime_evaluator
)
target_include_directories(bloom_runtime_gpu_preview_display_service_resident_test PRIVATE ..)
bloom_enable_warnings(bloom_runtime_gpu_preview_display_service_resident_test)
bloom_enable_strict_floating_point(bloom_runtime_gpu_preview_display_service_resident_test)
bloom_add_test(
    NAME bloom.runtime.gpu_preview_display_service_resident
    COMMAND bloom_runtime_gpu_preview_display_service_resident_test
    LABELS runtime unit gpu resident
    TIMEOUT 300
)

# Bounded owner-thread GPU scene content cache. It drives real resident images produced by SolidV1,
# so it depends only on runtime create() availability and runs against the CPU stub with an explicit
# skip; --loader/--require-device are supplied by the local supervisor for a native run.
add_executable(bloom_runtime_gpu_scene_cache_test gpu_scene_cache_tests.cpp)
target_link_libraries(bloom_runtime_gpu_scene_cache_test PRIVATE bloom_runtime_evaluator bloom_render)
bloom_enable_warnings(bloom_runtime_gpu_scene_cache_test)
bloom_enable_strict_floating_point(bloom_runtime_gpu_scene_cache_test)
bloom_add_test(
    NAME bloom.runtime.gpu_scene_cache
    COMMAND bloom_runtime_gpu_scene_cache_test
    LABELS runtime unit gpu render
    TIMEOUT 120
)

# CPU-side immutable GPU scene preparation (solid / translation-only / Normal merge / output). It
# drives a genuine CpuCompositionEvaluator oracle and needs no loader and no device; it is a pure
# CPU test that runs against the CPU stub too.
add_executable(bloom_runtime_gpu_scene_preparation_test gpu_scene_preparation_tests.cpp)
target_link_libraries(bloom_runtime_gpu_scene_preparation_test PRIVATE bloom_runtime_evaluator bloom_render)
bloom_enable_warnings(bloom_runtime_gpu_scene_preparation_test)
bloom_enable_strict_floating_point(bloom_runtime_gpu_scene_preparation_test)
bloom_add_test(
    NAME bloom.runtime.gpu_scene_preparation
    COMMAND bloom_runtime_gpu_scene_preparation_test
    LABELS runtime unit gpu render
    TIMEOUT 300
)

# Bounded prepared-upload cache: node lifetime under replacement/eviction, byte and entry ceilings,
# zero-budget disable, and per-entry accounting. CPU-only; no loader and no device.
add_executable(bloom_runtime_gpu_prepared_upload_cache_test gpu_prepared_upload_cache_tests.cpp)
target_link_libraries(bloom_runtime_gpu_prepared_upload_cache_test
    PRIVATE bloom_runtime_evaluator bloom_render)
bloom_enable_warnings(bloom_runtime_gpu_prepared_upload_cache_test)
bloom_enable_strict_floating_point(bloom_runtime_gpu_prepared_upload_cache_test)
bloom_add_test(
    NAME bloom.runtime.gpu_prepared_upload_cache
    COMMAND bloom_runtime_gpu_prepared_upload_cache_test
    LABELS unit gpu runtime)

# The media still-image test writes REAL OpenEXR fixtures to a temporary directory and resolves them
# through the production image pipeline, so it links OpenEXR directly exactly as
# bloom_media_image_test does. CPU-only: no loader, no device.
add_executable(bloom_runtime_gpu_media_scene_preparation_test
    gpu_media_scene_preparation_tests.cpp)
target_link_libraries(bloom_runtime_gpu_media_scene_preparation_test
    PRIVATE bloom_runtime_evaluator bloom_render OpenEXR::OpenEXR)
target_include_directories(bloom_runtime_gpu_media_scene_preparation_test
    PRIVATE ${OpenEXR_INCLUDE_DIRS} ${Imath_INCLUDE_DIRS})
bloom_enable_warnings(bloom_runtime_gpu_media_scene_preparation_test)
bloom_enable_strict_floating_point(bloom_runtime_gpu_media_scene_preparation_test)
bloom_add_test(
    NAME bloom.runtime.gpu_media_scene_preparation
    COMMAND bloom_runtime_gpu_media_scene_preparation_test
    LABELS unit gpu runtime)

# The real-video test reads the generated media-worker fixtures and drives the SAME
# VideoDecodeSession/worker the evaluator uses. It includes the private src/runtime/video_source.hpp
# only to read the decoded-cache resident-byte counter, and it must run with a worker built from the
# same provider archive as the host (a stale worker rejects the signed handshake). It is registered
# only where the FFmpeg worker and its fixture generator exist (the same Linux-only condition
# apps/bloom-media-worker uses), so a platform without an actual generated media-worker fixture never
# registers a test that could only skip.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(bloom_runtime_gpu_media_video_scene_preparation_test
        gpu_media_video_scene_preparation_tests.cpp)
    target_include_directories(bloom_runtime_gpu_media_video_scene_preparation_test
        PRIVATE ${PROJECT_SOURCE_DIR}/src/runtime)
    target_link_libraries(bloom_runtime_gpu_media_video_scene_preparation_test
        PRIVATE bloom_runtime_evaluator bloom_render bloom_media_video)
    bloom_enable_warnings(bloom_runtime_gpu_media_video_scene_preparation_test)
    bloom_enable_strict_floating_point(bloom_runtime_gpu_media_video_scene_preparation_test)
    add_dependencies(bloom_runtime_gpu_media_video_scene_preparation_test
        bloom-media-worker bloom_media_generate_fixtures)
    bloom_add_test(
        NAME bloom.runtime.gpu_media_video_scene_preparation
        COMMAND bloom_runtime_gpu_media_video_scene_preparation_test
                "${PROJECT_BINARY_DIR}/apps/bloom-media-worker/fixtures"
        LABELS unit gpu runtime media)
    set_tests_properties(bloom.runtime.gpu_media_video_scene_preparation
        PROPERTIES FIXTURES_REQUIRED media3)
endif()

# Owner-thread non-blocking GPU scene executor. It drives real GpuSolid/CoveredSolid/GpuComposite
# dispatches through a real device and a real GpuSceneCache; without a loader it skips unless the
# supervisor passes --loader/--require-device. It compiles and runs against the CPU stub too, printing
# an explicit skip there.
add_executable(bloom_runtime_gpu_scene_executor_test gpu_scene_executor_tests.cpp)
target_link_libraries(bloom_runtime_gpu_scene_executor_test PRIVATE bloom_runtime_evaluator bloom_render)
bloom_enable_warnings(bloom_runtime_gpu_scene_executor_test)
bloom_enable_strict_floating_point(bloom_runtime_gpu_scene_executor_test)
bloom_add_test(
    NAME bloom.runtime.gpu_scene_executor
    COMMAND bloom_runtime_gpu_scene_executor_test
    LABELS runtime unit gpu render
    TIMEOUT 600
)

# Test-only fault variant: the same tests compiled with the executor fault hook and linked against
# the fault-instrumented native render closure (defined in src/render/tests). Only this variant runs
# the native retirement vector (stalled deadline / unknown fence / device loss); the default target
# prints an explicit NOTE for it. The fault header and macro never reach a product build, and the
# installed production API is unchanged.
if(TARGET bloom_render_gpu_scene_executor_fault)
    add_executable(bloom_runtime_gpu_scene_executor_fault_test gpu_scene_executor_tests.cpp)
    target_compile_definitions(bloom_runtime_gpu_scene_executor_fault_test
        PRIVATE BLOOM_GPU_SCENE_EXECUTOR_TEST_FAULT_INJECTION)
    target_include_directories(bloom_runtime_gpu_scene_executor_fault_test
        PRIVATE "${PROJECT_SOURCE_DIR}/src/render/tests")
    target_link_libraries(bloom_runtime_gpu_scene_executor_fault_test
        PRIVATE bloom_render_gpu_scene_executor_fault bloom_runtime_evaluator bloom_render)
    bloom_enable_warnings(bloom_runtime_gpu_scene_executor_fault_test)
    bloom_enable_strict_floating_point(bloom_runtime_gpu_scene_executor_fault_test)
    bloom_add_test(
        NAME bloom.runtime.gpu_scene_executor_fault
        COMMAND bloom_runtime_gpu_scene_executor_fault_test
        LABELS runtime unit gpu render
        TIMEOUT 600
    )
endif()

# Media (still/sequence) extension of the executor: a real still-image/sequence plan through the
# CpuGpuSceneBuilder -> GpuImageUpload -> translation/source-over/output path on a real device. The
# media builder/media source objects come from the gpu-media-scene-prep package; this target links
# the same archives and OpenEXR/Imath the media scene preparation tests already use. Without a loader
# or device it skips unless the supervisor passes --loader/--require-device.
add_executable(bloom_runtime_gpu_media_executor_test
    gpu_media_executor_tests.cpp
    gpu_media_executor_test_support.hpp)
target_include_directories(bloom_runtime_gpu_media_executor_test
    PRIVATE ${OpenEXR_INCLUDE_DIRS} ${Imath_INCLUDE_DIRS})
target_link_libraries(bloom_runtime_gpu_media_executor_test
    PRIVATE bloom_runtime_evaluator bloom_render bloom_media_image bloom_media_video
            bloom_media_cache bloom_color bloom_color_ocio OpenEXR::OpenEXR)
bloom_enable_warnings(bloom_runtime_gpu_media_executor_test)
bloom_enable_strict_floating_point(bloom_runtime_gpu_media_executor_test)
# The media scene fixture/oracle header defines anonymous-namespace helpers that this TU does not
# all use; the proof compiles this one translation unit with unused-function quieted.
target_compile_options(bloom_runtime_gpu_media_executor_test PRIVATE -Wno-unused-function)
bloom_add_test(
    NAME bloom.runtime.gpu_media_executor
    COMMAND bloom_runtime_gpu_media_executor_test
    LABELS runtime unit gpu render media
    TIMEOUT 600
)

# Test-only fault variant: cancellation / deadline / unproven upload retention against a real
# submitted copy. It uses the same proof-only macro and header as the solid fault variant and links
# the fault-instrumented native closure, which must now also include gpu_image_upload.cpp and
# gpu_image_upload_resources.cpp so the injected upload poll hook is present.
if(TARGET bloom_render_gpu_scene_executor_fault)
    add_executable(bloom_runtime_gpu_media_executor_retirement_test
        gpu_media_executor_retirement_tests.cpp
        gpu_media_executor_test_support.hpp)
    target_compile_definitions(bloom_runtime_gpu_media_executor_retirement_test
        PRIVATE BLOOM_GPU_SCENE_EXECUTOR_TEST_FAULT_INJECTION)
    target_include_directories(bloom_runtime_gpu_media_executor_retirement_test
        PRIVATE "${PROJECT_SOURCE_DIR}/src/render/tests" ${OpenEXR_INCLUDE_DIRS}
                ${Imath_INCLUDE_DIRS})
    target_link_libraries(bloom_runtime_gpu_media_executor_retirement_test
        PRIVATE bloom_render_gpu_scene_executor_fault bloom_runtime_evaluator bloom_render
                bloom_media_image bloom_media_video bloom_media_cache bloom_color bloom_color_ocio
                OpenEXR::OpenEXR)
    bloom_enable_warnings(bloom_runtime_gpu_media_executor_retirement_test)
    bloom_enable_strict_floating_point(bloom_runtime_gpu_media_executor_retirement_test)
    # Same as the media executor target: the shared fixture/oracle header has helpers this TU does
    # not use.
    target_compile_options(bloom_runtime_gpu_media_executor_retirement_test
        PRIVATE -Wno-unused-function)
    bloom_add_test(
        NAME bloom.runtime.gpu_media_executor_retirement
        COMMAND bloom_runtime_gpu_media_executor_retirement_test
        LABELS runtime unit gpu render media
        TIMEOUT 600
    )
endif()

# Owner-bound GPU-resident frame lease + registry. The leased images are real SolidV1 ->
# GpuResidentDisplay results, so it depends only on runtime create() availability and runs against
# the CPU stub with an explicit skip. --loader/--require-device are supplied by the local supervisor
# for a native run; the default ctest invocation takes the platform loader. The native run includes
# a 1000-round concurrent UI-last-token-release vs owner collectExpired() stress.
add_executable(bloom_runtime_gpu_resident_frame_lease_test gpu_resident_frame_lease_tests.cpp)
target_link_libraries(
    bloom_runtime_gpu_resident_frame_lease_test
    PRIVATE bloom_runtime_evaluator bloom_render
)
bloom_enable_warnings(bloom_runtime_gpu_resident_frame_lease_test)
bloom_enable_strict_floating_point(bloom_runtime_gpu_resident_frame_lease_test)
bloom_add_test(
    NAME bloom.runtime.gpu_resident_frame_lease
    COMMAND bloom_runtime_gpu_resident_frame_lease_test
    LABELS runtime unit gpu render
    TIMEOUT 120
)

# GPU-resident preview product: the fourth, closed PreparedPreviewFrame arm. On a real device it
# produces a real Solid -> resident-display -> owner-registry lease with a genuine
# qualifyResidentPreview() report, then validates the product factory and its rejection/invalidation
# gates. Without a device it skips cleanly; --loader/--require-device are supplied by the supervisor.
add_executable(
    bloom_runtime_gpu_resident_preview_product_test
    gpu_resident_preview_product_tests.cpp
)
target_link_libraries(
    bloom_runtime_gpu_resident_preview_product_test
    PRIVATE bloom_runtime_evaluator
)
bloom_enable_warnings(bloom_runtime_gpu_resident_preview_product_test)
bloom_enable_strict_floating_point(bloom_runtime_gpu_resident_preview_product_test)
bloom_add_test(
    NAME bloom.runtime.gpu_resident_preview_product
    COMMAND bloom_runtime_gpu_resident_preview_product_test
    LABELS runtime unit gpu color render
    TIMEOUT 300
)

# Resident-preview runtime qualification. The native proof creates a real device + typed pipelines,
# runs qualifyResidentPreview() (all-pixel parity against the CPU primitives and OCIO oracle), and
# exercises the ownership/foreign-device/processor/cancel/budget/wrong-thread gates. It includes the
# embedded shader arrays directly and re-hashes them against the report pins. It is hardware-gated by
# runtime create() availability and --require-device, so the same target runs against the CPU stub
# with an explicit skip; --loader/--require-device come from the local supervisor for a native run.
add_executable(
    bloom_runtime_gpu_resident_preview_qualification_test
    gpu_resident_preview_qualification_tests.cpp
)
target_include_directories(
    bloom_runtime_gpu_resident_preview_qualification_test
    PRIVATE "${PROJECT_SOURCE_DIR}/src/render/vulkan"
)
target_link_libraries(
    bloom_runtime_gpu_resident_preview_qualification_test
    PRIVATE bloom_runtime_evaluator
)
bloom_enable_warnings(bloom_runtime_gpu_resident_preview_qualification_test)
bloom_enable_strict_floating_point(bloom_runtime_gpu_resident_preview_qualification_test)
bloom_add_test(
    NAME bloom.runtime.gpu_resident_preview_qualification
    COMMAND bloom_runtime_gpu_resident_preview_qualification_test
    LABELS runtime unit gpu color
    TIMEOUT 600
)

# Portable stub predicate harness: the resident-preview qualification runtime sources must compile,
# link, and run with NO Vulkan dependency. The stub render backend honestly reports no device, so this
# exercises the CPU-only identity predicates and report vocabulary instead of a fake GPU success. The
# actual device proof is the target above; this one is built by the Vulkan-free build/gpu-device-stub.
add_executable(
    bloom_runtime_gpu_resident_preview_qualification_stub_test
    gpu_resident_preview_qualification_stub_tests.cpp
)
target_link_libraries(
    bloom_runtime_gpu_resident_preview_qualification_stub_test
    PRIVATE bloom_runtime_evaluator
)
bloom_enable_warnings(bloom_runtime_gpu_resident_preview_qualification_stub_test)
bloom_enable_strict_floating_point(bloom_runtime_gpu_resident_preview_qualification_stub_test)
bloom_add_test(
    NAME bloom.runtime.gpu_resident_preview_qualification_stub
    COMMAND bloom_runtime_gpu_resident_preview_qualification_stub_test
    LABELS runtime unit gpu color
    TIMEOUT 120
)

# Presentation coordinator: a CPU-only port validation (shared mailbox, typed admission, coalescing,
# stale sequence, duplicate-surface ownership, fail-closed teardown) and a CPU-only
# ownership/bounds/forget gate (fixed process quarantine reservations, capacity recovery without a
# permanent fuse, terminal forget, thousands of attach/retire/forget cycles). Both are Qt-free and
# Vulkan-free and need no device or owner thread; the real Wayland lifecycle proof is
# src/ui/tests/gpu_presentation_coordinator_tests.cpp.
add_executable(bloom_runtime_gpu_presentation_coordinator_port_test
               gpu_presentation_coordinator_port_tests.cpp)
target_include_directories(bloom_runtime_gpu_presentation_coordinator_port_test PRIVATE ..)
target_link_libraries(bloom_runtime_gpu_presentation_coordinator_port_test
                      PRIVATE bloom_runtime_evaluator)
bloom_enable_warnings(bloom_runtime_gpu_presentation_coordinator_port_test)
bloom_add_test(
    NAME bloom.runtime.gpu_presentation_coordinator_port
    COMMAND bloom_runtime_gpu_presentation_coordinator_port_test
    LABELS runtime unit gpu presentation
    TIMEOUT 60
)

add_executable(bloom_runtime_gpu_presentation_coordinator_bounds_test
               gpu_presentation_coordinator_bounds_tests.cpp)
target_include_directories(bloom_runtime_gpu_presentation_coordinator_bounds_test PRIVATE ..)
target_link_libraries(bloom_runtime_gpu_presentation_coordinator_bounds_test
                      PRIVATE bloom_runtime_evaluator)
bloom_enable_warnings(bloom_runtime_gpu_presentation_coordinator_bounds_test)
bloom_add_test(
    NAME bloom.runtime.gpu_presentation_coordinator_bounds
    COMMAND bloom_runtime_gpu_presentation_coordinator_bounds_test
    LABELS runtime unit gpu presentation
    TIMEOUT 60
)
