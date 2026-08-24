add_executable(bloom_runtime_task_test task_scheduler_tests.cpp)
add_executable(bloom_runtime_regression_test task_scheduler_regression_tests.cpp)
add_executable(bloom_runtime_registry_test node_definition_registry_tests.cpp)
add_executable(bloom_runtime_snapshot_compiler_test snapshot_compiler_tests.cpp)

target_link_libraries(bloom_runtime_task_test PRIVATE bloom_runtime)
target_link_libraries(bloom_runtime_regression_test PRIVATE bloom_runtime)
target_link_libraries(bloom_runtime_registry_test PRIVATE bloom_runtime_compiler)
target_link_libraries(bloom_runtime_snapshot_compiler_test PRIVATE bloom_runtime_compiler)
target_include_directories(bloom_runtime_regression_test PRIVATE ..)
target_include_directories(bloom_runtime_snapshot_compiler_test PRIVATE ..)
bloom_enable_warnings(bloom_runtime_task_test)
bloom_enable_warnings(bloom_runtime_regression_test)
bloom_enable_warnings(bloom_runtime_registry_test)
bloom_enable_warnings(bloom_runtime_snapshot_compiler_test)

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
