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

# Capacity-aware resident preview admission policy. The pure cases need no device; the optional
# native owner case creates one device and resolves its real allocation budget, then uses compact
# arithmetic (no large allocations). --loader/--require-device are supplied by the local supervisor.
add_executable(bloom_runtime_gpu_preview_resident_capacity_test
    gpu_preview_resident_capacity_tests.cpp)
target_link_libraries(bloom_runtime_gpu_preview_resident_capacity_test
    PRIVATE bloom_runtime_evaluator bloom_render)
bloom_enable_warnings(bloom_runtime_gpu_preview_resident_capacity_test)
bloom_enable_strict_floating_point(bloom_runtime_gpu_preview_resident_capacity_test)
bloom_add_test(
    NAME bloom.runtime.gpu_preview_resident_capacity
    COMMAND bloom_runtime_gpu_preview_resident_capacity_test
    LABELS runtime unit gpu render
    TIMEOUT 120
)

# CPU-side immutable GPU scene preparation (solid / translation-only / Normal merge / output). It
# drives a genuine CpuCompositionEvaluator oracle and needs no loader and no device; it is a pure
# CPU test that runs against the CPU stub too.
add_executable(bloom_runtime_gpu_scene_preparation_test gpu_scene_preparation_tests.cpp)
target_include_directories(bloom_runtime_gpu_scene_preparation_test PRIVATE "${PROJECT_SOURCE_DIR}/src/runtime")
target_link_libraries(bloom_runtime_gpu_scene_preparation_test PRIVATE bloom_runtime_evaluator bloom_render)
bloom_enable_warnings(bloom_runtime_gpu_scene_preparation_test)
bloom_enable_strict_floating_point(bloom_runtime_gpu_scene_preparation_test)
bloom_add_test(
    NAME bloom.runtime.gpu_scene_preparation
    COMMAND bloom_runtime_gpu_scene_preparation_test
    LABELS runtime unit gpu render
    TIMEOUT 300
)

# CompiledCompositionSource nested compositions prepared by the production builder: genuine child
# GPU commands spliced into the parent command list, checked bit-for-bit against the unchanged CPU
# evaluator oracle. Pure CPU: no loader, no device.
add_executable(bloom_runtime_gpu_nested_scene_preparation_test
    gpu_nested_scene_preparation_tests.cpp)
target_include_directories(bloom_runtime_gpu_nested_scene_preparation_test PRIVATE "${PROJECT_SOURCE_DIR}/src/runtime")
target_link_libraries(bloom_runtime_gpu_nested_scene_preparation_test
    PRIVATE bloom_runtime_evaluator bloom_render)
bloom_enable_warnings(bloom_runtime_gpu_nested_scene_preparation_test)
bloom_enable_strict_floating_point(bloom_runtime_gpu_nested_scene_preparation_test)
bloom_add_test(
    NAME bloom.runtime.gpu_nested_scene_preparation
    COMMAND bloom_runtime_gpu_nested_scene_preparation_test
    LABELS runtime unit gpu render
    TIMEOUT 300
)

# Real native parity for nested compositions: production builder -> production executor ->
# readback vs the CPU evaluator, with positive cold dispatch, warm zero dispatch, and an unrelated
# cached child branch preserved across a single-branch edit. Skips honestly (exit 77) without a
# device; --require-device makes an unavailable device a failure.
add_executable(bloom_runtime_gpu_nested_scene_native_test gpu_nested_scene_native_tests.cpp)
target_include_directories(bloom_runtime_gpu_nested_scene_native_test
    PRIVATE "${PROJECT_SOURCE_DIR}/src/render/tests" "${PROJECT_SOURCE_DIR}/src/runtime")
target_link_libraries(bloom_runtime_gpu_nested_scene_native_test
    PRIVATE bloom_runtime_evaluator bloom_render)
bloom_enable_warnings(bloom_runtime_gpu_nested_scene_native_test)
bloom_enable_strict_floating_point(bloom_runtime_gpu_nested_scene_native_test)
bloom_add_test(
    NAME bloom.runtime.gpu_nested_scene_native
    COMMAND bloom_runtime_gpu_nested_scene_native_test
    LABELS runtime native gpu render
    TIMEOUT 300
)
set_tests_properties(bloom.runtime.gpu_nested_scene_native PROPERTIES SKIP_RETURN_CODE 77)

# Bounded GPU coverage contract gate. It enumerates every CompiledOperation/ImageEffectKernel
# alternative, feature axis, render route, and built-in pixel node type from the same contract,
# drives the REAL production CpuGpuSceneBuilder over one genuine fixture per required id, and
# reports the current holes by name. It is expected RED until the required production fixtures
# exist; it must then turn green without weakening the contract or adding opt-outs. CPU-only.
#
# -Werror=switch makes the contract's no-default enumerator switches (ShapeKind, NodeLoweringKind)
# a compile failure when a new enumerator is added without a classification.
# The generated media-worker fixtures are wired automatically: the coverage target depends on the
# existing bloom_media_generate_fixtures generator and consumes the CTest `media3` fixture setup, so
# no caller has to supply a directory. On a platform without that generator the path stays empty and
# the video fixture reports an explicit test-environment-missing result rather than a fake pass.
set(BLOOM_GPU_COVERAGE_MEDIA_FIXTURES "")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(BLOOM_GPU_COVERAGE_MEDIA_FIXTURES "${PROJECT_BINARY_DIR}/apps/bloom-media-worker/fixtures")
endif()

# Route-proof handoff. The setup fixture creates a fresh run nonce in this build-owned directory and
# clears ONLY the six known proof files. Genuine harnesses publish their own proofs; the native gate
# consumes only proofs whose nonce and schema match, so an absent or stale proof keeps the route RED.
set(BLOOM_GPU_ROUTE_PROOF_DIR "${PROJECT_BINARY_DIR}/gpu-route-proofs")

add_executable(bloom_runtime_gpu_coverage_contract_test gpu_coverage_contract_tests.cpp)
target_link_libraries(bloom_runtime_gpu_coverage_contract_test
    PRIVATE bloom_runtime_evaluator bloom_render bloom_media_video OpenEXR::OpenEXR)
target_include_directories(bloom_runtime_gpu_coverage_contract_test
    PRIVATE ${OpenEXR_INCLUDE_DIRS} ${Imath_INCLUDE_DIRS}
            "${PROJECT_SOURCE_DIR}/tests/support")
# The shared runtime OCIO context for the effect/working-space fixtures is built from the REAL tools
# staged beside this gate executable by bloom_package_gpu_shader_tools (the same packaging the
# desktop/CLI/MCP targets use), never a manually injected or ambient path. The executable-relative
# definition is what the resolver validates.
target_compile_definitions(bloom_runtime_gpu_coverage_contract_test PRIVATE
    "BLOOM_GPU_COVERAGE_TEST_EXECUTABLE=\"$<TARGET_FILE:bloom_runtime_gpu_coverage_contract_test>\"")
include(BloomGpuShaderTools)
bloom_package_gpu_shader_tools(bloom_runtime_gpu_coverage_contract_test)
# apps/bloom-media-worker is added after src/runtime, so a TARGET guard here would be false at
# configure time; mirror the video-scene-preparation tests and take the real worker + fixture
# generator as build-order dependencies on the platform that defines them.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_dependencies(bloom_runtime_gpu_coverage_contract_test
        bloom-media-worker bloom_media_generate_fixtures)
endif()
bloom_enable_warnings(bloom_runtime_gpu_coverage_contract_test)
if(MSVC)
    target_compile_options(bloom_runtime_gpu_coverage_contract_test PRIVATE /we4062)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(bloom_runtime_gpu_coverage_contract_test PRIVATE -Werror=switch)
endif()
bloom_enable_strict_floating_point(bloom_runtime_gpu_coverage_contract_test)
bloom_add_test(
    NAME bloom.runtime.gpu_coverage_contract
    COMMAND bloom_runtime_gpu_coverage_contract_test
            --media-fixtures "${BLOOM_GPU_COVERAGE_MEDIA_FIXTURES}"
    LABELS runtime unit gpu contract
    TIMEOUT 300
)

