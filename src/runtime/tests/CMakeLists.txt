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
