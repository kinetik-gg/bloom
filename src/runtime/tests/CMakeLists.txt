add_executable(bloom_runtime_task_test task_scheduler_tests.cpp)
add_executable(bloom_runtime_regression_test task_scheduler_regression_tests.cpp)

target_link_libraries(bloom_runtime_task_test PRIVATE bloom_runtime)
target_link_libraries(bloom_runtime_regression_test PRIVATE bloom_runtime)
target_include_directories(bloom_runtime_regression_test PRIVATE ..)
bloom_enable_warnings(bloom_runtime_task_test)
bloom_enable_warnings(bloom_runtime_regression_test)

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