# Native acceptance is a distinct CTest from the CPU gate. With a device it must actually execute
# the native routes/fixtures; without one it must SKIP (exit 77), and `--require-device` must hard
# fail. It never reports a native pass it did not earn.
bloom_add_test(
    NAME bloom.runtime.gpu_coverage_native_acceptance
    COMMAND bloom_runtime_gpu_coverage_contract_test --native-acceptance
            --media-fixtures "${BLOOM_GPU_COVERAGE_MEDIA_FIXTURES}"
            --route-proof-dir "${BLOOM_GPU_ROUTE_PROOF_DIR}"
    LABELS runtime native gpu contract
    TIMEOUT 600
)
set_tests_properties(bloom.runtime.gpu_coverage_native_acceptance
    PROPERTIES SKIP_RETURN_CODE 77)

# The generated video fixtures are owned by the media worker's `media3` setup, exactly like the
# existing video-scene-preparation tests. The route-proof setup fixture is separate and always
# required by the native gate; it publishes no proof, only a fresh nonce.
#
# The gate consumes the genuine proofs the real route harnesses publish, so it must run after them.
# The harnesses live in the UI test tree (they need the Qt shell) and each sets up its own fixture;
# the names are listed here because a runtime test may not set a property on a test that lives in
# another directory. A required fixture with no setup test is treated as satisfied, so a build with
# no UI shell (or a platform without these harnesses) keeps the gate's previous behaviour and never
# fabricates a proof. The viewer and RAM-preview writers are this workstream's; the remaining export
# writers add their own fixture names to this same list.
set(BLOOM_GPU_ROUTE_PROOF_WRITER_FIXTURES
    gpu_route_proof_viewer
    gpu_route_proof_ram)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set_tests_properties(bloom.runtime.gpu_coverage_contract PROPERTIES FIXTURES_REQUIRED media3)
    # The export route-proof writers are Linux-only (the host/scripting harnesses they extend are),
    # so the native acceptance fixture chain includes their writers only on Linux. The chained
    # fixtures guarantee order: gpu_route_proofs (fresh nonce) -> gpu_route_proof_writers ->
    # native acceptance.
    set_tests_properties(bloom.runtime.gpu_coverage_native_acceptance
        PROPERTIES FIXTURES_REQUIRED
            "media3;gpu_route_proofs;gpu_route_proof_writers;${BLOOM_GPU_ROUTE_PROOF_WRITER_FIXTURES}")
else()
    set_tests_properties(bloom.runtime.gpu_coverage_native_acceptance
        PROPERTIES FIXTURES_REQUIRED
            "gpu_route_proofs;${BLOOM_GPU_ROUTE_PROOF_WRITER_FIXTURES}")
endif()

# Route-proof handoff tests. Both targets include the Qt-free shared codec from tests/support through
# an explicit PRIVATE include directory; they link no product library, so no dependency is rebuilt.
# The setup test is the CTest fixture that creates the fresh nonce and clears only the six known
# proof files. The codec test uses clearly synthetic, unit-only proofs in a private temp directory;
# none of them is ever published as route acceptance.
set(BLOOM_GPU_ROUTE_PROOF_TEST_INCLUDES
    "${PROJECT_SOURCE_DIR}/tests/support"
    "${PROJECT_SOURCE_DIR}/src/runtime/include"
    "${PROJECT_SOURCE_DIR}/src/core/include"
    "${PROJECT_SOURCE_DIR}/src/document/include"
    "${PROJECT_SOURCE_DIR}/src/render/include")

add_executable(bloom_runtime_gpu_coverage_route_proof_setup_test
    gpu_coverage_route_proof_setup_tests.cpp)
target_include_directories(bloom_runtime_gpu_coverage_route_proof_setup_test
    PRIVATE ${BLOOM_GPU_ROUTE_PROOF_TEST_INCLUDES})
target_compile_features(bloom_runtime_gpu_coverage_route_proof_setup_test PRIVATE cxx_std_20)
bloom_enable_warnings(bloom_runtime_gpu_coverage_route_proof_setup_test)
bloom_add_test(
    NAME bloom.runtime.gpu_coverage_route_proof_setup
    COMMAND bloom_runtime_gpu_coverage_route_proof_setup_test "${BLOOM_GPU_ROUTE_PROOF_DIR}"
    LABELS runtime unit contract
    TIMEOUT 60
)
set_tests_properties(bloom.runtime.gpu_coverage_route_proof_setup
    PROPERTIES FIXTURES_SETUP gpu_route_proofs)

add_executable(bloom_runtime_gpu_coverage_route_proof_io_test
    gpu_coverage_route_proof_io_tests.cpp)
target_include_directories(bloom_runtime_gpu_coverage_route_proof_io_test
    PRIVATE ${BLOOM_GPU_ROUTE_PROOF_TEST_INCLUDES})
target_compile_features(bloom_runtime_gpu_coverage_route_proof_io_test PRIVATE cxx_std_20)
bloom_enable_warnings(bloom_runtime_gpu_coverage_route_proof_io_test)
bloom_add_test(
    NAME bloom.runtime.gpu_coverage_route_proof_io
    COMMAND bloom_runtime_gpu_coverage_route_proof_io_test
    LABELS runtime unit contract
    TIMEOUT 60
)

# Mutation proof: each way a new pixel operation/effect/feature can be introduced without
# classification must fail to compile, and each must fail on its expected reason. All probe targets
# are excluded from the default build; the check script builds the positive control first, then each
# negative probe, and asserts the diagnostic. The built-in-node mutation (a runtime registry proof)
# lives in the coverage gate itself; a new node type sharing an existing lowering gains a required
# id with no fixture and is reported RED by name.
add_library(bloom_runtime_gpu_coverage_contract_positive_probe OBJECT EXCLUDE_FROM_ALL
    gpu_coverage_contract_positive_probe.cpp)
target_link_libraries(bloom_runtime_gpu_coverage_contract_positive_probe
    PRIVATE bloom_runtime_compiler)
bloom_enable_warnings(bloom_runtime_gpu_coverage_contract_positive_probe)
if(MSVC)
    target_compile_options(bloom_runtime_gpu_coverage_contract_positive_probe PRIVATE /we4062)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(bloom_runtime_gpu_coverage_contract_positive_probe PRIVATE
        -Werror=switch)
endif()

add_library(bloom_runtime_gpu_coverage_contract_negative_probe OBJECT EXCLUDE_FROM_ALL
    gpu_coverage_contract_negative_probe.cpp)
target_link_libraries(bloom_runtime_gpu_coverage_contract_negative_probe
    PRIVATE bloom_runtime_compiler)
bloom_enable_warnings(bloom_runtime_gpu_coverage_contract_negative_probe)

add_library(bloom_runtime_gpu_coverage_contract_effect_negative_probe OBJECT EXCLUDE_FROM_ALL
    gpu_coverage_contract_effect_negative_probe.cpp)
target_link_libraries(bloom_runtime_gpu_coverage_contract_effect_negative_probe
    PRIVATE bloom_runtime_compiler)
bloom_enable_warnings(bloom_runtime_gpu_coverage_contract_effect_negative_probe)

# A new feature enumerator is guarded by the contract's no-default switches under -Werror=switch;
# this probe proves that guard is actually enabled in the build by failing on a missing case.
add_library(bloom_runtime_gpu_coverage_contract_feature_negative_probe OBJECT EXCLUDE_FROM_ALL
    gpu_coverage_contract_feature_negative_probe.cpp)
target_link_libraries(bloom_runtime_gpu_coverage_contract_feature_negative_probe
    PRIVATE bloom_runtime_compiler)
bloom_enable_warnings(bloom_runtime_gpu_coverage_contract_feature_negative_probe)
if(MSVC)
    target_compile_options(bloom_runtime_gpu_coverage_contract_feature_negative_probe PRIVATE
        /we4062)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(bloom_runtime_gpu_coverage_contract_feature_negative_probe PRIVATE
        -Werror=switch)
