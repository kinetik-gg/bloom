add_library(bloom_commands STATIC)

target_sources(
    bloom_commands
    PRIVATE
        animation_operations.cpp
        keyframe_easing_operations.cpp
        command_stack.cpp
        operations.cpp
        parameter_source.cpp
        asset_operations.cpp
        data_block_operations.cpp
        export_queue_operations.cpp
        asset_organization_operations.cpp
        node_operations.cpp
        layer_operations.cpp
        layer_time_operations.cpp
        layer_mask_operations.cpp
        composition_settings_operations.cpp
        layer_alignment_operations.cpp
        layer_drop_operations.cpp
        layer_stack_operations.cpp
        layer_timing_gesture.cpp
        node_editor_operations.cpp
        node_layout_arrange.cpp
        parameter_reset_operations.cpp
        project_item_operations.cpp
        video_source_operations.cpp
        node_duplication.cpp
        node_group_operations.cpp
        node_operation_eligibility.cpp
        node_layout_operations.cpp
        render_footprint.cpp
        result.cpp
        transaction.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bloom/commands/animation_operations.hpp
            include/bloom/commands/command_stack.hpp
            include/bloom/commands/operation.hpp
            include/bloom/commands/operations.hpp
            include/bloom/commands/asset_operations.hpp
            include/bloom/commands/data_block_operations.hpp
            include/bloom/commands/export_queue_operations.hpp
            include/bloom/commands/node_operations.hpp
            include/bloom/commands/layer_operations.hpp
            include/bloom/commands/layer_time_operations.hpp
            include/bloom/commands/layer_mask_operations.hpp
            include/bloom/commands/composition_settings_operations.hpp
            include/bloom/commands/layer_alignment_operations.hpp
            include/bloom/commands/layer_drop_operations.hpp
            include/bloom/commands/layer_stack_operations.hpp
            include/bloom/commands/layer_timing_gesture.hpp
            include/bloom/commands/node_editor_operations.hpp
            include/bloom/commands/node_layout_arrange.hpp
            include/bloom/commands/parameter_reset_operations.hpp
            include/bloom/commands/project_item_operations.hpp
            include/bloom/commands/video_source_operations.hpp
            include/bloom/commands/result.hpp
            include/bloom/commands/transaction.hpp
)

target_compile_features(bloom_commands PUBLIC cxx_std_20)
target_link_libraries(bloom_commands PUBLIC bloom_document bloom_media_provider
                      PRIVATE bloom_media_image bloom_media_audio bloom_media_video bloom_color_ocio)
bloom_enable_warnings(bloom_commands)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
