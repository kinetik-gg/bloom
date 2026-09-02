add_executable(bloom_kit_tokens_test
    kit_tokens_tests.cpp
)
add_executable(bloom_composition_projection_test
    composition_projection_test.cpp
)
add_executable(bloom_composition_preview_controller_test
    composition_preview_controller_tests.cpp
)
add_executable(bloom_composition_session_animation_test
    composition_session_animation_tests.cpp
)
add_executable(bloom_composition_session_rebind_test
    composition_session_rebind_tests.cpp
)
add_executable(bloom_application_shutdown_test
    application_shutdown_tests.cpp
)
add_executable(bloom_project_host_test
    project_host_tests.cpp
)
add_executable(bloom_task_monitor_test
    task_monitor_test.cpp
)
add_executable(bloom_viewer_editor_test
    viewer_editor_tests.cpp
)
add_executable(bloom_timeline_frame_math_test
    timeline_frame_math_tests.cpp
)
add_executable(bloom_timeline_ruler_test
    timeline_ruler_tests.cpp
)
add_executable(bloom_composition_session_position_interaction_test
    composition_session_position_interaction_tests.cpp
)
add_executable(bloom_direct_manipulation_test
    direct_manipulation_tests.cpp
)
add_executable(bloom_frame_export_controller_test
    frame_export_controller_tests.cpp
)

# QTest::keyClick (issue #105's widget test: a real key-event dispatch through Qt's own shortcut
# machinery, not a synthesized focus check) needs Qt6::Test, which no other bloom_ui test target
# links -- scoped to this one executable rather than bloom_ui itself.
if(NOT TARGET Qt6::Test)
    find_package(Qt6 6.8 REQUIRED COMPONENTS Test)
endif()
add_executable(bloom_playback_controller_test
    playback_controller_tests.cpp
)

# Links ZLIB::ZLIB directly for its own trimmed hand-rolled ZIP-writer fixture (a preserved-
# read-only archive builder) -- mirroring src/host/CMakeLists.txt's bloom_host_session_open_test,
# which documents the same src-module tests/-directory boundary this duplicate exists to respect.
if(NOT TARGET ZLIB::ZLIB)
    find_package(ZLIB REQUIRED)
endif()
add_executable(bloom_main_window_readonly_placeholder_test
    main_window_readonly_placeholder_tests.cpp
)

target_link_libraries(bloom_kit_tokens_test PRIVATE bloom_ui_kit)
target_link_libraries(bloom_composition_projection_test PRIVATE bloom_ui)
target_link_libraries(bloom_composition_preview_controller_test PRIVATE bloom_ui)
target_link_libraries(bloom_composition_session_animation_test PRIVATE bloom_ui)
target_link_libraries(bloom_composition_session_rebind_test PRIVATE bloom_ui)
target_link_libraries(bloom_application_shutdown_test PRIVATE bloom_ui)
target_link_libraries(bloom_project_host_test PRIVATE bloom_ui)
target_link_libraries(bloom_task_monitor_test PRIVATE bloom_ui)
target_link_libraries(bloom_viewer_editor_test PRIVATE bloom_ui)
target_link_libraries(bloom_timeline_frame_math_test PRIVATE bloom_ui)
target_link_libraries(bloom_timeline_ruler_test PRIVATE bloom_ui)
target_link_libraries(bloom_composition_session_position_interaction_test PRIVATE bloom_ui)
target_link_libraries(bloom_direct_manipulation_test PRIVATE bloom_ui)
target_link_libraries(bloom_frame_export_controller_test PRIVATE bloom_ui)
target_link_libraries(bloom_main_window_readonly_placeholder_test PRIVATE bloom_ui ZLIB::ZLIB)
target_link_libraries(bloom_playback_controller_test PRIVATE bloom_ui Qt6::Test)
bloom_enable_warnings(bloom_kit_tokens_test)
bloom_enable_warnings(bloom_composition_projection_test)
bloom_enable_warnings(bloom_composition_preview_controller_test)
bloom_enable_warnings(bloom_composition_session_animation_test)
bloom_enable_warnings(bloom_composition_session_rebind_test)
bloom_enable_warnings(bloom_application_shutdown_test)
bloom_enable_warnings(bloom_project_host_test)
bloom_enable_warnings(bloom_task_monitor_test)
bloom_enable_warnings(bloom_viewer_editor_test)
bloom_enable_warnings(bloom_timeline_frame_math_test)
bloom_enable_warnings(bloom_timeline_ruler_test)
bloom_enable_warnings(bloom_composition_session_position_interaction_test)
bloom_enable_warnings(bloom_direct_manipulation_test)
bloom_enable_warnings(bloom_frame_export_controller_test)
bloom_enable_warnings(bloom_main_window_readonly_placeholder_test)
bloom_enable_warnings(bloom_playback_controller_test)

include(BloomTesting)

bloom_add_test(
    NAME bloom.ui.kit_tokens
    COMMAND bloom_kit_tokens_test
    LABELS ui unit kit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

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
    NAME bloom.ui.composition_session_rebind
    COMMAND bloom_composition_session_rebind_test
    LABELS ui unit
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
    NAME bloom.ui.project_host
    COMMAND bloom_project_host_test
    LABELS ui integration host
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

bloom_add_test(
    NAME bloom.ui.timeline_frame_math
    COMMAND bloom_timeline_frame_math_test
    LABELS ui unit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.timeline_ruler
    COMMAND bloom_timeline_ruler_test
    LABELS ui integration animation
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.composition_session_position_interaction
    COMMAND bloom_composition_session_position_interaction_test
    LABELS ui unit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.direct_manipulation
    COMMAND bloom_direct_manipulation_test
    LABELS ui integration
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.main_window_readonly_placeholder
    COMMAND bloom_main_window_readonly_placeholder_test
    LABELS ui integration host
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.frame_export_controller
    COMMAND bloom_frame_export_controller_test
    LABELS ui integration host output
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.playback_controller
    COMMAND bloom_playback_controller_test
    LABELS ui integration animation
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)