endif()

add_test(
    NAME bloom.runtime.gpu_coverage_contract_mutation_proof
    COMMAND ${CMAKE_COMMAND}
            "-DBLOOM_BUILD_DIR=${CMAKE_BINARY_DIR}"
            "-DBLOOM_PROBE_POSITIVE=bloom_runtime_gpu_coverage_contract_positive_probe"
            "-DBLOOM_PROBE_NEGATIVE_OPERATION=bloom_runtime_gpu_coverage_contract_negative_probe"
            "-DBLOOM_REASON_OPERATION_A=GpuOperationCoverageTraits"
            "-DBLOOM_REASON_OPERATION_B=SyntheticUnclassifiedOperation"
            "-DBLOOM_PROBE_NEGATIVE_EFFECT=bloom_runtime_gpu_coverage_contract_effect_negative_probe"
            "-DBLOOM_REASON_EFFECT_A=GpuImageEffectCoverageTraits"
            "-DBLOOM_REASON_EFFECT_B=SyntheticUnclassifiedKernel"
            "-DBLOOM_PROBE_NEGATIVE_FEATURE=bloom_runtime_gpu_coverage_contract_feature_negative_probe"
            "-DBLOOM_REASON_FEATURE_A=SyntheticFeatureEnum"
            "-DBLOOM_REASON_FEATURE_B=Wswitch"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/gpu_coverage_negative_probe_check.cmake")
set_tests_properties(bloom.runtime.gpu_coverage_contract_mutation_proof
    PROPERTIES LABELS runtime quality contract TIMEOUT 240)

# Ownership regression for the coverage contract registries. Generated names must be owned by the
# returned entry, never a view into a temporary. The test retains each registry, churns the
# allocator with poison blocks, then re-reads every name; it needs no library because the registries
# are header-only. The ASAN variant of the same TU turns a stale read into a hard
# heap-use-after-free and links no already-built library, so it rebuilds no dependency.
add_executable(bloom_runtime_gpu_coverage_registry_ownership_test
    gpu_coverage_registry_ownership_tests.cpp)
target_include_directories(bloom_runtime_gpu_coverage_registry_ownership_test
    PRIVATE "${PROJECT_SOURCE_DIR}/src/runtime/include" "${PROJECT_SOURCE_DIR}/src/core/include"
            "${PROJECT_SOURCE_DIR}/src/document/include" "${PROJECT_SOURCE_DIR}/src/render/include")
target_compile_features(bloom_runtime_gpu_coverage_registry_ownership_test PRIVATE cxx_std_20)
bloom_enable_warnings(bloom_runtime_gpu_coverage_registry_ownership_test)
bloom_add_test(
    NAME bloom.runtime.gpu_coverage_registry_ownership
    COMMAND bloom_runtime_gpu_coverage_registry_ownership_test
    LABELS runtime unit contract
    TIMEOUT 60
)
if(CMAKE_CXX_COMPILER_ID MATCHES "^(AppleClang|Clang|GNU)$")
    add_executable(bloom_runtime_gpu_coverage_registry_ownership_asan_test
        gpu_coverage_registry_ownership_tests.cpp)
    target_include_directories(bloom_runtime_gpu_coverage_registry_ownership_asan_test
        PRIVATE "${PROJECT_SOURCE_DIR}/src/runtime/include" "${PROJECT_SOURCE_DIR}/src/core/include"
                "${PROJECT_SOURCE_DIR}/src/document/include" "${PROJECT_SOURCE_DIR}/src/render/include")
    target_compile_features(bloom_runtime_gpu_coverage_registry_ownership_asan_test PRIVATE cxx_std_20)
    target_compile_options(bloom_runtime_gpu_coverage_registry_ownership_asan_test
        PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(bloom_runtime_gpu_coverage_registry_ownership_asan_test
        PRIVATE -fsanitize=address)
    bloom_enable_warnings(bloom_runtime_gpu_coverage_registry_ownership_asan_test)
    bloom_add_test(
        NAME bloom.runtime.gpu_coverage_registry_ownership_asan
        COMMAND bloom_runtime_gpu_coverage_registry_ownership_asan_test
        LABELS runtime unit contract sanitizer
        TIMEOUT 60
    )
endif()

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
    PRIVATE ${OpenEXR_INCLUDE_DIRS} ${Imath_INCLUDE_DIRS}
            "${PROJECT_SOURCE_DIR}/src/runtime")
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

# Bounded GPU final-render/export bridge: the real plan -> CpuGpuSceneBuilder -> GpuSceneExecutor
# -> production GpuProcessReadback -> GPU-provenance ProcessFrame path, checked against the CPU
# reference oracle for process pixels, dispatch counters, and provenance. The canonical output
# semantic identity is proven in bloom.host (src/host/tests/gpu_process_frame_identity_tests.cpp)
# because the runtime module must not depend on bloom::output, even in tests. Uses the same loader/
# --require-device contract and skips explicitly without a device.
add_executable(bloom_runtime_gpu_process_frame_test gpu_process_frame_tests.cpp)
target_link_libraries(bloom_runtime_gpu_process_frame_test
    PRIVATE bloom_runtime_evaluator bloom_render)
bloom_enable_warnings(bloom_runtime_gpu_process_frame_test)
bloom_enable_strict_floating_point(bloom_runtime_gpu_process_frame_test)
bloom_add_test(
    NAME bloom.runtime.gpu_process_frame
    COMMAND bloom_runtime_gpu_process_frame_test
    LABELS runtime unit gpu render export
    TIMEOUT 600
)

# Focused real-device live-pin ref-count regression for the executor (audit F1) plus the
# upload-cancel branch consistency fix (audit F2). It runs its own 24-level distinct-index merge DAG
# under a budget of a known live-image count times the probe-measured actual per-image allocation,
# independent of the buggy whole-graph retained peak; the unchanged executor suite above keeps its
# own coverage. Same loader/--require-device contract.
add_executable(bloom_runtime_gpu_live_pin_focus_test gpu_live_pin_focus_tests.cpp)
target_link_libraries(bloom_runtime_gpu_live_pin_focus_test
    PRIVATE bloom_runtime_evaluator bloom_render)
bloom_enable_warnings(bloom_runtime_gpu_live_pin_focus_test)
bloom_enable_strict_floating_point(bloom_runtime_gpu_live_pin_focus_test)
bloom_add_test(
    NAME bloom.runtime.gpu_live_pin_focus
    COMMAND bloom_runtime_gpu_live_pin_focus_test
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
    PRIVATE ${OpenEXR_INCLUDE_DIRS} ${Imath_INCLUDE_DIRS}
            "${PROJECT_SOURCE_DIR}/src/runtime")
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

# The bounded media-source GPU colour split: a real AP0 EXR -> CpuGpuSceneBuilder (with the shared
# GpuSceneOcioContext) -> RAW full-source upload + real input->working OCIO ProcessEffect ->
# GpuSceneExecutor on a real device, checked against the unchanged CpuCompositionEvaluator frame at
# 2e-6. The OCIO program is compiled with the pinned tools; without them the test skips. Without a
# device it notes the skipped native route unless --require-device is passed.
add_executable(bloom_runtime_gpu_media_color_test gpu_media_color_tests.cpp)
target_include_directories(bloom_runtime_gpu_media_color_test
    PRIVATE ${OpenEXR_INCLUDE_DIRS} ${Imath_INCLUDE_DIRS}
            "${PROJECT_SOURCE_DIR}/src/runtime")
target_link_libraries(bloom_runtime_gpu_media_color_test
    PRIVATE bloom_runtime_evaluator bloom_render bloom_media_image bloom_media_video
            bloom_media_cache bloom_color bloom_color_ocio OpenEXR::OpenEXR)
# The OCIO tool discovery below this target has not run yet at this point in the file, so resolve the
# pinned tools locally (find_program caches, so this is the same result).
find_program(bloom_media_color_glslang_validator
    NAMES glslangValidator
    HINTS "${BLOOM_DEPENDENCY_PREFIX}/bin"
    NO_DEFAULT_PATH)
find_program(bloom_media_color_spirv_val
    NAMES spirv-val
    HINTS "${BLOOM_DEPENDENCY_PREFIX}/bin"
    NO_DEFAULT_PATH)
if(bloom_media_color_glslang_validator AND bloom_media_color_spirv_val)
    get_filename_component(bloom_media_color_tools_dir
        "${bloom_media_color_glslang_validator}" DIRECTORY)
    target_compile_definitions(bloom_runtime_gpu_media_color_test PRIVATE
        "BLOOM_GPUSHADER_TOOLS_DIR=\"${bloom_media_color_tools_dir}\"")
endif()
bloom_enable_warnings(bloom_runtime_gpu_media_color_test)
bloom_enable_strict_floating_point(bloom_runtime_gpu_media_color_test)
target_compile_options(bloom_runtime_gpu_media_color_test PRIVATE -Wno-unused-function)
# The real decoded video vectors need the media worker and its generated fixtures. They are
# registered only where those exist (the same Linux-only condition apps/bloom-media-worker uses);
# elsewhere the test prints an explicit NOTE for the video vectors rather than passing them.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_dependencies(bloom_runtime_gpu_media_color_test
        bloom-media-worker bloom_media_generate_fixtures)
    bloom_add_test(
        NAME bloom.runtime.gpu_media_color
        COMMAND bloom_runtime_gpu_media_color_test
                --fixtures "${PROJECT_BINARY_DIR}/apps/bloom-media-worker/fixtures"
        LABELS runtime unit gpu render media color
        TIMEOUT 600
    )
    set_tests_properties(bloom.runtime.gpu_media_color PROPERTIES FIXTURES_REQUIRED media3)
