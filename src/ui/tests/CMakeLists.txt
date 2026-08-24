add_executable(bloom_composition_projection_test
    composition_projection_test.cpp
)
add_executable(bloom_composition_preview_controller_test
    composition_preview_controller_tests.cpp
)
add_executable(bloom_composition_session_animation_test
    composition_session_animation_tests.cpp
)
add_executable(bloom_application_shutdown_test
    application_shutdown_tests.cpp
)
add_executable(bloom_task_monitor_test
    task_monitor_test.cpp
)
add_executable(bloom_viewer_editor_test
    viewer_editor_tests.cpp
)

target_link_libraries(bloom_composition_projection_test PRIVATE bloom_ui)
target_link_libraries(bloom_composition_preview_controller_test PRIVATE bloom_ui)
target_link_libraries(bloom_composition_session_animation_test PRIVATE bloom_ui)
target_link_libraries(bloom_application_shutdown_test PRIVATE bloom_ui)
target_link_libraries(bloom_task_monitor_test PRIVATE bloom_ui)
target_link_libraries(bloom_viewer_editor_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_composition_projection_test)
bloom_enable_warnings(bloom_composition_preview_controller_test)
bloom_enable_warnings(bloom_composition_session_animation_test)
bloom_enable_warnings(bloom_application_shutdown_test)
bloom_enable_warnings(bloom_task_monitor_test)
bloom_enable_warnings(bloom_viewer_editor_test)

include(BloomTesting)

bloom_add_test(
    NAME bloom.ui.composition_projection
    COMMAND bloom_composition_projection_test
    LABELS ui integration
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.composition_preview_controller
    COMMAND bloom_composition_preview_controller_test
    LABELS ui integration
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.composition_session_animation
    COMMAND bloom_composition_session_animation_test
    LABELS ui integration animation
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.application_shutdown
    COMMAND bloom_application_shutdown_test
    LABELS ui integration
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.task_monitor
    COMMAND bloom_task_monitor_test
    LABELS ui integration
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.viewer_editor
    COMMAND bloom_viewer_editor_test
    LABELS ui unit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)
