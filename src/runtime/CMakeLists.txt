find_package(Threads REQUIRED)

add_library(bloom_runtime STATIC)

target_sources(
    bloom_runtime
    PRIVATE
        task_primitives.cpp
        task_gpu_executor.cpp
        task_gpu_executor_lease.cpp
        task_scheduler.cpp
        task_scheduler_admission.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bloom/runtime/cancellation.hpp
            include/bloom/runtime/task_gpu_executor.hpp
            include/bloom/runtime/task_scheduler.hpp
            include/bloom/runtime/task_types.hpp
)

target_compile_features(bloom_runtime PUBLIC cxx_std_20)
target_link_libraries(bloom_runtime PUBLIC bloom_core Threads::Threads)
bloom_enable_warnings(bloom_runtime)

add_library(bloom_runtime_compiler STATIC)

target_sources(
    bloom_runtime_compiler
    PRIVATE
        compile_diagnostics.cpp
        compiled_plan.cpp
        node_definition_registry.cpp
        snapshot_compiler.cpp
        snapshot_compiler_lowering.ipp
        snapshot_compiler_support.hpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bloom/runtime/compiled_plan.hpp
            include/bloom/runtime/node_definition_registry.hpp
            include/bloom/runtime/snapshot_compiler.hpp
)

target_compile_features(bloom_runtime_compiler PUBLIC cxx_std_20)
target_link_libraries(bloom_runtime_compiler PUBLIC bloom_runtime bloom_document)
bloom_enable_warnings(bloom_runtime_compiler)

add_library(bloom_runtime_evaluator STATIC)

target_sources(
    bloom_runtime_evaluator
    PRIVATE
        animation_sampling.cpp
        cpu_composition_evaluator.cpp
        cpu_composition_evaluator_support.hpp
        evaluation.cpp
        prepared_preview_frame.cpp
        reference_display_preparation.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bloom/runtime/animation_sampling.hpp
            include/bloom/runtime/cpu_composition_evaluator.hpp
            include/bloom/runtime/evaluation.hpp
            include/bloom/runtime/prepared_preview_frame.hpp
            include/bloom/runtime/reference_display_preparation.hpp
)

target_compile_features(bloom_runtime_evaluator PUBLIC cxx_std_20)
target_link_libraries(bloom_runtime_evaluator PUBLIC bloom_runtime_compiler bloom_render)
bloom_enable_warnings(bloom_runtime_evaluator)
bloom_enable_strict_floating_point(bloom_runtime_evaluator)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