else()
    bloom_add_test(
        NAME bloom.runtime.gpu_media_color
        COMMAND bloom_runtime_gpu_media_color_test
        LABELS runtime unit gpu render media color
        TIMEOUT 600
    )
endif()

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
                ${Imath_INCLUDE_DIRS} "${PROJECT_SOURCE_DIR}/src/runtime")
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

# Runtime-native parity proof for GpuAffine's composed-matrix form against the ACTUAL runtime parented
# CPU oracle (ParentedLayerTransform::row). It lives here because runtime may depend on render while
# production render may not depend on runtime. It uses only the public render API, so it compiles and
# registers in both the Vulkan and stub builds; without a device it skips cleanly (and
# --require-device fails closed).
add_executable(bloom_runtime_gpu_affine_parented_test gpu_affine_parented_native_tests.cpp)
target_include_directories(
    bloom_runtime_gpu_affine_parented_test
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src/render/tests"
        "${PROJECT_SOURCE_DIR}/src/runtime"
)
target_link_libraries(
    bloom_runtime_gpu_affine_parented_test
    PRIVATE bloom_runtime_evaluator bloom_render
)
bloom_enable_warnings(bloom_runtime_gpu_affine_parented_test)
bloom_enable_strict_floating_point(bloom_runtime_gpu_affine_parented_test)
bloom_add_test(
    NAME bloom.runtime.gpu_affine_parented
    COMMAND bloom_runtime_gpu_affine_parented_test
    LABELS runtime unit gpu render
    TIMEOUT 180
)

# Real-device executor parity for the new affine + blend scene commands: real PreparedGpuScene
# (proof-only friend fixture) -> production GpuSceneExecutor -> readback vs the CPU affine/blend
# oracles, with family counters, warm-zero-dispatch, and budget refusal. Same loader/--require-device
# contract; skips cleanly without a device.
add_executable(bloom_runtime_gpu_affine_blend_executor_test
    gpu_affine_blend_executor_native_tests.cpp
)
target_include_directories(
    bloom_runtime_gpu_affine_blend_executor_test
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src/render/tests"
        "${PROJECT_SOURCE_DIR}/src/runtime"
)
target_link_libraries(
    bloom_runtime_gpu_affine_blend_executor_test
    PRIVATE bloom_runtime_evaluator bloom_render
)
bloom_enable_warnings(bloom_runtime_gpu_affine_blend_executor_test)
bloom_enable_strict_floating_point(bloom_runtime_gpu_affine_blend_executor_test)
bloom_add_test(
    NAME bloom.runtime.gpu_affine_blend_executor
    COMMAND bloom_runtime_gpu_affine_blend_executor_test
    LABELS runtime unit gpu render
    TIMEOUT 300
)

# Test-only fault variant of the affine/blend executor parity proof. It compiles the same TU under
# the proof-only macro and links the fault-instrumented native closure, which now also includes
# gpu_affine/gpu_blend. Only this variant exercises affine/blend stalled/unknown-fence/device-loss
# retirement; the default target stays fault-free.
if(TARGET bloom_render_gpu_scene_executor_fault)
    add_executable(bloom_runtime_gpu_affine_blend_executor_fault_test
        gpu_affine_blend_executor_native_tests.cpp
    )
    target_compile_definitions(bloom_runtime_gpu_affine_blend_executor_fault_test
        PRIVATE BLOOM_GPU_SCENE_EXECUTOR_TEST_FAULT_INJECTION)
    target_include_directories(
        bloom_runtime_gpu_affine_blend_executor_fault_test
        PRIVATE
            "${PROJECT_SOURCE_DIR}/src/render/tests"
            "${PROJECT_SOURCE_DIR}/src/runtime"
    )
    target_link_libraries(
        bloom_runtime_gpu_affine_blend_executor_fault_test
        PRIVATE bloom_render_gpu_scene_executor_fault bloom_runtime_evaluator bloom_render
    )
    bloom_enable_warnings(bloom_runtime_gpu_affine_blend_executor_fault_test)
    bloom_enable_strict_floating_point(bloom_runtime_gpu_affine_blend_executor_fault_test)
    bloom_add_test(
        NAME bloom.runtime.gpu_affine_blend_executor_fault
        COMMAND bloom_runtime_gpu_affine_blend_executor_fault_test
        LABELS runtime unit gpu render
        TIMEOUT 300
    )
endif()
# Native vertical for the blend-mode slice through the REAL production builder: a real compiled plan
# -> CpuGpuSceneBuilder -> GpuSceneExecutor -> readback vs a live CpuCompositionEvaluator frame. The
# proof-only fixture seam is NOT used, so this proves the builder's own all-8 blend emission.
add_executable(bloom_runtime_gpu_scene_executor_blend_integration_test
    gpu_scene_executor_blend_integration_native_tests.cpp
)
target_include_directories(
    bloom_runtime_gpu_scene_executor_blend_integration_test
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src/render/tests"
        "${PROJECT_SOURCE_DIR}/src/runtime"
)
target_link_libraries(
    bloom_runtime_gpu_scene_executor_blend_integration_test
    PRIVATE bloom_runtime_evaluator bloom_render
)
bloom_enable_warnings(bloom_runtime_gpu_scene_executor_blend_integration_test)
bloom_enable_strict_floating_point(bloom_runtime_gpu_scene_executor_blend_integration_test)
bloom_add_test(
    NAME bloom.runtime.gpu_scene_executor_blend_integration
    COMMAND bloom_runtime_gpu_scene_executor_blend_integration_test
    LABELS runtime unit gpu render
    TIMEOUT 300
)
# Runtime OCIO command/preparation service: reusable wrapper generation, canonical command
# identity, prepared-command validation, and the bounded preparation cache. CPU-only; the
# compile-dependent cases need the pinned glslangValidator/spirv-val and an OCIO config and print
# an explicit note otherwise. The identity/wrapper cases always run.
find_program(bloom_runtime_ocio_glslang_validator
    NAMES glslangValidator
    HINTS "${BLOOM_DEPENDENCY_PREFIX}/bin"
    NO_DEFAULT_PATH)
find_program(bloom_runtime_ocio_spirv_val
    NAMES spirv-val
    HINTS "${BLOOM_DEPENDENCY_PREFIX}/bin"
    NO_DEFAULT_PATH)
