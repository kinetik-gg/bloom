find_package(Threads REQUIRED)

add_library(bloom_runtime STATIC)

target_sources(
    bloom_runtime
    PRIVATE
        row_band_execution.cpp
        memory_budget_ledger.cpp
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
            include/bloom/runtime/memory_budget_ledger.hpp
            include/bloom/runtime/row_band_execution.hpp
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
        compiled_plan_cache.cpp
        composition_time.cpp
        plan_time_dependence.cpp
        curve_compilation.cpp
        snapshot_compiler.cpp
        snapshot_compiler_lowering.ipp
        snapshot_compiler_support.hpp
        snapshot_compiler_value_graph.ipp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bloom/runtime/compiled_plan.hpp
            include/bloom/runtime/compiled_plan_cache.hpp
            include/bloom/runtime/composition_time.hpp
            include/bloom/runtime/compiled_value_graph.hpp
            include/bloom/runtime/operation_index.hpp
            include/bloom/runtime/curve_compilation.hpp
            include/bloom/runtime/node_definition_registry.hpp
            include/bloom/runtime/snapshot_compiler.hpp
)

target_compile_features(bloom_runtime_compiler PUBLIC cxx_std_20)
target_link_libraries(bloom_runtime_compiler PUBLIC bloom_runtime bloom_document bloom_render)
bloom_enable_warnings(bloom_runtime_compiler)

add_library(bloom_runtime_evaluator STATIC)

target_sources(
    bloom_runtime_evaluator
    PRIVATE
        operation_cache.cpp
        video_source.cpp
        video_source.hpp
        image_source.cpp
        image_source.hpp
        input_color_context.cpp
        input_color_context.hpp
        layer_parent_transform.hpp
        operation_key.hpp
        animation_sampling.cpp
        cpu_composition_evaluator.cpp
        cpu_composition_resolution.hpp
        cpu_gpu_scene_preparation.cpp
        gpu_scene_preparation_private.hpp
        gpu_scene_preparation_common.hpp
        gpu_scene_coverage.cpp
        gpu_scene_shape_coverage.cpp
        gpu_scene_coverage.hpp
        gpu_scene_coverage_cache.cpp
        gpu_media_preparation.cpp
        gpu_media_preparation.hpp
        gpu_scene_media_layer.cpp
        gpu_scene_media_layer.hpp
        gpu_prepared_upload_cache.cpp
        image_effect.cpp
        image_effect.hpp
        cpu_composition_evaluator_support.hpp
        evaluation.cpp
        prepared_preview_frame.cpp
        qualified_display_preparation.cpp
        qualified_display_processor_provider.cpp
        reference_display_preparation.cpp
        gpu_neutral_display_qualification.cpp
        gpu_neutral_display_qualification_fixtures.cpp
        gpu_resident_preview_qualification.cpp
        gpu_resident_preview_qualification_ops.cpp
        gpu_resident_preview_qualification_fixtures.cpp
        gpu_resident_preview_qualification_display.cpp
        gpu_resident_preview_qualification_internal.hpp
        gpu_preview_display_product.cpp
        gpu_resident_preview_product.cpp
        gpu_resident_frame_lease.cpp
        gpu_presentation_coordinator.cpp
        gpu_presentation_coordinator_pump.cpp
        gpu_presentation_coordinator_state.hpp
        gpu_presentation_coordinator_private.hpp
        gpu_presentation_client.cpp
        gpu_preview_display_service.cpp
        gpu_preview_display_service_jobs.cpp
        gpu_preview_display_service_private.hpp
        gpu_preview_display_service_loop.hpp
        gpu_preview_display_service_presentation.cpp
        gpu_preview_display_service_presentation_private.hpp
        gpu_preview_display_service_resident.cpp
        gpu_preview_display_service_resident_jobs.cpp
        gpu_preview_display_service_resident_private.hpp
        gpu_scene_cache.cpp
        gpu_scene_executor.cpp
        gpu_scene_executor_planner.cpp
        gpu_scene_executor_execution.cpp
        gpu_scene_executor_private.hpp
        gpu_process_frame.cpp
        view_adjust.cpp
        view_adjust_support.hpp
        value_graph_evaluation.cpp
        value_utility_support.hpp
        value_utility_kernels.cpp
        value_utility_data_blocks.cpp
        value_utility_conversions.cpp
        value_utility_time.cpp
        value_utility_strings.cpp
        value_utility_numeric.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bloom/runtime/operation_cache.hpp
            include/bloom/runtime/animation_sampling.hpp
            include/bloom/runtime/cpu_composition_evaluator.hpp
            include/bloom/runtime/evaluation.hpp
            include/bloom/runtime/gpu_neutral_display_qualification.hpp
            include/bloom/runtime/gpu_resident_preview_qualification.hpp
            include/bloom/runtime/gpu_preview_display_product.hpp
            include/bloom/runtime/gpu_resident_preview_product.hpp
            include/bloom/runtime/gpu_preview_display_service.hpp
            include/bloom/runtime/gpu_presentation_coordinator.hpp
            include/bloom/runtime/gpu_resident_frame_lease.hpp
            include/bloom/runtime/gpu_scene_cache.hpp
            include/bloom/runtime/gpu_scene_coverage_cache.hpp
            include/bloom/runtime/gpu_prepared_upload_cache.hpp
            include/bloom/runtime/gpu_scene_executor.hpp
            include/bloom/runtime/gpu_process_frame.hpp
            include/bloom/runtime/input_color_context.hpp
            include/bloom/runtime/prepared_gpu_scene.hpp
            include/bloom/runtime/prepared_preview_frame.hpp
            include/bloom/runtime/preview_cpu_stage.hpp
            include/bloom/runtime/preview_gpu_scene_stage.hpp
            include/bloom/runtime/qualified_display_preparation.hpp
            include/bloom/runtime/qualified_display_processor_provider.hpp
            include/bloom/runtime/reference_display_preparation.hpp
            include/bloom/runtime/view_adjust.hpp
            include/bloom/runtime/value_graph_evaluation.hpp
            include/bloom/runtime/value_utility_kernels.hpp
)

