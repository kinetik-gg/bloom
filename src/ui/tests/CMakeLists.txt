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
add_executable(bloom_composition_preview_cpu_stage_test
    composition_preview_cpu_stage_tests.cpp
)
add_executable(bloom_composition_preview_gpu_scene_stage_test
    composition_preview_gpu_scene_stage_tests.cpp
)
add_executable(bloom_gpu_preview_submitter_test
    gpu_preview_submitter_tests.cpp
)
add_executable(bloom_gpu_preview_app_test
    gpu_preview_app_tests.cpp
)
# bloom_ui_gpu_presentation_target_test adopts the borrowed VkInstance through Qt's public
# QVulkanInstance, so it is created in the capability-gated block below (BLOOM_UI_VULKAN_AVAILABLE
# is the conjunction of the pinned Vulkan headers and a Qt build with the Vulkan feature).
# Independent QPainter oracle for the present-image shader's filtering/channel rules. Qt-only, links
# Qt6::Gui (the app supplies Qt6::Widgets); it never mirrors the shader algebra.
add_executable(bloom_ui_gpu_present_image_qpainter_oracle_test
    gpu_present_image_qpainter_oracle_tests.cpp
)
# Native offscreen proof for the P2/P3 presentation sampling path. Qt supplies the independent
# QPainter oracle; the production render draw is compiled against the render module's private
# Vulkan headers, so the target is created only where that private seam is available (below).
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
add_executable(bloom_timeline_graph_math_test
    timeline_graph_math_tests.cpp
)
add_executable(bloom_timeline_graph_view_test
    timeline_graph_view_tests.cpp
)
add_executable(bloom_composition_session_position_interaction_test
    composition_session_position_interaction_tests.cpp
)
add_executable(bloom_gesture_finiteness_test
    gesture_finiteness_tests.cpp
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
target_link_libraries(bloom_composition_preview_cpu_stage_test PRIVATE bloom_ui)
target_link_libraries(bloom_composition_preview_gpu_scene_stage_test PRIVATE bloom_ui)
target_include_directories(bloom_composition_preview_gpu_scene_stage_test
    PRIVATE "${PROJECT_SOURCE_DIR}/src/runtime/tests" "${PROJECT_SOURCE_DIR}/src/runtime")
target_link_libraries(bloom_gpu_preview_submitter_test PRIVATE bloom_ui)
target_link_libraries(bloom_gpu_preview_app_test PRIVATE bloom_ui bloom_runtime_evaluator)
# bloom_ui_gpu_presentation_target_test adopts the borrowed VkInstance through Qt's public
# QVulkanInstance, so it exists only where this build has both the pinned Vulkan headers and a Qt
# build that shipped the Vulkan feature (BLOOM_UI_VULKAN_AVAILABLE, src/ui/CMakeLists.txt). On a Qt
# without that feature -- the official Qt 6.8 macOS packages -- the target is not created and the
# Vulkan-free UI tests still build and run.
if(BLOOM_UI_VULKAN_AVAILABLE)
    add_executable(bloom_ui_gpu_presentation_target_test
        gpu_presentation_target_tests.cpp
    )
    target_link_libraries(bloom_ui_gpu_presentation_target_test
        PRIVATE bloom_ui bloom_render Vulkan::Headers)
    # QVulkanInstance's public class requires <vulkan/vulkan.h> on the application include path; the
    # pinned dependency prefix supplies it. Only the tests need this; the UI and render modules stay
    # Vulkan-header-free in their public interfaces.
    if(DEFINED BLOOM_DEPENDENCY_PREFIX AND IS_DIRECTORY "${BLOOM_DEPENDENCY_PREFIX}/include/vulkan")
        target_include_directories(bloom_ui_gpu_presentation_target_test
            SYSTEM PRIVATE "${BLOOM_DEPENDENCY_PREFIX}/include")
    endif()
    bloom_enable_warnings(bloom_ui_gpu_presentation_target_test)
endif()
# The QPainter oracle is Qt-only (Qt6::Gui); it never links render. It uses std::source_location,
# so it needs the project's C++20 level explicitly (Qt's default here is C++17).
target_link_libraries(bloom_ui_gpu_present_image_qpainter_oracle_test PRIVATE Qt6::Gui)
target_compile_features(bloom_ui_gpu_present_image_qpainter_oracle_test PRIVATE cxx_std_20)
# The injected-construction-fault seam lives in bloom_render's private Vulkan sources. A sibling
# directory's BLOOM_RENDER_VULKAN_AVAILABLE is not visible here, so repeat the same mode-aware probe
# before compiling the private-header fault case and adding the private include root. This seam is
# independent of Qt's Vulkan feature: the offscreen proof below drives the render module directly
# and never includes QVulkanInstance, so it stays available wherever the render Vulkan backend is.
set(BLOOM_UI_PRESENTATION_TARGET_PRIVATE_SEAM OFF)
if(BLOOM_RENDER_ENABLE_VULKAN AND DEFINED BLOOM_DEPENDENCY_MODE)
    if(BLOOM_DEPENDENCY_MODE STREQUAL "qualified")
        find_package(VulkanHeaders QUIET CONFIG
            PATHS "${BLOOM_DEPENDENCY_PREFIX}" NO_DEFAULT_PATH)
        find_package(VulkanMemoryAllocator QUIET CONFIG
            PATHS "${BLOOM_DEPENDENCY_PREFIX}" NO_DEFAULT_PATH)
    elseif(BLOOM_DEPENDENCY_MODE STREQUAL "developer-system")
        find_package(VulkanHeaders QUIET CONFIG)
        find_package(VulkanMemoryAllocator QUIET CONFIG)
    endif()
    if(TARGET Vulkan::Headers AND TARGET GPUOpen::VulkanMemoryAllocator)
        set(BLOOM_UI_PRESENTATION_TARGET_PRIVATE_SEAM ON)
    endif()
endif()
if(BLOOM_UI_PRESENTATION_TARGET_PRIVATE_SEAM)
    if(TARGET bloom_ui_gpu_presentation_target_test)
        target_compile_definitions(bloom_ui_gpu_presentation_target_test
            PRIVATE BLOOM_UI_GPU_PRESENTATION_TARGET_PRIVATE_SEAM=1)
        target_include_directories(bloom_ui_gpu_presentation_target_test
            PRIVATE "${PROJECT_SOURCE_DIR}/src/render/vulkan")
    endif()
    # The offscreen native proof includes the render module's private Vulkan headers and needs the
    # same prefix-provided Vulkan-Hpp/VMA headers. It links bloom_render + bloom_color_ocio (the
    # latter only for the independent CPU OCIO oracle) and Qt6::Gui (the QPainter oracle).
    add_executable(bloom_ui_gpu_present_image_offscreen_test
        gpu_present_image_offscreen_tests.cpp
    )
    target_link_libraries(bloom_ui_gpu_present_image_offscreen_test
        PRIVATE bloom_ui bloom_render bloom_color_ocio Qt6::Gui)
    target_include_directories(bloom_ui_gpu_present_image_offscreen_test
        PRIVATE "${PROJECT_SOURCE_DIR}/src/render/vulkan" "${CMAKE_CURRENT_SOURCE_DIR}")
    if(DEFINED BLOOM_DEPENDENCY_PREFIX AND IS_DIRECTORY "${BLOOM_DEPENDENCY_PREFIX}/include/vulkan")
        target_include_directories(bloom_ui_gpu_present_image_offscreen_test
            SYSTEM PRIVATE "${BLOOM_DEPENDENCY_PREFIX}/include")
    endif()
    target_compile_definitions(bloom_ui_gpu_present_image_offscreen_test
        PRIVATE BLOOM_RENDER_TEST_VULKAN_BACKEND=1)
    bloom_enable_warnings(bloom_ui_gpu_present_image_offscreen_test)
    bloom_enable_strict_floating_point(bloom_ui_gpu_present_image_offscreen_test)
endif()
target_link_libraries(bloom_composition_session_animation_test PRIVATE bloom_ui)
target_link_libraries(bloom_composition_session_rebind_test PRIVATE bloom_ui)
target_link_libraries(bloom_application_shutdown_test PRIVATE bloom_ui)
target_link_libraries(bloom_project_host_test PRIVATE bloom_ui)
target_link_libraries(bloom_task_monitor_test PRIVATE bloom_ui)
target_link_libraries(bloom_viewer_editor_test PRIVATE bloom_ui)
target_link_libraries(bloom_timeline_frame_math_test PRIVATE bloom_ui)
target_link_libraries(bloom_timeline_ruler_test PRIVATE bloom_ui)
target_include_directories(bloom_timeline_ruler_test PRIVATE ..)
target_link_libraries(bloom_timeline_graph_math_test PRIVATE bloom_ui)
target_link_libraries(bloom_timeline_graph_view_test PRIVATE bloom_ui)
# The graph editor's arithmetic is a PRIVATE header of src/ui (it has no public consumers), so
# these two tests reach it through the module's own source root rather than a relative path that
# would bypass the include-root boundary.
target_include_directories(bloom_timeline_graph_math_test PRIVATE ..)
target_include_directories(bloom_timeline_graph_view_test PRIVATE ..)
target_link_libraries(bloom_composition_session_position_interaction_test PRIVATE bloom_ui)
target_link_libraries(bloom_gesture_finiteness_test PRIVATE bloom_ui)
target_link_libraries(bloom_direct_manipulation_test PRIVATE bloom_ui)
target_link_libraries(bloom_frame_export_controller_test PRIVATE bloom_ui)
target_include_directories(bloom_frame_export_controller_test PRIVATE ..)
if(TARGET bloom-media-worker)
    add_dependencies(bloom_frame_export_controller_test bloom-media-worker)
endif()
target_link_libraries(bloom_main_window_readonly_placeholder_test PRIVATE bloom_ui ZLIB::ZLIB)
target_link_libraries(bloom_playback_controller_test PRIVATE bloom_ui Qt6::Test)
target_link_libraries(bloom_timeline_editor_style_test PRIVATE bloom_ui Qt6::Test)
# Task DRIVE-1's upstream-group pin reads the timeline's own row vocabulary (upstreamGroupKey).
target_include_directories(bloom_timeline_editor_style_test PRIVATE ..)
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
bloom_enable_warnings(bloom_composition_preview_cpu_stage_test)
bloom_enable_warnings(bloom_composition_preview_gpu_scene_stage_test)
# The frozen replay oracle header defines anonymous-namespace helpers this TU does not all use; the
# other consumer uses them all, so only this translation unit quiets unused-function.
target_compile_options(bloom_composition_preview_gpu_scene_stage_test PRIVATE -Wno-unused-function)
bloom_enable_warnings(bloom_gpu_preview_submitter_test)
bloom_enable_warnings(bloom_gpu_preview_app_test)
bloom_enable_warnings(bloom_ui_gpu_present_image_qpainter_oracle_test)
bloom_enable_warnings(bloom_composition_session_animation_test)
bloom_enable_warnings(bloom_composition_session_rebind_test)
bloom_enable_warnings(bloom_application_shutdown_test)
bloom_enable_warnings(bloom_project_host_test)
bloom_enable_warnings(bloom_task_monitor_test)
bloom_enable_warnings(bloom_viewer_editor_test)
bloom_enable_warnings(bloom_timeline_frame_math_test)
bloom_enable_warnings(bloom_timeline_ruler_test)
bloom_enable_warnings(bloom_timeline_graph_math_test)
bloom_enable_warnings(bloom_timeline_graph_view_test)
bloom_enable_warnings(bloom_composition_session_position_interaction_test)
bloom_enable_warnings(bloom_gesture_finiteness_test)
bloom_enable_warnings(bloom_direct_manipulation_test)
bloom_enable_warnings(bloom_frame_export_controller_test)
bloom_enable_warnings(bloom_main_window_readonly_placeholder_test)
bloom_enable_warnings(bloom_playback_controller_test)
bloom_enable_warnings(bloom_timeline_editor_style_test)
bloom_enable_warnings(bloom_node_editor_test)

add_executable(bloom_assets_editor_test assets_editor_tests.cpp)
target_link_libraries(bloom_assets_editor_test PRIVATE bloom_ui Qt6::Test)
bloom_enable_warnings(bloom_assets_editor_test)

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
    NAME bloom.ui.composition_preview_cpu_stage
    COMMAND bloom_composition_preview_cpu_stage_test
    LABELS ui integration
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# GPU-scene preview preparation stage: the CPU-only analogue of the CPU stage. It runs through the
# real SnapshotCompiler, CpuGpuSceneBuilder, QualifiedDisplayProcessorProvider and TaskScheduler and
# checks the prepared scene against a genuine CpuCompositionEvaluator oracle; no device or loader.
bloom_add_test(
    NAME bloom.ui.composition_preview_gpu_scene_stage
    COMMAND bloom_composition_preview_gpu_scene_stage_test
    LABELS ui integration
    TIMEOUT 120
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.gpu_preview_submitter
    COMMAND bloom_gpu_preview_submitter_test
    LABELS ui integration gpu
    TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.gpu_preview_app
    COMMAND bloom_gpu_preview_app_test
    LABELS ui integration gpu
    TIMEOUT 300
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

if(TARGET bloom_ui_gpu_presentation_target_test)
    bloom_add_test(
        NAME bloom.ui.gpu_presentation_target
        COMMAND bloom_ui_gpu_presentation_target_test
        LABELS ui integration gpu
        TIMEOUT 120
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )
endif()

bloom_add_test(
    NAME bloom.ui.gpu_present_image_qpainter_oracle
    COMMAND bloom_ui_gpu_present_image_qpainter_oracle_test
    LABELS ui unit gpu
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

if(TARGET bloom_ui_gpu_present_image_offscreen_test)
    bloom_add_test(
        NAME bloom.ui.gpu_present_image_offscreen
        COMMAND bloom_ui_gpu_present_image_offscreen_test
        LABELS ui integration gpu
        TIMEOUT 600
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )
endif()

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
    NAME bloom.ui.timeline_graph_math
    COMMAND bloom_timeline_graph_math_test
    LABELS ui unit animation
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

bloom_add_test(
    NAME bloom.ui.timeline_graph_view
    COMMAND bloom_timeline_graph_view_test
    LABELS ui integration animation
    TIMEOUT 60
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
    NAME bloom.ui.gesture_finiteness
    COMMAND bloom_gesture_finiteness_test
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

bloom_add_test(
    NAME bloom.ui.assets-editor
    COMMAND bloom_assets_editor_test
    LABELS ui integration assets
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

add_executable(bloom_project_host_empty_test project_host_empty_tests.cpp)
target_link_libraries(bloom_project_host_empty_test PRIVATE bloom_ui Qt6::Test)
bloom_enable_warnings(bloom_project_host_empty_test)
bloom_add_test(
    NAME bloom.ui.project-host-empty
    COMMAND bloom_project_host_empty_test
    LABELS ui integration
    TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

add_executable(bloom_composition_duration_test composition_duration_tests.cpp)
target_link_libraries(bloom_composition_duration_test PRIVATE bloom_ui Qt6::Test)
bloom_enable_warnings(bloom_composition_duration_test)
bloom_add_test(
    NAME bloom.ui.composition-duration
    COMMAND bloom_composition_duration_test
    LABELS ui integration
    TIMEOUT 60
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

add_executable(bloom_node_layout_arrange_test node_layout_arrange_tests.cpp)
target_include_directories(bloom_node_layout_arrange_test PRIVATE ..)
target_link_libraries(bloom_node_layout_arrange_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_node_layout_arrange_test)
bloom_add_test(NAME bloom.ui.node-layout-arrange COMMAND bloom_node_layout_arrange_test LABELS ui unit TIMEOUT 30)

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

add_executable(bloom_node_value_animation_test node_value_animation_tests.cpp)
target_include_directories(bloom_node_value_animation_test PRIVATE ..)
target_link_libraries(bloom_node_value_animation_test PRIVATE bloom_ui bloom_runtime bloom_render Qt6::Test)
bloom_enable_warnings(bloom_node_value_animation_test)
bloom_add_test(NAME bloom.ui.node-value-animation COMMAND bloom_node_value_animation_test LABELS ui integration kit TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(bloom_node_delete_key_test node_delete_key_tests.cpp)
target_include_directories(bloom_node_delete_key_test PRIVATE ..)
target_link_libraries(bloom_node_delete_key_test PRIVATE bloom_ui bloom_runtime bloom_render Qt6::Test)
bloom_enable_warnings(bloom_node_delete_key_test)
bloom_add_test(NAME bloom.ui.node-delete-key COMMAND bloom_node_delete_key_test LABELS ui integration kit TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(bloom_node_blend_mode_test node_blend_mode_tests.cpp)
target_include_directories(bloom_node_blend_mode_test PRIVATE ..)
target_link_libraries(bloom_node_blend_mode_test PRIVATE bloom_ui bloom_runtime bloom_render bloom_color Qt6::Test)
bloom_enable_warnings(bloom_node_blend_mode_test)
bloom_add_test(NAME bloom.ui.node-blend-mode COMMAND bloom_node_blend_mode_test LABELS ui integration TIMEOUT 90 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(bloom_node_reroute_test node_reroute_tests.cpp)
target_include_directories(bloom_node_reroute_test PRIVATE ..)
target_link_libraries(bloom_node_reroute_test PRIVATE bloom_ui bloom_runtime bloom_render Qt6::Test)
bloom_enable_warnings(bloom_node_reroute_test)
bloom_add_test(NAME bloom.ui.node-reroute COMMAND bloom_node_reroute_test LABELS ui integration kit TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(bloom_node_link_detach_test node_link_detach_tests.cpp)
target_include_directories(bloom_node_link_detach_test PRIVATE ..)
target_link_libraries(bloom_node_link_detach_test PRIVATE bloom_ui bloom_runtime bloom_render Qt6::Test)
bloom_enable_warnings(bloom_node_link_detach_test)
bloom_add_test(NAME bloom.ui.node-link-detach COMMAND bloom_node_link_detach_test LABELS ui integration kit TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(bloom_node_explicit_wiring_test node_explicit_wiring_tests.cpp)
target_include_directories(bloom_node_explicit_wiring_test PRIVATE ..)
target_link_libraries(bloom_node_explicit_wiring_test PRIVATE bloom_ui bloom_runtime bloom_render Qt6::Test)
bloom_enable_warnings(bloom_node_explicit_wiring_test)
bloom_add_test(NAME bloom.ui.node-explicit-wiring COMMAND bloom_node_explicit_wiring_test LABELS ui integration kit TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(bloom_node_driver_gesture_test node_driver_gesture_tests.cpp)
target_include_directories(bloom_node_driver_gesture_test PRIVATE ..)
target_link_libraries(bloom_node_driver_gesture_test PRIVATE bloom_ui bloom_runtime bloom_render Qt6::Test)
bloom_enable_warnings(bloom_node_driver_gesture_test)
bloom_add_test(NAME bloom.ui.node-driver-gesture COMMAND bloom_node_driver_gesture_test LABELS ui integration kit TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Task FIX2, deliverable 2: one truth across every surface. Its own executable rather than more
# cases inside composition_projection_test.cpp, which owns the cross-editor projection contract this
# task keeps intact.
add_executable(bloom_surface_synchronization_test surface_synchronization_tests.cpp)
target_include_directories(bloom_surface_synchronization_test PRIVATE ..)
target_link_libraries(bloom_surface_synchronization_test PRIVATE bloom_ui bloom_runtime bloom_render Qt6::Test)
bloom_enable_warnings(bloom_surface_synchronization_test)
bloom_add_test(NAME bloom.ui.surface-synchronization COMMAND bloom_surface_synchronization_test LABELS ui integration kit TIMEOUT 90 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Task FIX2, the owner's known case: the timeline row's own controls under a real pointer. Its own
# executable rather than more cases inside timeline_editor_style_tests.cpp, which owns the row
# CHROME contract this task keeps intact.
add_executable(bloom_timeline_row_controls_test timeline_row_controls_tests.cpp)
target_include_directories(bloom_timeline_row_controls_test PRIVATE ..)
target_link_libraries(bloom_timeline_row_controls_test PRIVATE bloom_ui bloom_runtime bloom_render Qt6::Test)
bloom_enable_warnings(bloom_timeline_row_controls_test)
bloom_add_test(NAME bloom.ui.timeline-row-controls COMMAND bloom_timeline_row_controls_test LABELS ui integration kit TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Task FIX2, deliverable 1: the owner's "values in nodes parameters are not workable", driven
# through real pointer and key events on the production canvas. Its own executable for the same
# reason node_card_tests.cpp is -- and deliberately separate from node_production_wiring_tests.cpp,
# which owns the "no adapter on setSubmit()" contract rather than the gesture contract.
add_executable(bloom_node_card_value_gesture_test node_card_value_gesture_tests.cpp)
target_include_directories(bloom_node_card_value_gesture_test PRIVATE ..)
target_link_libraries(bloom_node_card_value_gesture_test PRIVATE bloom_ui bloom_runtime bloom_render Qt6::Test)
bloom_enable_warnings(bloom_node_card_value_gesture_test)
bloom_add_test(NAME bloom.ui.node-card-value-gesture COMMAND bloom_node_card_value_gesture_test LABELS ui integration kit TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(bloom_node_production_wiring_test node_production_wiring_tests.cpp)
target_include_directories(bloom_node_production_wiring_test PRIVATE ..)
target_link_libraries(bloom_node_production_wiring_test PRIVATE bloom_ui bloom_project Qt6::Test)
bloom_enable_warnings(bloom_node_production_wiring_test)
bloom_add_test(NAME bloom.ui.node-production-wiring COMMAND bloom_node_production_wiring_test LABELS ui integration kit TIMEOUT 30 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Task PERF1: the RAM preview cache, the compiled-plan cache, and the RAM Preview command. Its own
# executable rather than more cases inside composition_preview_controller_tests.cpp or
# playback_controller_tests.cpp, each of which owns a contract this task had to leave intact -- and
# this one needs its own counting/pausable preparation function to prove that a cache hit evaluates
# nothing.
add_executable(bloom_ram_preview_test ram_preview_tests.cpp)
target_link_libraries(bloom_ram_preview_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_ram_preview_test)
bloom_add_test(
    NAME bloom.ui.ram-preview
    COMMAND bloom_ram_preview_test
    LABELS ui integration animation
    TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# PERF-RAM-2: the bounded two-deep RAM preview pipeline. Its own executable because the overlap
# proof needs a deterministic two-worker barrier (preparation 2 must start before preparation 1
# finishes) and a controllable hold/release rendezvous for the mid-run range/provenance edits;
# neither belongs in the existing counting fixture. Links bloom_ui alone, exactly as
# bloom_ram_preview_test does, because the submit seam and the service are in that interface.
add_executable(bloom_ram_preview_pipeline_test ram_preview_pipeline_tests.cpp)
target_link_libraries(bloom_ram_preview_pipeline_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_ram_preview_pipeline_test)
bloom_add_test(
    NAME bloom.ui.ram-preview-pipeline
    COMMAND bloom_ram_preview_pipeline_test
    LABELS ui integration animation
    TIMEOUT 120
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# PERF-RAM-2: the real GpuPreviewDisplayService admission seam the two-deep pipeline submits
# through. Kept apart from the behavior suite so that one can be read as controller behavior and
# this one as service admission. With no loader it asserts the honest CPU-fallback/Unavailable
# path; when a pinned loader is present it also drives the enabled service and proves genuine
# two-stage overlap under the real admission (a two-worker barrier), reporting the gate as pending
# rather than failing when no device is actually available.
add_executable(bloom_ram_preview_service_pipeline_test ram_preview_service_pipeline_tests.cpp)
target_link_libraries(bloom_ram_preview_service_pipeline_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_ram_preview_service_pipeline_test)
if(DEFINED BLOOM_DEPENDENCY_PREFIX AND EXISTS "${BLOOM_DEPENDENCY_PREFIX}/lib/libvulkan.so.1")
    set(bloom_ram_preview_vulkan_loader_args
        --loader "${BLOOM_DEPENDENCY_PREFIX}/lib/libvulkan.so.1")
else()
    set(bloom_ram_preview_vulkan_loader_args)
endif()
bloom_add_test(
    NAME bloom.ui.ram-preview-service-pipeline
    COMMAND bloom_ram_preview_service_pipeline_test ${bloom_ram_preview_vulkan_loader_args}
    LABELS ui integration animation
    TIMEOUT 120
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# Task CACHEFIX-1: the byte accounting every cache in the process owes, exercised at the scale of
# the 38 GB incident (200 synthetic 4K RGBA32F frames) and checked against the process's own RSS.
# Links bloom_ui for Qt and the media/runtime cache libraries it pulls in; deliberately not the
# whole window fixture, because the accounting under test lives in the caches.
add_executable(bloom_ui_cache_accounting_test cache_accounting_tests.cpp)
target_link_libraries(bloom_ui_cache_accounting_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_ui_cache_accounting_test)
bloom_add_test(
    NAME bloom.ui.cache-accounting
    COMMAND bloom_ui_cache_accounting_test
    LABELS ui unit cache
    TIMEOUT 300
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# Task CACHE-2: the media disk cache's QSettings surface and status-bar/menu-facing pure logic,
# independent of a full evaluator or window (see main_window_chrome_tests.cpp and
# window_status_bar's own coverage there for the widget-level wiring).
add_executable(bloom_media_disk_cache_settings_test media_disk_cache_settings_tests.cpp)
target_link_libraries(bloom_media_disk_cache_settings_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_media_disk_cache_settings_test)
bloom_add_test(
    NAME bloom.ui.media-disk-cache-settings
    COMMAND bloom_media_disk_cache_settings_test
    LABELS ui unit cache
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# The shared global-preferences model: defaults, round-trip, invalid-value fallback, and the
# absolute-path/zero-budget rules the existing readers rely on. Also covers non-finite grid values.
add_executable(bloom_application_preferences_test application_preferences_tests.cpp)
target_link_libraries(bloom_application_preferences_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_application_preferences_test)
bloom_add_test(
    NAME bloom.ui.application-preferences
    COMMAND bloom_application_preferences_test
    LABELS ui unit
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# The acceleration-status mapping: a pure, device-free mapping from one already-published GPU
# service status to the Performance page model, including initializing, ready-without-resident,
# lost-presentation, and post-bootstrap device-loss truths. It never constructs a service or device.
add_executable(bloom_acceleration_status_test acceleration_status_tests.cpp)
target_link_libraries(bloom_acceleration_status_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_acceleration_status_test)
bloom_add_test(
    NAME bloom.ui.acceleration-status
    COMMAND bloom_acceleration_status_test
    LABELS ui unit
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# The Settings window: control/preference binding, dirty-state gating, Apply emission, restore
# defaults, restart badges, category navigation, and the read-only Performance page.
add_executable(bloom_preferences_window_test preferences_window_tests.cpp)
target_link_libraries(bloom_preferences_window_test PRIVATE bloom_ui)
target_include_directories(bloom_preferences_window_test PRIVATE ..)
bloom_enable_warnings(bloom_preferences_window_test)
bloom_add_test(
    NAME bloom.ui.preferences-window
    COMMAND bloom_preferences_window_test
    LABELS ui unit
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

add_executable(bloom_background_preview_test background_preview_tests.cpp)
target_link_libraries(bloom_background_preview_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_background_preview_test)
bloom_add_test(
    NAME bloom.ui.background-preview
    COMMAND bloom_background_preview_test
    LABELS ui integration animation
    TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

add_executable(bloom_properties_registry_test properties_registry_tests.cpp)
target_link_libraries(bloom_properties_registry_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_properties_registry_test)
bloom_add_test(NAME bloom.ui.properties-registry COMMAND bloom_properties_registry_test LABELS ui integration kit TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(bloom_text_box_overlay_test text_box_overlay_tests.cpp)
target_link_libraries(bloom_text_box_overlay_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_text_box_overlay_test)
bloom_add_test(NAME bloom.ui.text-box-overlay COMMAND bloom_text_box_overlay_test LABELS ui unit TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(bloom_properties_mockup_test properties_mockup_tests.cpp)
target_link_libraries(bloom_properties_mockup_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_properties_mockup_test)
bloom_add_test(NAME bloom.ui.properties-mockup COMMAND bloom_properties_mockup_test LABELS ui integration kit TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(bloom_ui_grammar_audit_test ui_grammar_audit_tests.cpp)
target_include_directories(bloom_ui_grammar_audit_test PRIVATE ..)
target_link_libraries(bloom_ui_grammar_audit_test PRIVATE bloom_ui bloom_runtime_compiler Qt6::Test)
target_compile_definitions(bloom_ui_grammar_audit_test PRIVATE BLOOM_GRAMMAR_ARTIFACT_DIR="${PROJECT_SOURCE_DIR}/build")
bloom_enable_warnings(bloom_ui_grammar_audit_test)
foreach(scale IN ITEMS 1 1.5 2)
    add_test(NAME bloom.ui.grammar_audit.dpr${scale} COMMAND bloom_ui_grammar_audit_test)
    set_tests_properties(bloom.ui.grammar_audit.dpr${scale} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=${scale};BLOOM_REDUCED_MOTION=1"
        LABELS "ui;grammar" TIMEOUT 60)
endforeach()

add_executable(bloom_window_golden_test window_golden_tests.cpp)
target_link_libraries(bloom_window_golden_test PRIVATE bloom_ui bloom_runtime_compiler Qt6::Test)
target_compile_definitions(bloom_window_golden_test PRIVATE
    BLOOM_GRAMMAR_ARTIFACT_DIR="${PROJECT_SOURCE_DIR}/build"
    BLOOM_GRAMMAR_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/goldens")
bloom_enable_warnings(bloom_window_golden_test)
# The whole-window references are platform-specific (glyph rasterization differs between
# FreeType on Linux and CoreText on macOS), so the comparison runs only against the Linux-approved
# references; other platforms build the fixture without registering the comparison until their own
# references exist.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    foreach(scale IN ITEMS 1 1.5)
        add_test(NAME bloom.ui.window_golden.dpr${scale} COMMAND bloom_window_golden_test)
        set_tests_properties(bloom.ui.window_golden.dpr${scale} PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=${scale};BLOOM_REDUCED_MOTION=1"
            LABELS "ui;golden" TIMEOUT 60)
    endforeach()
endif()

add_executable(bloom_media_ui_test media_ui_tests.cpp)
target_include_directories(bloom_media_ui_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bloom_media_ui_test PRIVATE bloom_ui Qt6::Test)
bloom_enable_warnings(bloom_media_ui_test)
foreach(scale IN ITEMS 1 1.5 2)
    add_test(NAME bloom.ui.media.dpr${scale} COMMAND bloom_media_ui_test)
    set_tests_properties(bloom.ui.media.dpr${scale} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=${scale};BLOOM_REDUCED_MOTION=1"
        LABELS "ui;media;integration" TIMEOUT 60)
endforeach()

add_executable(bloom_media_window_capture media_window_capture.cpp)
target_include_directories(bloom_media_window_capture PRIVATE ..)
target_link_libraries(bloom_media_window_capture PRIVATE bloom_ui bloom_runtime_compiler Qt6::Test)
target_compile_definitions(bloom_media_window_capture PRIVATE
    BLOOM_GRAMMAR_ARTIFACT_DIR="${PROJECT_SOURCE_DIR}/build")
bloom_enable_warnings(bloom_media_window_capture)
add_test(NAME bloom.ui.media-window.dpr1 COMMAND bloom_media_window_capture)
set_tests_properties(bloom.ui.media-window.dpr1 PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1;BLOOM_REDUCED_MOTION=1"
    LABELS "ui;media;capture" TIMEOUT 60)

add_executable(bloom_color_boundary_test color_boundary_tests.cpp)
target_link_libraries(bloom_color_boundary_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_color_boundary_test)
foreach(scale IN ITEMS 1 1.25 1.5 2)
    bloom_add_test(NAME bloom.ui.color-boundary.dpr${scale}
        COMMAND bloom_color_boundary_test LABELS ui integration color TIMEOUT 60
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=${scale}")
endforeach()

# End-to-end audio transport: WAV import -> audio layer -> compiled mix -> PlaybackController ->
# AudioEngine (NullBackend) renders non-silent samples, wired exactly like apps/bloom/main.cpp.
add_executable(bloom_audio_transport_test audio_transport_tests.cpp)
target_include_directories(bloom_audio_transport_test PRIVATE ..)
target_link_libraries(bloom_audio_transport_test PRIVATE bloom_ui bloom_runtime_compiler Qt6::Test)
target_compile_definitions(bloom_audio_transport_test PRIVATE
    BLOOM_GRAMMAR_ARTIFACT_DIR="${PROJECT_SOURCE_DIR}/build")
bloom_enable_warnings(bloom_audio_transport_test)
add_test(NAME bloom.ui.audio-transport COMMAND bloom_audio_transport_test)
set_tests_properties(bloom.ui.audio-transport PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1;BLOOM_REDUCED_MOTION=1")

add_executable(bloom_live_value_edit_test live_value_edit_tests.cpp)
target_include_directories(bloom_live_value_edit_test PRIVATE ..)
target_link_libraries(bloom_live_value_edit_test PRIVATE bloom_ui bloom_runtime_compiler Qt6::Test)
bloom_enable_warnings(bloom_live_value_edit_test)
bloom_add_test(NAME bloom.ui.live-value-edit COMMAND bloom_live_value_edit_test
    LABELS ui integration TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(bloom_viewer_text_edit_test viewer_text_edit_tests.cpp)
target_link_libraries(bloom_viewer_text_edit_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_viewer_text_edit_test)
bloom_add_test(
    NAME bloom.ui.viewer_text_edit
    COMMAND bloom_viewer_text_edit_test
    LABELS ui unit
    TIMEOUT 90
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

add_executable(bloom_assets_organization_test asset_organization_tests.cpp)
target_include_directories(bloom_assets_organization_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bloom_assets_organization_test PRIVATE bloom_ui Qt6::Test)
bloom_enable_warnings(bloom_assets_organization_test)
bloom_add_test(NAME bloom.ui.asset-organization COMMAND bloom_assets_organization_test LABELS unit ui)

add_executable(bloom_network_share_paths_test network_share_paths_tests.cpp)
target_include_directories(bloom_network_share_paths_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bloom_network_share_paths_test PRIVATE bloom_ui Qt6::Test)
bloom_enable_warnings(bloom_network_share_paths_test)
bloom_add_test(
    NAME bloom.ui.network-share-paths
    COMMAND bloom_network_share_paths_test
    LABELS unit ui media integration
    TIMEOUT 60
)

if(BLOOM_BUILD_PYTHON)
    add_executable(bloom_script_panel_test script_panel_tests.cpp)
    target_include_directories(bloom_script_panel_test PRIVATE ..)
    target_link_libraries(bloom_script_panel_test PRIVATE bloom_ui bloom_runtime_compiler Qt6::Test)
    target_compile_definitions(bloom_script_panel_test PRIVATE
        BLOOM_GRAMMAR_ARTIFACT_DIR="${PROJECT_SOURCE_DIR}/build"
        BLOOM_SCRIPT_CLI="$<TARGET_FILE:bloom-cli>")
    add_dependencies(bloom_script_panel_test bloom-cli)
    bloom_enable_warnings(bloom_script_panel_test)
    bloom_add_test(NAME bloom.ui.script-panel COMMAND bloom_script_panel_test
        LABELS ui scripting python integration TIMEOUT 120
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;BLOOM_REDUCED_MOTION=1")
endif()

# Presentation coordinator (real Wayland lifecycle) and ViewerGpuPresenter adapter tests. All three
# translation units adopt the borrowed VkInstance through Qt's public QVulkanInstance and therefore
# need both the pinned Vulkan headers and Qt's Vulkan feature; they are registered only when this
# build has them (BLOOM_UI_VULKAN_AVAILABLE, src/ui/CMakeLists.txt). The runtime coordinator's
# Vulkan-free port/bounds gates live in src/runtime/tests. Without a pinned loader argument the
# fixtures skip cleanly; the supervisor passes --loader (and --require-device for the native runs).
# Each fixture also runs a pre-QApplication native Wayland environment gate
# (gpu_native_test_environment.hpp) and returns the CTest skip code 77 when no compositor is
# reachable, so a headless runner reports Skipped instead of crashing inside Qt's platform plugin.
if(BLOOM_UI_VULKAN_AVAILABLE)
    add_executable(bloom_ui_gpu_presentation_coordinator_test
        gpu_presentation_coordinator_tests.cpp)
    target_link_libraries(bloom_ui_gpu_presentation_coordinator_test
        PRIVATE bloom_ui Vulkan::Headers)
    bloom_enable_warnings(bloom_ui_gpu_presentation_coordinator_test)

    add_executable(bloom_ui_gpu_preview_display_service_presentation_test
        gpu_preview_display_service_presentation_tests.cpp)
    target_include_directories(bloom_ui_gpu_preview_display_service_presentation_test
        PRIVATE "${PROJECT_SOURCE_DIR}/src/runtime")
    target_link_libraries(bloom_ui_gpu_preview_display_service_presentation_test
        PRIVATE bloom_ui Vulkan::Headers)
    bloom_enable_warnings(bloom_ui_gpu_preview_display_service_presentation_test)

    add_executable(bloom_ui_gpu_preview_display_service_resident_test
        gpu_preview_display_service_resident_tests.cpp)
    target_include_directories(bloom_ui_gpu_preview_display_service_resident_test
        PRIVATE "${PROJECT_SOURCE_DIR}/src/runtime")
    target_link_libraries(bloom_ui_gpu_preview_display_service_resident_test
        PRIVATE bloom_ui Vulkan::Headers)
    bloom_enable_warnings(bloom_ui_gpu_preview_display_service_resident_test)

    add_executable(bloom_ui_viewer_gpu_presenter_test
        viewer_gpu_presenter_fake_port_tests.cpp)
    target_include_directories(bloom_ui_viewer_gpu_presenter_test
        PRIVATE "${PROJECT_SOURCE_DIR}/src/ui")
    target_link_libraries(bloom_ui_viewer_gpu_presenter_test
        PRIVATE bloom_ui Vulkan::Headers Qt6::Test)
    bloom_enable_warnings(bloom_ui_viewer_gpu_presenter_test)

    add_executable(bloom_ui_viewer_gpu_presenter_native_test
        viewer_gpu_presenter_native_tests.cpp)
    target_link_libraries(bloom_ui_viewer_gpu_presenter_native_test
        PRIVATE bloom_ui Vulkan::Headers Qt6::Test)
    bloom_enable_warnings(bloom_ui_viewer_gpu_presenter_native_test)

    if(DEFINED BLOOM_DEPENDENCY_PREFIX AND EXISTS "${BLOOM_DEPENDENCY_PREFIX}/lib/libvulkan.so.1")
        set(bloom_ui_vulkan_loader_args
            --loader "${BLOOM_DEPENDENCY_PREFIX}/lib/libvulkan.so.1")
    else()
        set(bloom_ui_vulkan_loader_args)
    endif()
    bloom_add_test(
        NAME bloom.ui.gpu_presentation_coordinator
        COMMAND bloom_ui_gpu_presentation_coordinator_test ${bloom_ui_vulkan_loader_args}
        LABELS ui integration gpu presentation TIMEOUT 300
        ENVIRONMENT "QT_QPA_PLATFORM=wayland"
    )
    bloom_add_test(
        NAME bloom.ui.gpu_preview_display_service_presentation
        COMMAND bloom_ui_gpu_preview_display_service_presentation_test ${bloom_ui_vulkan_loader_args}
        LABELS ui integration gpu presentation TIMEOUT 300
        ENVIRONMENT "QT_QPA_PLATFORM=wayland"
    )
    bloom_add_test(
        NAME bloom.ui.gpu_preview_display_service_resident
        COMMAND bloom_ui_gpu_preview_display_service_resident_test ${bloom_ui_vulkan_loader_args}
        LABELS ui integration gpu presentation TIMEOUT 300
        ENVIRONMENT "QT_QPA_PLATFORM=wayland"
    )
    bloom_add_test(
        NAME bloom.ui.viewer_gpu_presenter
        COMMAND bloom_ui_viewer_gpu_presenter_test ${bloom_ui_vulkan_loader_args}
        LABELS ui integration gpu presentation TIMEOUT 180
        ENVIRONMENT "QT_QPA_PLATFORM=wayland"
    )
    bloom_add_test(
        NAME bloom.ui.viewer_gpu_presenter_native
        COMMAND bloom_ui_viewer_gpu_presenter_native_test ${bloom_ui_vulkan_loader_args}
        LABELS ui integration gpu presentation TIMEOUT 300
        ENVIRONMENT "QT_QPA_PLATFORM=wayland"
    )
    # The five fixtures above share the pre-QApplication native Wayland environment gate and exit
    # with 77 when it is unavailable; register that code as a skip so CTest lists them as Skipped.
    set_tests_properties(
        bloom.ui.gpu_presentation_coordinator
        bloom.ui.gpu_preview_display_service_presentation
        bloom.ui.gpu_preview_display_service_resident
        bloom.ui.viewer_gpu_presenter
        bloom.ui.viewer_gpu_presenter_native
        PROPERTIES SKIP_RETURN_CODE 77
    )
endif()

# Host retire-before-mutation gate. The first target is a CPU-only, archive-free test of the
# asynchronous gate (including the late-callback-after-gate-destruction lifetime path). The second
# is a focused Qt host fixture that drives the real EditorArea/WorkspaceHost gates with a delayed
# fake native-surface editor; it links the host TUs and therefore exercises the actual integration.
add_executable(bloom_ui_editor_native_surface_gate_test
    editor_native_surface_gate_tests.cpp)
target_link_libraries(bloom_ui_editor_native_surface_gate_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_ui_editor_native_surface_gate_test)
bloom_add_test(
    NAME bloom.ui.editor_native_surface_gate
    COMMAND bloom_ui_editor_native_surface_gate_test
    LABELS ui TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

add_executable(bloom_ui_host_native_surface_retirement_test
    host_native_surface_retirement_tests.cpp)
target_link_libraries(bloom_ui_host_native_surface_retirement_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_ui_host_native_surface_retirement_test)
bloom_add_test(
    NAME bloom.ui.host_native_surface_retirement
    COMMAND bloom_ui_host_native_surface_retirement_test
    LABELS ui TIMEOUT 120
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# GPU-resident preview product: the RAM preview cache's fourth-arm retention and invalidation
# contract. The fixture produces a genuine Solid -> resident-display -> owner-registry lease with
# the real product factory on a device; without one the test skips cleanly. The supervisor supplies
# --loader/--require-device for a native run.
add_executable(bloom_ui_preview_frame_cache_resident_test preview_frame_cache_resident_tests.cpp)
target_link_libraries(bloom_ui_preview_frame_cache_resident_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_ui_preview_frame_cache_resident_test)
bloom_add_test(
    NAME bloom.ui.preview-frame-cache-resident
    COMMAND bloom_ui_preview_frame_cache_resident_test
    LABELS ui unit gpu cache render
    TIMEOUT 300
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# CPU-only edge cases for the RAM preview cache: same-key live reuse (no double charge, generation
# re-stamp) and other-project retention under a standalone no-policy cache, while same-project older
# revisions are still dropped. It builds genuine CPU reference frames and never creates a GPU device,
# so it registers unconditionally and always runs.
add_executable(bloom_ui_preview_frame_cache_cpu_edge_test preview_frame_cache_cpu_edge_tests.cpp)
target_link_libraries(bloom_ui_preview_frame_cache_cpu_edge_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_ui_preview_frame_cache_cpu_edge_test)
bloom_add_test(
    NAME bloom.ui.preview-frame-cache-cpu-edge
    COMMAND bloom_ui_preview_frame_cache_cpu_edge_test
    LABELS ui unit cache
    TIMEOUT 120
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# GPU-resident viewer: the off-UI premultiplied overlay raster and the controller's inert/no-live
# semantics. Device-free and Vulkan-free, so it registers unconditionally and runs offscreen.
add_executable(bloom_ui_viewer_gpu_resident_controller_test viewer_gpu_resident_controller_tests.cpp)
target_link_libraries(bloom_ui_viewer_gpu_resident_controller_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_ui_viewer_gpu_resident_controller_test)
bloom_add_test(
    NAME bloom.ui.viewer_gpu_resident_controller
    COMMAND bloom_ui_viewer_gpu_resident_controller_test
    LABELS ui unit gpu
    TIMEOUT 120
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# The actual ViewerEditor resident integration gate: injects the private port seam and proves the
# unchanged CPU paint path for a non-resident frame. Device-free; the private src/ui port header is
# reached through the module's own source root.
add_executable(bloom_ui_viewer_editor_resident_test viewer_editor_resident_tests.cpp)
target_include_directories(bloom_ui_viewer_editor_resident_test PRIVATE ..)
target_link_libraries(bloom_ui_viewer_editor_resident_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_ui_viewer_editor_resident_test)
bloom_add_test(
    NAME bloom.ui.viewer_editor_resident
    COMMAND bloom_ui_viewer_editor_resident_test
    LABELS ui integration
    TIMEOUT 180
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# Real Wayland device present/ack/resize/resume for the resident controller. Without a packaged
# loader it skips cleanly (no --loader is passed by normal CTest, which stays offscreen); the
# supervisor passes --loader/--require-device under QT_QPA_PLATFORM=wayland for the native run.
add_executable(bloom_ui_viewer_gpu_resident_native_test viewer_gpu_resident_native_tests.cpp)
target_link_libraries(bloom_ui_viewer_gpu_resident_native_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_ui_viewer_gpu_resident_native_test)
bloom_add_test(
    NAME bloom.ui.viewer_gpu_resident_native
    COMMAND bloom_ui_viewer_gpu_resident_native_test
    LABELS ui integration gpu presentation
    TIMEOUT 300
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# The typed app-activation bootstrap helper: budget/cap alignment, additive cache limits, capability
# fallback and session-refreshing media context. Device-free and Vulkan-free.
add_executable(bloom_ui_gpu_viewer_bootstrap_test gpu_viewer_bootstrap_tests.cpp)
target_link_libraries(bloom_ui_gpu_viewer_bootstrap_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_ui_gpu_viewer_bootstrap_test)
bloom_add_test(
    NAME bloom.ui.gpu_viewer_bootstrap
    COMMAND bloom_ui_gpu_viewer_bootstrap_test
    LABELS ui unit gpu
    TIMEOUT 120
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# The resident cache sublimit + LRU eviction regression against the genuine product fixture. It
# creates a device; without a packaged loader it skips cleanly. The supervisor passes
# --loader/--require-device for the native run.
add_executable(bloom_ui_preview_frame_cache_resident_limits_test
    preview_frame_cache_resident_limits_tests.cpp)
target_link_libraries(bloom_ui_preview_frame_cache_resident_limits_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_ui_preview_frame_cache_resident_limits_test)
bloom_add_test(
    NAME bloom.ui.preview-frame-cache-resident-limits
    COMMAND bloom_ui_preview_frame_cache_resident_limits_test
    LABELS ui unit gpu cache
    TIMEOUT 300
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# GPU end-to-end vertical acceptance over a genuine media document. The two service-chain targets
# adopt the borrowed VkInstance through Qt's public QVulkanInstance and write real OpenEXR fixtures,
# so they need both the pinned Vulkan headers and Qt's Vulkan feature plus the dependency prefix's
# OpenEXR/Imath, and register only when this build has them. Normal CTest runs use
# QT_QPA_PLATFORM=offscreen; the supervisor passes
# --loader/--require-device for the real native gate, and --require-device fails closed.
if(BLOOM_UI_VULKAN_AVAILABLE)
    bloom_find_dependency(OpenEXR)

    add_executable(bloom_ui_gpu_resident_service_chain_test
        gpu_resident_service_chain_tests.cpp
        gpu_service_chain_fixture.hpp
        gpu_vertical_proof_support.hpp)
    target_include_directories(bloom_ui_gpu_resident_service_chain_test
        PRIVATE "${PROJECT_SOURCE_DIR}/src/runtime" ${OpenEXR_INCLUDE_DIRS} ${Imath_INCLUDE_DIRS})
    target_link_libraries(bloom_ui_gpu_resident_service_chain_test
        PRIVATE bloom_ui bloom_runtime_evaluator Vulkan::Headers OpenEXR::OpenEXR)
    bloom_enable_warnings(bloom_ui_gpu_resident_service_chain_test)
    bloom_enable_strict_floating_point(bloom_ui_gpu_resident_service_chain_test)
    bloom_add_test(
        NAME bloom.ui.gpu_resident_service_chain
        COMMAND bloom_ui_gpu_resident_service_chain_test
        LABELS ui gpu resident media
        TIMEOUT 300
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(bloom_ui_gpu_viewer_service_chain_test
        gpu_viewer_service_chain_tests.cpp
        gpu_service_chain_fixture.hpp
        gpu_vertical_proof_support.hpp)
    target_include_directories(bloom_ui_gpu_viewer_service_chain_test
        PRIVATE "${PROJECT_SOURCE_DIR}/src/runtime" ${OpenEXR_INCLUDE_DIRS} ${Imath_INCLUDE_DIRS})
    target_link_libraries(bloom_ui_gpu_viewer_service_chain_test
        PRIVATE bloom_ui bloom_runtime_evaluator Vulkan::Headers OpenEXR::OpenEXR)
    bloom_enable_warnings(bloom_ui_gpu_viewer_service_chain_test)
    bloom_enable_strict_floating_point(bloom_ui_gpu_viewer_service_chain_test)
    if(DEFINED BLOOM_DEPENDENCY_PREFIX AND EXISTS "${BLOOM_DEPENDENCY_PREFIX}/lib/libvulkan.so.1")
        set(bloom_ui_gpu_viewer_service_chain_loader_args
            --loader "${BLOOM_DEPENDENCY_PREFIX}/lib/libvulkan.so.1")
    else()
        set(bloom_ui_gpu_viewer_service_chain_loader_args)
    endif()
    bloom_add_test(
        NAME bloom.ui.gpu_viewer_service_chain
        COMMAND bloom_ui_gpu_viewer_service_chain_test
                ${bloom_ui_gpu_viewer_service_chain_loader_args}
        LABELS ui gpu viewer resident
        TIMEOUT 300
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )
endif()

# Device-free ViewerEditor view-coordinate oracle: fit / zoom-about-cursor / pan pinned against an
# independent CPU reference built from the production fitDisplayRect()/actualPixelRect() primitives.
add_executable(bloom_ui_gpu_viewer_end_to_end_test gpu_viewer_end_to_end_tests.cpp)
target_link_libraries(bloom_ui_gpu_viewer_end_to_end_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_ui_gpu_viewer_end_to_end_test)
bloom_enable_strict_floating_point(bloom_ui_gpu_viewer_end_to_end_test)
bloom_add_test(
    NAME bloom.ui.gpu_viewer_end_to_end
    COMMAND bloom_ui_gpu_viewer_end_to_end_test
    LABELS ui gpu viewer
    TIMEOUT 120
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)