add_executable(bloom_runtime_gpu_ocio_command_test gpu_ocio_command_tests.cpp)
target_link_libraries(bloom_runtime_gpu_ocio_command_test PRIVATE bloom_runtime_evaluator)
if(bloom_runtime_ocio_glslang_validator AND bloom_runtime_ocio_spirv_val)
    get_filename_component(bloom_runtime_ocio_tools_dir
        "${bloom_runtime_ocio_glslang_validator}" DIRECTORY)
    target_compile_definitions(bloom_runtime_gpu_ocio_command_test PRIVATE
        "BLOOM_GPUSHADER_TOOLS_DIR=\"${bloom_runtime_ocio_tools_dir}\"")
endif()
bloom_enable_warnings(bloom_runtime_gpu_ocio_command_test)
bloom_enable_strict_floating_point(bloom_runtime_gpu_ocio_command_test)
bloom_add_test(
    NAME bloom.runtime.gpu_ocio_command
    COMMAND bloom_runtime_gpu_ocio_command_test
    LABELS runtime unit gpu color
    TIMEOUT 300
)

# Bounded shared runtime OCIO context resolver (bloom/runtime/gpu_ocio_context.hpp). The focused
# vectors use the REAL tools staged beside this test executable by bloom_package_gpu_shader_tools
# (the same packaging the desktop/CLI/MCP targets use) through the same BLOOM_GPU_TOOLS_* compile
# definitions; there are no fake tool paths. A build without qualified tools still runs the
# typed-refusal (CPU-fallback) vectors.
add_executable(bloom_runtime_gpu_ocio_context_test gpu_ocio_context_tests.cpp)
target_link_libraries(bloom_runtime_gpu_ocio_context_test
    PRIVATE bloom_runtime_evaluator bloom_color_gpu_shader)
target_compile_definitions(bloom_runtime_gpu_ocio_context_test PRIVATE
    "BLOOM_GPU_OCIO_CONTEXT_TEST_EXECUTABLE=\"$<TARGET_FILE:bloom_runtime_gpu_ocio_context_test>\"")
bloom_enable_warnings(bloom_runtime_gpu_ocio_context_test)
include(BloomGpuShaderTools)
bloom_package_gpu_shader_tools(bloom_runtime_gpu_ocio_context_test)
bloom_add_test(
    NAME bloom.runtime.gpu_ocio_context
    COMMAND bloom_runtime_gpu_ocio_context_test
    LABELS runtime unit gpu color
    TIMEOUT 300
)

# CPU-side image-effect scene preparation: identity/bypass/look aliasing, the missing-LUT CPU
# refusal passthrough, the fail-closed non-identity CST, and the fail-closed real file transform.
# With the pinned tools it also prepares a genuine GPU OCIO ProcessEffect command and materializes a
# text leaf input as an accepted coverage command. No device and no loader are required.
add_executable(bloom_runtime_gpu_scene_effect_preparation_test
    gpu_scene_effect_preparation_tests.cpp)
target_include_directories(bloom_runtime_gpu_scene_effect_preparation_test
    PRIVATE "${PROJECT_SOURCE_DIR}/src/runtime")
target_link_libraries(bloom_runtime_gpu_scene_effect_preparation_test
    PRIVATE bloom_runtime_evaluator bloom_render)
if(bloom_runtime_ocio_glslang_validator AND bloom_runtime_ocio_spirv_val)
    target_compile_definitions(bloom_runtime_gpu_scene_effect_preparation_test PRIVATE
        "BLOOM_GPUSHADER_TOOLS_DIR=\"${bloom_runtime_ocio_tools_dir}\"")
endif()
bloom_enable_warnings(bloom_runtime_gpu_scene_effect_preparation_test)
bloom_enable_strict_floating_point(bloom_runtime_gpu_scene_effect_preparation_test)
bloom_add_test(
    NAME bloom.runtime.gpu_scene_effect_preparation
    COMMAND bloom_runtime_gpu_scene_effect_preparation_test
    LABELS runtime unit gpu color
    TIMEOUT 300
)
set_tests_properties(bloom.runtime.gpu_scene_effect_preparation PROPERTIES SKIP_RETURN_CODE 77)

# Native acceptance for the runtime OCIO executor: real extracted/compiled OCIO CST + display +
# view-adjust programs dispatched through the owner-thread executor and compared against the actual
# unchanged CPU OCIO oracle, with warm zero-compile/zero-upload, invalidation, cancellation, and an
# injected upload fault. Requires the Vulkan backend (the fault seam lives in the native closure) and
# the pinned tools; skips cleanly otherwise.
if(BLOOM_RUNTIME_RENDER_VULKAN_AVAILABLE)
    add_executable(bloom_runtime_gpu_ocio_program_executor_test
        gpu_ocio_program_executor_native_tests.cpp)
    target_include_directories(bloom_runtime_gpu_ocio_program_executor_test
        PRIVATE "${PROJECT_SOURCE_DIR}/src/render")
    target_link_libraries(bloom_runtime_gpu_ocio_program_executor_test
        PRIVATE bloom_runtime_evaluator bloom_render bloom_render_ocio_test_faults)
    if(bloom_runtime_ocio_glslang_validator AND bloom_runtime_ocio_spirv_val)
        target_compile_definitions(bloom_runtime_gpu_ocio_program_executor_test PRIVATE
            "BLOOM_GPUSHADER_TOOLS_DIR=\"${bloom_runtime_ocio_tools_dir}\"")
    endif()
    bloom_enable_warnings(bloom_runtime_gpu_ocio_program_executor_test)
    bloom_enable_strict_floating_point(bloom_runtime_gpu_ocio_program_executor_test)
    bloom_add_test(
        NAME bloom.runtime.gpu_ocio_program_executor
        COMMAND bloom_runtime_gpu_ocio_program_executor_test
        LABELS runtime unit gpu color
        TIMEOUT 300
    )
    set_tests_properties(bloom.runtime.gpu_ocio_program_executor
        PROPERTIES SKIP_RETURN_CODE 77)
endif()

# Producer/executor isolation of the nonlinear CST precision gate: the neutral config's
# process -> sRGB-texture GammaOp CST run through the real GpuOcioProgramPreparer +
# GpuOcioProgramExecutor and compared against the unchanged CpuColorSpaceProcessor at 2e-6 with
# byte-exact alpha, over negative/HDR/translucent/alpha-zero/threshold endpoints and a deterministic
# random grid. Requires the Vulkan backend and the pinned tools.
if(BLOOM_RUNTIME_RENDER_VULKAN_AVAILABLE)
    add_executable(bloom_runtime_gpu_ocio_precision_test
        gpu_ocio_precision_native_tests.cpp)
    target_link_libraries(bloom_runtime_gpu_ocio_precision_test
        PRIVATE bloom_runtime_evaluator bloom_render)
    if(bloom_runtime_ocio_glslang_validator AND bloom_runtime_ocio_spirv_val)
        target_compile_definitions(bloom_runtime_gpu_ocio_precision_test PRIVATE
            "BLOOM_GPUSHADER_TOOLS_DIR=\"${bloom_runtime_ocio_tools_dir}\"")
    endif()
    bloom_enable_warnings(bloom_runtime_gpu_ocio_precision_test)
    bloom_enable_strict_floating_point(bloom_runtime_gpu_ocio_precision_test)
    bloom_add_test(
        NAME bloom.runtime.gpu_ocio_precision
        COMMAND bloom_runtime_gpu_ocio_precision_test
        LABELS runtime unit gpu color
        TIMEOUT 300
    )
    set_tests_properties(bloom.runtime.gpu_ocio_precision
        PROPERTIES SKIP_RETURN_CODE 77)
endif()

