# bloom_ui_kit (task U1, issue #117): the Kinetik design system -- tokens, icons, fonts, the
# application theme installer, and the reusable widget kit. A target of its own rather than more
# files inside bloom_ui, mirroring src/color's bloom_color/bloom_color_ocio precedent: the kit owns
# the vendored third-party icon and font assets and the Qt6::Svg dependency, and nothing about a
# panel, a session, or a document may leak into it. bloom_ui links it PUBLIC, so every existing UI
# source keeps consuming it without its own link line.
#
# Both repository_checks.cpp rules are satisfied without a checker edit: the kit lives entirely
# below src/ui, which is exempt from the "Qt types are restricted to src/ui and apps" scan
# (isNonUiSource()) and from the "non-UI source module links directly to Qt" CMake scan
# (appendCmakeArchitectureFindings() skips parts[1] == "ui"); and its public headers sit at
# src/ui/include/bloom/ui/kit/*.hpp, which satisfies appendPublicHeaderPathFinding()'s
# src/<module>/include/bloom/<module> rule because parts[3]/parts[4] are still bloom/ui.
add_library(bloom_ui_kit STATIC)

target_sources(
    bloom_ui_kit
    PRIVATE
        kit/theme.cpp
        kit/tokens.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bloom/ui/kit/theme.hpp
            include/bloom/ui/kit/tokens.hpp
)

target_compile_features(bloom_ui_kit PUBLIC cxx_std_20)
target_link_libraries(bloom_ui_kit PUBLIC Qt6::Widgets)
bloom_enable_warnings(bloom_ui_kit)

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
        playback_controller.cpp
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
            include/bloom/ui/playback_controller.hpp
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
        bloom_ui_kit
        Qt6::Widgets
)
target_link_libraries(bloom_ui PRIVATE bloom_runtime_compiler)
bloom_enable_warnings(bloom_ui)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
