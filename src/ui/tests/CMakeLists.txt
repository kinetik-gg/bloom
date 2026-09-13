add_executable(bloom_editor_area_chrome_test
    editor_area_chrome_tests.cpp
)
add_executable(bloom_kinetik_baseline_test
    kinetik_baseline_tests.cpp
)
add_executable(bloom_kit_button_test
    kit_button_tests.cpp
)
add_executable(bloom_kit_choice_test
    kit_choice_tests.cpp
)
add_executable(bloom_kit_color_test
    kit_color_tests.cpp
)
add_executable(bloom_kit_color_chip_test
    kit_color_chip_tests.cpp
)
add_executable(bloom_kit_color_picker_test
    kit_color_picker_tests.cpp
)
add_executable(bloom_kit_color_sampler_test
    kit_color_sampler_tests.cpp
)
add_executable(bloom_kit_color_swatches_test
    kit_color_swatches_tests.cpp
)
add_executable(bloom_kit_dropdown_test
    kit_dropdown_tests.cpp
)
add_executable(bloom_kit_focus_border_test
    kit_focus_border_tests.cpp
)
add_executable(bloom_kit_fonts_test
    kit_fonts_tests.cpp
)
add_executable(bloom_kit_panel_switcher_test
    kit_panel_switcher_tests.cpp
)
add_executable(bloom_kit_icons_test
    kit_icons_tests.cpp
)
add_executable(bloom_kit_title_bar_test
    kit_title_bar_tests.cpp
)
add_executable(bloom_kit_mnemonic_style_test
    kit_mnemonic_style_tests.cpp
)
add_executable(bloom_third_party_license_catalog_test
    third_party_license_catalog_tests.cpp
)
add_executable(bloom_main_window_chrome_test
    main_window_chrome_tests.cpp
)
add_executable(bloom_frameless_window_support_test
    frameless_window_support_tests.cpp
)
add_executable(bloom_kit_range_test
    kit_range_tests.cpp
)
add_executable(bloom_kit_value_field_test
    kit_value_field_tests.cpp
)
add_executable(bloom_kit_theme_test
    kit_theme_tests.cpp
)
add_executable(bloom_kit_tokens_test
    kit_tokens_tests.cpp
)
add_executable(bloom_composition_projection_test
    composition_projection_test.cpp
)
add_executable(bloom_properties_editor_test
    properties_editor_tests.cpp
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

# QSignalSpy and QTest::keyClick -- issue #105's real key-event dispatch through Qt's own shortcut
# machinery, and issue #117's header-chrome signal assertions -- need Qt6::Test. Scoped to the two
# executables that use it rather than to bloom_ui itself.
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

# Task U7 (issue #122): TimelineEditor track-row/transport Kinetik restyle tests, appended rather
# than folded into timeline_ruler_tests.cpp (which owns the ruler/keyframe-panel interaction
# contract this task must keep byte-equivalent).
add_executable(bloom_timeline_editor_style_test
    timeline_editor_style_tests.cpp
)

# Task U4 (issue #123): the node editor's navigation, selection, typed connector, context menu and
# in-node editing contracts. Its own executable rather than more cases inside
# composition_projection_test.cpp, which owns the cross-editor projection/selection contract this
# task keeps byte-equivalent.
add_executable(bloom_node_editor_test
    node_editor_tests.cpp
)

target_link_libraries(bloom_editor_area_chrome_test PRIVATE bloom_ui Qt6::Test)
target_link_libraries(bloom_kinetik_baseline_test PRIVATE bloom_ui Qt6::Test)
target_link_libraries(bloom_kit_button_test PRIVATE bloom_ui_kit)
target_link_libraries(bloom_kit_choice_test PRIVATE bloom_ui_kit Qt6::Test)
target_link_libraries(bloom_kit_color_test PRIVATE bloom_ui_kit)
target_link_libraries(bloom_kit_color_chip_test PRIVATE bloom_ui_kit Qt6::Test)
target_link_libraries(bloom_kit_color_picker_test PRIVATE bloom_ui_kit)
target_link_libraries(bloom_kit_color_sampler_test PRIVATE bloom_ui_kit)
target_link_libraries(bloom_kit_color_swatches_test PRIVATE bloom_ui_kit Qt6::Test)
target_link_libraries(bloom_kit_dropdown_test PRIVATE bloom_ui_kit Qt6::Test)
target_link_libraries(bloom_kit_focus_border_test PRIVATE bloom_ui_kit)
target_link_libraries(bloom_kit_fonts_test PRIVATE bloom_ui_kit)
target_link_libraries(bloom_kit_panel_switcher_test PRIVATE bloom_ui_kit Qt6::Test)
target_link_libraries(bloom_kit_icons_test PRIVATE bloom_ui_kit)
target_link_libraries(bloom_kit_title_bar_test PRIVATE bloom_ui_kit Qt6::Test)
target_link_libraries(bloom_kit_mnemonic_style_test PRIVATE bloom_ui_kit)
target_link_libraries(bloom_third_party_license_catalog_test PRIVATE bloom_ui)
target_link_libraries(bloom_main_window_chrome_test PRIVATE bloom_ui Qt6::Test)
target_link_libraries(bloom_frameless_window_support_test PRIVATE bloom_ui)
target_link_libraries(bloom_kit_range_test PRIVATE bloom_ui_kit Qt6::Test)
target_link_libraries(bloom_kit_value_field_test PRIVATE bloom_ui_kit Qt6::Test)
target_link_libraries(bloom_kit_theme_test PRIVATE bloom_ui_kit)
target_link_libraries(bloom_kit_tokens_test PRIVATE bloom_ui_kit)
target_link_libraries(bloom_composition_projection_test PRIVATE bloom_ui)
target_link_libraries(bloom_properties_editor_test PRIVATE bloom_ui)
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
target_link_libraries(bloom_timeline_editor_style_test PRIVATE bloom_ui)
target_link_libraries(bloom_node_editor_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_editor_area_chrome_test)
bloom_enable_warnings(bloom_kinetik_baseline_test)
bloom_enable_warnings(bloom_kit_button_test)
bloom_enable_warnings(bloom_kit_choice_test)
bloom_enable_warnings(bloom_kit_color_test)
bloom_enable_warnings(bloom_kit_color_chip_test)
bloom_enable_warnings(bloom_kit_color_picker_test)
bloom_enable_warnings(bloom_kit_color_sampler_test)
bloom_enable_warnings(bloom_kit_color_swatches_test)
bloom_enable_warnings(bloom_kit_dropdown_test)
bloom_enable_warnings(bloom_kit_focus_border_test)
bloom_enable_warnings(bloom_kit_fonts_test)
bloom_enable_warnings(bloom_kit_panel_switcher_test)
bloom_enable_warnings(bloom_kit_icons_test)
bloom_enable_warnings(bloom_kit_title_bar_test)
bloom_enable_warnings(bloom_kit_mnemonic_style_test)
bloom_enable_warnings(bloom_third_party_license_catalog_test)
bloom_enable_warnings(bloom_main_window_chrome_test)
bloom_enable_warnings(bloom_frameless_window_support_test)
bloom_enable_warnings(bloom_kit_range_test)
bloom_enable_warnings(bloom_kit_value_field_test)
bloom_enable_warnings(bloom_kit_theme_test)
bloom_enable_warnings(bloom_kit_tokens_test)
bloom_enable_warnings(bloom_composition_projection_test)
bloom_enable_warnings(bloom_properties_editor_test)
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
bloom_enable_warnings(bloom_timeline_editor_style_test)
bloom_enable_warnings(bloom_node_editor_test)

include(BloomTesting)

bloom_add_test(
    NAME bloom.ui.editor_area_chrome
    COMMAND bloom_editor_area_chrome_test
    LABELS ui unit kit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.kinetik_baseline
    COMMAND bloom_kinetik_baseline_test
    LABELS ui integration kit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.kit_button
    COMMAND bloom_kit_button_test
    LABELS ui unit kit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.kit_choice
    COMMAND bloom_kit_choice_test
    LABELS ui unit kit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.kit_color
    COMMAND bloom_kit_color_test
    LABELS ui unit kit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.kit_color_chip
    COMMAND bloom_kit_color_chip_test
    LABELS ui unit kit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.kit_color_picker
    COMMAND bloom_kit_color_picker_test
    LABELS ui unit kit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.kit_color_sampler
    COMMAND bloom_kit_color_sampler_test
    LABELS ui unit kit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.kit_color_swatches
    COMMAND bloom_kit_color_swatches_test
    LABELS ui unit kit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.kit_dropdown
    COMMAND bloom_kit_dropdown_test
    LABELS ui unit kit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.kit_focus_border
    COMMAND bloom_kit_focus_border_test
    LABELS ui unit kit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.kit_fonts
    COMMAND bloom_kit_fonts_test
    LABELS ui unit kit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.kit_panel_switcher
    COMMAND bloom_kit_panel_switcher_test
    LABELS ui unit kit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.kit_icons
    COMMAND bloom_kit_icons_test
    LABELS ui unit kit
    TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.kit_title_bar
    COMMAND bloom_kit_title_bar_test
    LABELS ui unit kit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.kit_mnemonic_style
    COMMAND bloom_kit_mnemonic_style_test
    LABELS ui unit kit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.third_party_license_catalog
    COMMAND bloom_third_party_license_catalog_test --root "${PROJECT_SOURCE_DIR}"
    LABELS ui unit quality
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.main_window_chrome
    COMMAND bloom_main_window_chrome_test
    LABELS ui integration
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.frameless_window_support
    COMMAND bloom_frameless_window_support_test
    LABELS ui unit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.kit_range
    COMMAND bloom_kit_range_test
    LABELS ui unit kit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.kit_theme
    COMMAND bloom_kit_theme_test
    LABELS ui unit kit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.kit_value_field
    COMMAND bloom_kit_value_field_test
    LABELS ui unit kit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

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
    NAME bloom.ui.properties_editor
    COMMAND bloom_properties_editor_test
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

bloom_add_test(
    NAME bloom.ui.timeline_editor_style
    COMMAND bloom_timeline_editor_style_test
    LABELS ui integration kit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.node_editor
    COMMAND bloom_node_editor_test
    LABELS ui integration kit
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

add_executable(bloom_ui_multi_selection_test composition_session_multi_selection_tests.cpp)
target_link_libraries(bloom_ui_multi_selection_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_ui_multi_selection_test)
bloom_add_test(NAME bloom.ui.multi-selection COMMAND bloom_ui_multi_selection_test LABELS unit ui)

add_executable(bloom_node_interaction_test node_interaction_tests.cpp node_connection_tests.cpp node_search_keyboard_tests.cpp)
target_include_directories(bloom_node_interaction_test PRIVATE ..)
target_link_libraries(bloom_node_interaction_test PRIVATE bloom_ui bloom_project Qt6::Test)
bloom_enable_warnings(bloom_node_interaction_test)
bloom_add_test(NAME bloom.ui.node-interaction COMMAND bloom_node_interaction_test LABELS ui integration kit TIMEOUT 30 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Task S1: the node card's own primitives (in-card field geometry, the content-derived card
# minimum, socket identity). Its own executable rather than more cases inside
# node_interaction_tests.cpp, which owns the gesture/command contract this task keeps intact.
add_executable(bloom_node_card_test node_card_tests.cpp)
target_include_directories(bloom_node_card_test PRIVATE ..)
target_link_libraries(bloom_node_card_test PRIVATE bloom_ui bloom_project Qt6::Test)
bloom_enable_warnings(bloom_node_card_test)
bloom_add_test(NAME bloom.ui.node-card COMMAND bloom_node_card_test LABELS ui integration kit TIMEOUT 30 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Task S2 (issue #132): node group frames -- geometry, frame drag, membership by drop, inline title
# rename, and the two menus. Its own executable for the same reason node_card_tests.cpp is.
add_executable(bloom_node_group_test node_group_tests.cpp)
target_include_directories(bloom_node_group_test PRIVATE ..)
target_link_libraries(bloom_node_group_test PRIVATE bloom_ui bloom_project Qt6::Test)
bloom_enable_warnings(bloom_node_group_test)
bloom_add_test(NAME bloom.ui.node-groups COMMAND bloom_node_group_test LABELS ui integration kit TIMEOUT 30 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# The node canvas's lifetime contract against Qt's scene index: what a card's drag elevation has to
# announce before it shrinks, so that nothing the scene has given up or freed is left behind in the
# BSP leaves a later query or paint walks. Its own executable for the same reason node_card_tests.cpp
# is.
add_executable(bloom_node_canvas_lifetime_test node_canvas_lifetime_tests.cpp)
target_include_directories(bloom_node_canvas_lifetime_test PRIVATE ..)
target_link_libraries(bloom_node_canvas_lifetime_test PRIVATE bloom_ui bloom_project Qt6::Test)
bloom_enable_warnings(bloom_node_canvas_lifetime_test)
bloom_add_test(NAME bloom.ui.node-canvas-lifetime COMMAND bloom_node_canvas_lifetime_test LABELS ui integration kit TIMEOUT 30 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(bloom_node_driver_gesture_test node_driver_gesture_tests.cpp)
target_include_directories(bloom_node_driver_gesture_test PRIVATE ..)
target_link_libraries(bloom_node_driver_gesture_test PRIVATE bloom_ui bloom_runtime bloom_render Qt6::Test)
bloom_enable_warnings(bloom_node_driver_gesture_test)
bloom_add_test(NAME bloom.ui.node-driver-gesture COMMAND bloom_node_driver_gesture_test LABELS ui integration kit TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(bloom_node_production_wiring_test node_production_wiring_tests.cpp)
target_include_directories(bloom_node_production_wiring_test PRIVATE ..)
target_link_libraries(bloom_node_production_wiring_test PRIVATE bloom_ui bloom_project Qt6::Test)
bloom_enable_warnings(bloom_node_production_wiring_test)
bloom_add_test(NAME bloom.ui.node-production-wiring COMMAND bloom_node_production_wiring_test LABELS ui integration kit TIMEOUT 30 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