# Production GpuSceneExecutor driving an OCIO ProcessEffect scene command end to end: solid +
# upload -> OCIO CST effect -> merge -> composition output, compared against the unchanged CPU OCIO
# oracle plus the CPU source-over primitive, with warm zero-dispatch, dirty-effect-only invalidation,
# and an actual-retained-byte budget refusal. Requires the Vulkan backend and the pinned tools.
if(BLOOM_RUNTIME_RENDER_VULKAN_AVAILABLE)
    add_executable(bloom_runtime_gpu_ocio_scene_executor_test
        gpu_ocio_scene_executor_native_tests.cpp)
    target_link_libraries(bloom_runtime_gpu_ocio_scene_executor_test
        PRIVATE bloom_runtime_evaluator bloom_render)
    if(bloom_runtime_ocio_glslang_validator AND bloom_runtime_ocio_spirv_val)
        target_compile_definitions(bloom_runtime_gpu_ocio_scene_executor_test PRIVATE
            "BLOOM_GPUSHADER_TOOLS_DIR=\"${bloom_runtime_ocio_tools_dir}\"")
    endif()
    bloom_enable_warnings(bloom_runtime_gpu_ocio_scene_executor_test)
    bloom_enable_strict_floating_point(bloom_runtime_gpu_ocio_scene_executor_test)
    bloom_add_test(
        NAME bloom.runtime.gpu_ocio_scene_executor
        COMMAND bloom_runtime_gpu_ocio_scene_executor_test
        LABELS runtime unit gpu color
        TIMEOUT 300
    )
    set_tests_properties(bloom.runtime.gpu_ocio_scene_executor
        PROPERTIES SKIP_RETURN_CODE 77)
endif()

# General GPU display arm: real OCIO display programs for the default and a non-default display/view
# pair dispatched through runtime::GpuOcioDisplayArm and compared to the unchanged CPU OCIO display
# oracle (<=1 RGB code, alpha exact), with cold/warm program reuse, no full-frame readback, a typed
# non-neutral ViewAdjust refusal, and a genuine program/descriptor qualification. Requires the Vulkan
# backend and the pinned tools; skips cleanly otherwise.
if(BLOOM_RUNTIME_RENDER_VULKAN_AVAILABLE)
    add_executable(bloom_runtime_gpu_ocio_display_arm_test
        gpu_ocio_display_arm_native_tests.cpp)
    target_link_libraries(bloom_runtime_gpu_ocio_display_arm_test
        PRIVATE bloom_runtime_evaluator bloom_render)
    if(bloom_runtime_ocio_glslang_validator AND bloom_runtime_ocio_spirv_val)
        target_compile_definitions(bloom_runtime_gpu_ocio_display_arm_test PRIVATE
            "BLOOM_GPUSHADER_TOOLS_DIR=\"${bloom_runtime_ocio_tools_dir}\"")
    endif()
    bloom_enable_warnings(bloom_runtime_gpu_ocio_display_arm_test)
    bloom_enable_strict_floating_point(bloom_runtime_gpu_ocio_display_arm_test)
    bloom_add_test(
        NAME bloom.runtime.gpu_ocio_display_arm
        COMMAND bloom_runtime_gpu_ocio_display_arm_test
        LABELS runtime unit gpu color
        TIMEOUT 300
    )
    set_tests_properties(bloom.runtime.gpu_ocio_display_arm
        PROPERTIES SKIP_RETURN_CODE 77)
endif()

# General-display SERVICE acceptance: the production GpuPreviewDisplayService is driven with a real
# PreviewGpuSceneStageFunction whose stage carries an off-UI-prepared general GpuDisplayProgram. It
# proves the ordinary default Neutral request takes the prepared general arm ABOVE the retired 4K
# ceiling, that non-default display + adjustments produce real resident GpuResident frames with a
# valid lease and zero full-frame readback, and that a warm/display-only change retains the process
# output in the scene content cache while changing only the exact DisplayRgba8 command identity. It
# needs a presentable (Wayland) resident route, the Vulkan backend, and the pinned tools; without a
# device it prints an explicit skip (exit 77) and --require-device fails closed.
if(BLOOM_RUNTIME_RENDER_VULKAN_AVAILABLE)
    add_executable(bloom_runtime_gpu_preview_display_service_general_test
        gpu_preview_display_service_general_tests.cpp
        gpu_preview_display_service_general_fixture.ipp)
    target_include_directories(bloom_runtime_gpu_preview_display_service_general_test
        PRIVATE "${PROJECT_SOURCE_DIR}/src/runtime" ${OpenEXR_INCLUDE_DIRS} ${Imath_INCLUDE_DIRS})
    target_link_libraries(bloom_runtime_gpu_preview_display_service_general_test
        PRIVATE bloom_runtime_evaluator bloom_render OpenEXR::OpenEXR)
    # The shared OCIO context resolver consumes the REAL tools staged beside this test executable by
    # bloom_package_gpu_shader_tools (the same packaging the app uses); the test builds its request
    # from the executable-relative BLOOM_GPU_TOOLS_* definitions, never a manually injected path.
    target_compile_definitions(bloom_runtime_gpu_preview_display_service_general_test PRIVATE
        "BLOOM_GPU_GENERAL_TEST_EXECUTABLE=\"$<TARGET_FILE:bloom_runtime_gpu_preview_display_service_general_test>\"")
    include(BloomGpuShaderTools)
    bloom_package_gpu_shader_tools(bloom_runtime_gpu_preview_display_service_general_test)
    bloom_enable_warnings(bloom_runtime_gpu_preview_display_service_general_test)
    bloom_enable_strict_floating_point(bloom_runtime_gpu_preview_display_service_general_test)
    bloom_add_test(
        NAME bloom.runtime.gpu_preview_display_service_general
        COMMAND bloom_runtime_gpu_preview_display_service_general_test
        LABELS runtime integration gpu color resident
        TIMEOUT 600
    )
    set_tests_properties(bloom.runtime.gpu_preview_display_service_general
        PROPERTIES SKIP_RETURN_CODE 77)
endif()

# Real native parity for the PRODUCTION builder's image-effect commands: a compiled plan with a
# non-identity CST effect (and, on Linux, a real isolated FileTransform) is prepared by the real
# CpuGpuSceneBuilder, executed by the production GpuSceneExecutor, and compared against the
# unchanged CpuCompositionEvaluator oracle under the strict 2e-6 abs-or-rel gate with exact alpha and
# the CPU process descriptor. Cold/dispatch, warm zero-dispatch, and a changed-upstream reuse are
# asserted. Requires the Vulkan backend and the pinned tools; skips cleanly otherwise.
if(BLOOM_RUNTIME_RENDER_VULKAN_AVAILABLE)
    add_executable(bloom_runtime_gpu_scene_effect_native_test
        gpu_scene_effect_native_tests.cpp)
    target_include_directories(bloom_runtime_gpu_scene_effect_native_test
        PRIVATE "${PROJECT_SOURCE_DIR}/src/render/tests" "${PROJECT_SOURCE_DIR}/src/runtime")
    target_link_libraries(bloom_runtime_gpu_scene_effect_native_test
        PRIVATE bloom_runtime_evaluator bloom_render)
    if(bloom_runtime_ocio_glslang_validator AND bloom_runtime_ocio_spirv_val)
        target_compile_definitions(bloom_runtime_gpu_scene_effect_native_test PRIVATE
            "BLOOM_GPUSHADER_TOOLS_DIR=\"${bloom_runtime_ocio_tools_dir}\"")
    endif()
    bloom_enable_warnings(bloom_runtime_gpu_scene_effect_native_test)
    bloom_enable_strict_floating_point(bloom_runtime_gpu_scene_effect_native_test)
    bloom_add_test(
        NAME bloom.runtime.gpu_scene_effect_native
        COMMAND bloom_runtime_gpu_scene_effect_native_test
        LABELS runtime native gpu color
        TIMEOUT 300
    )
    set_tests_properties(bloom.runtime.gpu_scene_effect_native
        PROPERTIES SKIP_RETURN_CODE 77)
endif()