target_compile_features(bloom_runtime_evaluator PUBLIC cxx_std_20)
# bloom_color_ocio (issue #97, task C3): the qualified display-preparation stage applies the C2
# qualified Bloom Neutral CPU display processor (bloom/color/ocio_cpu_display_frame.hpp,
# ocio_cpu_display_processor.hpp) to a ProcessFrame. runtime->color is the correct dependency
# direction (docs/architecture/color-management.md's staged-graph "CPU Display Processor Boundary"
# has color own the adapter and runtime own the staged application); PUBLIC because
# qualified_display_preparation.hpp and qualified_display_processor_provider.hpp expose
# bloom::color value types (PreparedCpuDisplayProcessorHandle, PreparedDisplayFrame) directly in
# their own public signatures, exactly like the existing PUBLIC bloom_render dependency below.
target_link_libraries(bloom_runtime_evaluator PUBLIC bloom_runtime_compiler bloom_render
                                                     bloom_color_ocio bloom_media_image
                                                     bloom_media_cache bloom_media_video bloom_color_video)
bloom_enable_warnings(bloom_runtime_evaluator)
bloom_enable_strict_floating_point(bloom_runtime_evaluator)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

add_library(bloom_runtime_video_asset STATIC video_asset.cpp)
target_include_directories(bloom_runtime_video_asset PUBLIC include)
target_link_libraries(bloom_runtime_video_asset PUBLIC bloom_document bloom_media_provider)
bloom_enable_warnings(bloom_runtime_video_asset)
target_link_libraries(bloom_runtime_evaluator PUBLIC bloom_runtime_video_asset)
