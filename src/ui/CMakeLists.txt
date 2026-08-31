add_library(bloom_ui STATIC)

target_sources(
    bloom_ui
    PRIVATE
        application_shutdown_coordinator.cpp
        composition_editors.cpp
        composition_preview_controller.cpp
        composition_preview_pipeline.cpp
        composition_session.cpp
        editor_area.cpp
        editor_registry.cpp
        frame_export_controller.cpp
        jobs_editor.cpp
        main_window.cpp
        node_editor.cpp
        project_host.cpp
        qualified_display_processor_bootstrap.cpp
        task_monitor_model.cpp
        task_ui_bridge.cpp
        timeline_frame_math.cpp
        timeline_ruler.cpp
        viewer_editor.cpp
        workspace_host.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bloom/ui/application_shutdown_coordinator.hpp
            include/bloom/ui/composition_editors.hpp
            include/bloom/ui/composition_preview_controller.hpp
            include/bloom/ui/composition_preview_pipeline.hpp
            include/bloom/ui/composition_session.hpp
            include/bloom/ui/editor_area.hpp
            include/bloom/ui/editor_registry.hpp
            include/bloom/ui/frame_export_controller.hpp
            include/bloom/ui/jobs_editor.hpp
            include/bloom/ui/main_window.hpp
            include/bloom/ui/node_editor.hpp
            include/bloom/ui/project_host.hpp
            include/bloom/ui/qualified_display_processor_bootstrap.hpp
            include/bloom/ui/task_monitor_model.hpp
            include/bloom/ui/task_ui_bridge.hpp
            include/bloom/ui/timeline_frame_math.hpp
            include/bloom/ui/timeline_ruler.hpp
            include/bloom/ui/viewer_editor.hpp
            include/bloom/ui/workspace_host.hpp
)

target_compile_features(bloom_ui PUBLIC cxx_std_20)
# bloom_host/bloom_platform/bloom_project (task U1, issue #72): ProjectHost (project_host.hpp) is
# the application's File-menu composition root over bloom::host::ProjectSession -- it directly
# constructs a bloom::platform::StagedArtifactCoordinator and a
# bloom::project::ProjectIoMemoryCoordinator/ProjectIoOperationMemory, and its public signatures
# carry bloom::host types (ProjectSessionStateSnapshot, SessionSaveResult, SessionOpenResult, ...).
# No repository_checks.cpp kAllowedModuleDependencies edit was needed: src/ui is exempt from that
# checker's module-dependency scan entirely (see isNonUiSource()/kKnownModules -- "ui" is never a
# tracked module), matching AGENTS.md's "the UI is the application composition surface".
target_link_libraries(
    bloom_ui
    PUBLIC
        bloom_commands
        bloom_host
        bloom_platform
        bloom_project
        bloom_render
        bloom_runtime
        bloom_runtime_evaluator
        Qt6::Widgets
)
target_link_libraries(bloom_ui PRIVATE bloom_runtime_compiler)
bloom_enable_warnings(bloom_ui)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