# Real native acceptance for arbitrary resolved scene-linear working spaces (ACEScg): a mixed
# solid + text + media + CST + non-Normal blend composition is prepared by the production builder
# and executed, then compared against the unchanged CPU evaluator at the strict 2e-6 gate with exact
# alpha and the CPU process descriptor. Changed working space and changed solid colour identity
# invalidation are asserted. Requires the Vulkan backend and the pinned tools.
if(BLOOM_RUNTIME_RENDER_VULKAN_AVAILABLE)
    add_executable(bloom_runtime_gpu_scene_effect_working_space_native_test
        gpu_scene_effect_working_space_native_tests.cpp)
    target_include_directories(bloom_runtime_gpu_scene_effect_working_space_native_test
        PRIVATE "${PROJECT_SOURCE_DIR}/src/runtime")
    target_link_libraries(bloom_runtime_gpu_scene_effect_working_space_native_test
        PRIVATE bloom_runtime_evaluator bloom_render OpenEXR::OpenEXR)
    target_include_directories(bloom_runtime_gpu_scene_effect_working_space_native_test
        PRIVATE ${OpenEXR_INCLUDE_DIRS} ${Imath_INCLUDE_DIRS})
    if(bloom_runtime_ocio_glslang_validator AND bloom_runtime_ocio_spirv_val)
        target_compile_definitions(bloom_runtime_gpu_scene_effect_working_space_native_test PRIVATE
            "BLOOM_GPUSHADER_TOOLS_DIR=\"${bloom_runtime_ocio_tools_dir}\"")
    endif()
    bloom_enable_warnings(bloom_runtime_gpu_scene_effect_working_space_native_test)
    bloom_enable_strict_floating_point(bloom_runtime_gpu_scene_effect_working_space_native_test)
    bloom_add_test(
        NAME bloom.runtime.gpu_scene_effect_working_space_native
        COMMAND bloom_runtime_gpu_scene_effect_working_space_native_test
        LABELS runtime native gpu color
        TIMEOUT 300
    )
    set_tests_properties(bloom.runtime.gpu_scene_effect_working_space_native
        PROPERTIES SKIP_RETURN_CODE 77)
endif()

# Production export-builder injection: the real GpuProcessFrameEvaluator drives the production
# CpuGpuSceneBuilder with the resolved shared GpuSceneOcioContext and configured media context on a
# mixed media+CST+text graph, proving ONE combined readback with two payloads (display arm) and one
# payload (identity arm). The context is qualified through the real GpuOcioContextResolver from THIS
# target's own packaging, never a manual preparer or an ambient path.
if(BLOOM_RUNTIME_RENDER_VULKAN_AVAILABLE)
    add_executable(bloom_runtime_gpu_process_frame_ocio_native_test
        gpu_process_frame_ocio_native_tests.cpp)
    target_include_directories(bloom_runtime_gpu_process_frame_ocio_native_test
        PRIVATE "${PROJECT_SOURCE_DIR}/src/runtime")
    target_link_libraries(bloom_runtime_gpu_process_frame_ocio_native_test
        PRIVATE bloom_runtime_evaluator bloom_render bloom_color_gpu_shader OpenEXR::OpenEXR)
    target_include_directories(bloom_runtime_gpu_process_frame_ocio_native_test
        PRIVATE ${OpenEXR_INCLUDE_DIRS} ${Imath_INCLUDE_DIRS})
    target_compile_definitions(bloom_runtime_gpu_process_frame_ocio_native_test PRIVATE
        "BLOOM_GPU_OCIO_CONTEXT_TEST_EXECUTABLE=\"$<TARGET_FILE:bloom_runtime_gpu_process_frame_ocio_native_test>\"")
    bloom_enable_warnings(bloom_runtime_gpu_process_frame_ocio_native_test)
    bloom_enable_strict_floating_point(bloom_runtime_gpu_process_frame_ocio_native_test)
    include(BloomGpuShaderTools)
    bloom_package_gpu_shader_tools(bloom_runtime_gpu_process_frame_ocio_native_test)
    bloom_add_test(
        NAME bloom.runtime.gpu_process_frame_ocio_native
        COMMAND bloom_runtime_gpu_process_frame_ocio_native_test
        LABELS runtime native gpu color export
        TIMEOUT 300
    )
    set_tests_properties(bloom.runtime.gpu_process_frame_ocio_native
        PROPERTIES SKIP_RETURN_CODE 77)
endif()

# CompiledCompositionSource splice over a real OCIO ProcessEffect command. The production builder
# does not yet emit CompiledImageEffect, so the fixture builds an immutable child PreparedGpuScene
# with a genuine GpuOcioEffectCommand from the real preparer, splices it through the production
# nested helper (remapped input dependency, unchanged program identity, charged resident bytes), and
# then runs the spliced parent/child scene natively against the unchanged CPU OCIO oracle. Readback
# happens only for the test oracle. Requires the Vulkan backend and the pinned tools.
if(BLOOM_RUNTIME_RENDER_VULKAN_AVAILABLE)
    add_executable(bloom_runtime_gpu_nested_ocio_test gpu_nested_ocio_tests.cpp)
    target_include_directories(bloom_runtime_gpu_nested_ocio_test
        PRIVATE "${PROJECT_SOURCE_DIR}/src/runtime")
    target_link_libraries(bloom_runtime_gpu_nested_ocio_test
        PRIVATE bloom_runtime_evaluator bloom_render)
    if(bloom_runtime_ocio_glslang_validator AND bloom_runtime_ocio_spirv_val)
        target_compile_definitions(bloom_runtime_gpu_nested_ocio_test PRIVATE
            "BLOOM_GPUSHADER_TOOLS_DIR=\"${bloom_runtime_ocio_tools_dir}\"")
    endif()
    bloom_enable_warnings(bloom_runtime_gpu_nested_ocio_test)
    bloom_enable_strict_floating_point(bloom_runtime_gpu_nested_ocio_test)
    bloom_add_test(
        NAME bloom.runtime.gpu_nested_ocio
        COMMAND bloom_runtime_gpu_nested_ocio_test
        LABELS runtime unit gpu color
        TIMEOUT 300
    )
    set_tests_properties(bloom.runtime.gpu_nested_ocio
        PROPERTIES SKIP_RETURN_CODE 77)
endif()

# Strict native proof for the production wrapper's FileTransform arm: a real .cube LUT read through
# color::readLutFile, extracted through the isolated bloom-color-worker, wrapped with the shared
# precise-sampling adapter, and executed on the device with the unchanged isolated CPU FileTransform
# processor as the 2e-6 oracle. Requires the Vulkan backend and the pinned tools.
if(BLOOM_RUNTIME_RENDER_VULKAN_AVAILABLE)
    add_executable(bloom_runtime_gpu_ocio_file_transform_test
        gpu_ocio_file_transform_native_tests.cpp)
    target_link_libraries(bloom_runtime_gpu_ocio_file_transform_test
        PRIVATE bloom_runtime_evaluator bloom_render)
    if(bloom_runtime_ocio_glslang_validator AND bloom_runtime_ocio_spirv_val)
        target_compile_definitions(bloom_runtime_gpu_ocio_file_transform_test PRIVATE
            "BLOOM_GPUSHADER_TOOLS_DIR=\"${bloom_runtime_ocio_tools_dir}\"")
    endif()
    bloom_enable_warnings(bloom_runtime_gpu_ocio_file_transform_test)
    bloom_enable_strict_floating_point(bloom_runtime_gpu_ocio_file_transform_test)
    bloom_add_test(
        NAME bloom.runtime.gpu_ocio_file_transform
        COMMAND bloom_runtime_gpu_ocio_file_transform_test
        LABELS runtime unit gpu color
        TIMEOUT 300
    )
    set_tests_properties(bloom.runtime.gpu_ocio_file_transform
        PROPERTIES SKIP_RETURN_CODE 77)
endif()

