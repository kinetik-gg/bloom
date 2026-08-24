add_library(bloom_ui STATIC)

target_sources(
    bloom_ui
    PRIVATE
        application_shutdown_coordinator.cpp
        composition_editors.cpp
        composition_preview_controller.cpp
        composition_session.cpp
        editor_area.cpp
        editor_registry.cpp
        jobs_editor.cpp
        main_window.cpp
        node_editor.cpp
        snapshot_compile_preparation.cpp
        task_monitor_model.cpp
        task_ui_bridge.cpp
        workspace_host.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bloom/ui/application_shutdown_coordinator.hpp
            include/bloom/ui/composition_editors.hpp
            include/bloom/ui/composition_preview_controller.hpp
            include/bloom/ui/composition_session.hpp
            include/bloom/ui/editor_area.hpp
            include/bloom/ui/editor_registry.hpp
            include/bloom/ui/jobs_editor.hpp
            include/bloom/ui/main_window.hpp
            include/bloom/ui/node_editor.hpp
            include/bloom/ui/snapshot_compile_preparation.hpp
            include/bloom/ui/task_monitor_model.hpp
            include/bloom/ui/task_ui_bridge.hpp
            include/bloom/ui/workspace_host.hpp
)

target_compile_features(bloom_ui PUBLIC cxx_std_20)
target_link_libraries(bloom_ui PUBLIC bloom_commands bloom_runtime Qt6::Widgets)
target_link_libraries(bloom_ui PRIVATE bloom_runtime_compiler)
bloom_enable_warnings(bloom_ui)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