# Native acceptance for the exact runtime::ViewAdjust post-display exposure/gamma in the production
# OCIO DisplayPacking wrapper: real display programs for >=2 display/view pairs with non-neutral
# boundary adjustments, executed on the device and compared against the unchanged CPU display
# processor + ViewAdjust::fromEncoded oracle (<=1 RGBA8 code, exact alpha), with warm program reuse
# and process-command isolation. Requires the Vulkan backend and the pinned tools.
if(BLOOM_RUNTIME_RENDER_VULKAN_AVAILABLE)
    add_executable(bloom_runtime_gpu_ocio_view_adjust_test
        gpu_ocio_view_adjust_native_tests.cpp)
    target_link_libraries(bloom_runtime_gpu_ocio_view_adjust_test
        PRIVATE bloom_runtime_evaluator bloom_render)
    if(bloom_runtime_ocio_glslang_validator AND bloom_runtime_ocio_spirv_val)
        target_compile_definitions(bloom_runtime_gpu_ocio_view_adjust_test PRIVATE
            "BLOOM_GPUSHADER_TOOLS_DIR=\"${bloom_runtime_ocio_tools_dir}\"")
    endif()
    bloom_enable_warnings(bloom_runtime_gpu_ocio_view_adjust_test)
    bloom_enable_strict_floating_point(bloom_runtime_gpu_ocio_view_adjust_test)
    bloom_add_test(
        NAME bloom.runtime.gpu_ocio_view_adjust
        COMMAND bloom_runtime_gpu_ocio_view_adjust_test
        LABELS runtime unit gpu color
        TIMEOUT 300
    )
    set_tests_properties(bloom.runtime.gpu_ocio_view_adjust
        PROPERTIES SKIP_RETURN_CODE 77)
endif()

# CPU-side ROI parity for prepared GPU scenes. A genuine ROI request is driven through the REAL
# production CpuGpuSceneBuilder and every process pixel is compared against the unchanged CPU
# evaluator by replaying the prepared commands with the existing CPU primitives. It also proves the
# unchanged intermediate semantic keys stay stable across an ROI edit while only the clipped output
# command is cold. Pure CPU: no loader, no device.
add_executable(bloom_runtime_gpu_roi_scene_preparation_test
    gpu_roi_scene_preparation_tests.cpp)
target_include_directories(bloom_runtime_gpu_roi_scene_preparation_test
    PRIVATE "${PROJECT_SOURCE_DIR}/src/runtime")
target_link_libraries(bloom_runtime_gpu_roi_scene_preparation_test
    PRIVATE bloom_runtime_evaluator bloom_render)
bloom_enable_warnings(bloom_runtime_gpu_roi_scene_preparation_test)
bloom_enable_strict_floating_point(bloom_runtime_gpu_roi_scene_preparation_test)
bloom_add_test(
    NAME bloom.runtime.gpu_roi_scene_preparation
    COMMAND bloom_runtime_gpu_roi_scene_preparation_test
    LABELS runtime unit gpu render
    TIMEOUT 300
)

# Real native ROI parity: the production builder -> production executor -> readback path on a real
# device, checked against the unchanged CPU evaluator for solid/vector/affine/nested scenes with
# nonzero-origin and transparent ROIs. Cold must dispatch, the warm rerun must hit the cache with
# zero dispatch, and an ROI edit must re-dispatch only the clipped output command. Without a device
# it skips honestly (exit 77); --require-device makes an unavailable device a failure.
add_executable(bloom_runtime_gpu_roi_scene_native_test gpu_roi_scene_native_tests.cpp
    gpu_roi_scene_fixture_support.hpp)
target_include_directories(bloom_runtime_gpu_roi_scene_native_test
    PRIVATE "${PROJECT_SOURCE_DIR}/src/runtime")
target_link_libraries(bloom_runtime_gpu_roi_scene_native_test
    PRIVATE bloom_runtime_evaluator bloom_render)
bloom_enable_warnings(bloom_runtime_gpu_roi_scene_native_test)
bloom_enable_strict_floating_point(bloom_runtime_gpu_roi_scene_native_test)
bloom_add_test(
    NAME bloom.runtime.gpu_roi_scene_native
    COMMAND bloom_runtime_gpu_roi_scene_native_test
    LABELS runtime native gpu render
    TIMEOUT 600
)
set_tests_properties(bloom.runtime.gpu_roi_scene_native PROPERTIES SKIP_RETURN_CODE 77)

# Real native media + colour-transform ROI: a real AP0 EXR through the production media pipeline
# and a real input->working OCIO ProcessEffect, clipped to a requested ROI and compared against the
# unchanged CPU evaluator. The raw upload keeps the full source dimensions, so an unchanged source
# is reused across an ROI edit and only the clipped output re-dispatches. Needs the pinned shader
# tools; without them it skips, and without a device it skips unless --require-device.
add_executable(bloom_runtime_gpu_roi_media_color_native_test
    gpu_roi_media_color_native_tests.cpp)
target_include_directories(bloom_runtime_gpu_roi_media_color_native_test
    PRIVATE ${OpenEXR_INCLUDE_DIRS} ${Imath_INCLUDE_DIRS}
            "${PROJECT_SOURCE_DIR}/src/runtime")
target_link_libraries(bloom_runtime_gpu_roi_media_color_native_test
    PRIVATE bloom_runtime_evaluator bloom_render bloom_media_image bloom_media_video
            bloom_media_cache bloom_color bloom_color_ocio OpenEXR::OpenEXR)
find_program(bloom_roi_media_color_glslang_validator
    NAMES glslangValidator
    HINTS "${BLOOM_DEPENDENCY_PREFIX}/bin"
    NO_DEFAULT_PATH)
find_program(bloom_roi_media_color_spirv_val
    NAMES spirv-val
    HINTS "${BLOOM_DEPENDENCY_PREFIX}/bin"
    NO_DEFAULT_PATH)
if(bloom_roi_media_color_glslang_validator AND bloom_roi_media_color_spirv_val)
    get_filename_component(bloom_roi_media_color_tools_dir
        "${bloom_roi_media_color_glslang_validator}" DIRECTORY)
    target_compile_definitions(bloom_runtime_gpu_roi_media_color_native_test PRIVATE
        "BLOOM_GPUSHADER_TOOLS_DIR=\"${bloom_roi_media_color_tools_dir}\"")
endif()
bloom_enable_warnings(bloom_runtime_gpu_roi_media_color_native_test)
bloom_enable_strict_floating_point(bloom_runtime_gpu_roi_media_color_native_test)
target_compile_options(bloom_runtime_gpu_roi_media_color_native_test PRIVATE -Wno-unused-function)
bloom_add_test(
    NAME bloom.runtime.gpu_roi_media_color_native
    COMMAND bloom_runtime_gpu_roi_media_color_native_test
    LABELS runtime native gpu render media color
    TIMEOUT 600
)
set_tests_properties(bloom.runtime.gpu_roi_media_color_native PROPERTIES SKIP_RETURN_CODE 77)

# Runtime GPU output-colour stage: one prepared OCIO command dispatched, then the exact process
# RGBA32F payload plus the encoded output read back in ONE combined submission and compared against
# the unchanged CPU OCIO oracle (2e-6 / one RGBA8 code, exact alpha and process bits). The identity
# arm proves a single process-only payload; cancel and budget ceilings prove the retirement model.
if(BLOOM_RUNTIME_RENDER_VULKAN_AVAILABLE)
    add_executable(bloom_runtime_gpu_output_color_test gpu_output_color_tests.cpp)
    target_link_libraries(bloom_runtime_gpu_output_color_test
        PRIVATE bloom_runtime_evaluator bloom_render)
    if(bloom_runtime_ocio_glslang_validator AND bloom_runtime_ocio_spirv_val)
        target_compile_definitions(bloom_runtime_gpu_output_color_test PRIVATE
            "BLOOM_GPUSHADER_TOOLS_DIR=\"${bloom_runtime_ocio_tools_dir}\"")
    endif()
    bloom_enable_warnings(bloom_runtime_gpu_output_color_test)
    bloom_enable_strict_floating_point(bloom_runtime_gpu_output_color_test)
    bloom_add_test(
        NAME bloom.runtime.gpu_output_color
        COMMAND bloom_runtime_gpu_output_color_test
        LABELS runtime unit gpu color export
        TIMEOUT 300
    )
    set_tests_properties(bloom.runtime.gpu_output_color
        PROPERTIES SKIP_RETURN_CODE 77)
endif()
