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
        kit/button.cpp
        kit/color.cpp
        kit/color_chip.cpp
        kit/color_picker.cpp
        kit/color_sampler.cpp
        kit/color_swatches.cpp
        kit/dropdown.cpp
        kit/search_popup.cpp
        kit/section.cpp
        kit/dropdown_popup.cpp
        kit/fonts.cpp
        kit/icons.cpp
        kit/mnemonic_style.cpp
        kit/painting.cpp
        kit/controls.cpp
        kit/row.cpp
        kit/radio_group.cpp
        kit/range_selector.cpp
        kit/slider.cpp
        kit/switch_control.cpp
        kit/theme.cpp
        kit/title_bar.cpp
        kit/tokens.cpp
        kit/value_field.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bloom/ui/kit/button.hpp
            include/bloom/ui/kit/color.hpp
            include/bloom/ui/kit/color_chip.hpp
            include/bloom/ui/kit/color_picker.hpp
            include/bloom/ui/kit/color_sampler.hpp
            include/bloom/ui/kit/color_swatches.hpp
            include/bloom/ui/kit/dropdown.hpp
            include/bloom/ui/kit/search_popup.hpp
            include/bloom/ui/kit/section.hpp
            include/bloom/ui/kit/dropdown_popup.hpp
            include/bloom/ui/kit/fonts.hpp
            include/bloom/ui/kit/icons.hpp
            include/bloom/ui/kit/mnemonic_style.hpp
            include/bloom/ui/kit/painting.hpp
            include/bloom/ui/kit/panel_switcher.hpp
            include/bloom/ui/kit/controls.hpp
            include/bloom/ui/kit/row.hpp
            include/bloom/ui/kit/radio_group.hpp
            include/bloom/ui/kit/range_selector.hpp
            include/bloom/ui/kit/slider.hpp
            include/bloom/ui/kit/switch_control.hpp
            include/bloom/ui/kit/theme.hpp
            include/bloom/ui/kit/title_bar.hpp
            include/bloom/ui/kit/tokens.hpp
            include/bloom/ui/kit/value_field.hpp
)

target_compile_features(bloom_ui_kit PUBLIC cxx_std_20)
# Qt6::Svg is what renders the vendored Phosphor SVGs (QSvgRenderer). PUBLIC on Widgets because
# every kit header exposes Qt widget/gui types; PRIVATE on Svg because no kit header does -- icons.
# hpp hands back QPixmap/QIcon, never a renderer.
target_link_libraries(bloom_ui_kit PUBLIC Qt6::Widgets PRIVATE Qt6::Svg)
bloom_enable_warnings(bloom_ui_kit)

# The first Bloom Qt resource pack (ADR 0010): every vendored asset enumerated explicitly, never a
# glob and never a Git submodule or a configure-time download. BASE strips the vendoring prefix so
# the runtime path is the Bloom-owned "/bloom/kit/phosphor-icons/<weight>/<name>.svg"; the typed
# IconId API in include/bloom/ui/kit/icons.hpp is the only thing that ever spells one.
#
# qt_add_resources() on a static library routes the generated resource translation unit through its
# own object library and propagates the objects to consumers, so the icon data is never dropped by
# static-library dead-stripping the way a plain Q_INIT_RESOURCE would risk.
qt_add_resources(bloom_ui_kit "kinetik_icons"
    PREFIX "/bloom/kit"
    BASE "kit/third_party"
    FILES
        kit/third_party/phosphor-icons/bold/arrow-counter-clockwise-bold.svg
        kit/third_party/phosphor-icons/bold/arrow-square-out-bold.svg
        kit/third_party/phosphor-icons/bold/dots-six-vertical-bold.svg
        kit/third_party/phosphor-icons/bold/text-align-center-bold.svg
        kit/third_party/phosphor-icons/bold/text-align-left-bold.svg
        kit/third_party/phosphor-icons/bold/text-align-right-bold.svg
        kit/third_party/phosphor-icons/fill/arrow-counter-clockwise-fill.svg
        kit/third_party/phosphor-icons/fill/arrow-square-out-fill.svg
        kit/third_party/phosphor-icons/fill/dots-six-vertical-fill.svg
        kit/third_party/phosphor-icons/fill/text-align-center-fill.svg
        kit/third_party/phosphor-icons/fill/text-align-left-fill.svg
        kit/third_party/phosphor-icons/fill/text-align-right-fill.svg
        kit/third_party/phosphor-icons/regular/arrow-counter-clockwise.svg
        kit/third_party/phosphor-icons/regular/arrow-square-out.svg
        kit/third_party/phosphor-icons/regular/dots-six-vertical.svg
        kit/third_party/phosphor-icons/regular/text-align-center.svg
        kit/third_party/phosphor-icons/regular/text-align-left.svg
        kit/third_party/phosphor-icons/regular/text-align-right.svg
        kit/third_party/phosphor-icons/regular/caret-down.svg
        kit/third_party/phosphor-icons/regular/circle.svg
        kit/third_party/phosphor-icons/regular/caret-left.svg
        kit/third_party/phosphor-icons/regular/caret-right.svg
        kit/third_party/phosphor-icons/regular/caret-up-down.svg
        kit/third_party/phosphor-icons/regular/caret-up.svg
        kit/third_party/phosphor-icons/regular/check.svg
        kit/third_party/phosphor-icons/regular/clock.svg
        kit/third_party/phosphor-icons/regular/corners-in.svg
        kit/third_party/phosphor-icons/regular/corners-out.svg
        kit/third_party/phosphor-icons/regular/cube.svg
        kit/third_party/phosphor-icons/regular/cursor.svg
        kit/third_party/phosphor-icons/regular/diamond.svg
        kit/third_party/phosphor-icons/regular/dots-three-vertical.svg
        kit/third_party/phosphor-icons/regular/eye-slash.svg
        kit/third_party/phosphor-icons/regular/eye.svg
        kit/third_party/phosphor-icons/regular/film-slate.svg
        kit/third_party/phosphor-icons/regular/film-strip.svg
        kit/third_party/phosphor-icons/regular/folder.svg
        kit/third_party/phosphor-icons/regular/gear.svg
        kit/third_party/phosphor-icons/regular/graph.svg
        kit/third_party/phosphor-icons/regular/hand-grabbing.svg
        kit/third_party/phosphor-icons/regular/hand.svg
        kit/third_party/phosphor-icons/regular/image.svg
        kit/third_party/phosphor-icons/regular/info.svg
        kit/third_party/phosphor-icons/regular/link-simple.svg
        kit/third_party/phosphor-icons/regular/list.svg
        kit/third_party/phosphor-icons/regular/lock-simple-open.svg
        kit/third_party/phosphor-icons/regular/lock-simple.svg
        kit/third_party/phosphor-icons/regular/magnifying-glass.svg
        kit/third_party/phosphor-icons/regular/magnet.svg
        kit/third_party/phosphor-icons/regular/minus.svg
        kit/third_party/phosphor-icons/regular/music-notes.svg
        kit/third_party/phosphor-icons/regular/pause.svg
        kit/third_party/phosphor-icons/regular/play.svg
        kit/third_party/phosphor-icons/regular/plus.svg
        kit/third_party/phosphor-icons/regular/repeat.svg
        kit/third_party/phosphor-icons/regular/skip-back.svg
        kit/third_party/phosphor-icons/regular/skip-forward.svg
        kit/third_party/phosphor-icons/regular/sliders-horizontal.svg
        kit/third_party/phosphor-icons/regular/speaker-simple-high.svg
        kit/third_party/phosphor-icons/regular/speaker-simple-slash.svg
        kit/third_party/phosphor-icons/regular/square-split-horizontal.svg
        kit/third_party/phosphor-icons/regular/square-split-vertical.svg
        kit/third_party/phosphor-icons/regular/stack.svg
        kit/third_party/phosphor-icons/regular/text-t.svg
        kit/third_party/phosphor-icons/regular/trash.svg
        kit/third_party/phosphor-icons/regular/warning-circle.svg
        kit/third_party/phosphor-icons/regular/warning.svg
        kit/third_party/phosphor-icons/regular/x.svg
        kit/third_party/phosphor-icons/bold/caret-down-bold.svg
        kit/third_party/phosphor-icons/bold/circle-bold.svg
        kit/third_party/phosphor-icons/bold/caret-left-bold.svg
        kit/third_party/phosphor-icons/bold/caret-right-bold.svg
        kit/third_party/phosphor-icons/bold/caret-up-bold.svg
        kit/third_party/phosphor-icons/bold/caret-up-down-bold.svg
        kit/third_party/phosphor-icons/bold/check-bold.svg
        kit/third_party/phosphor-icons/bold/clock-bold.svg
        kit/third_party/phosphor-icons/bold/corners-in-bold.svg
        kit/third_party/phosphor-icons/bold/corners-out-bold.svg
        kit/third_party/phosphor-icons/bold/cube-bold.svg
        kit/third_party/phosphor-icons/bold/cursor-bold.svg
        kit/third_party/phosphor-icons/bold/diamond-bold.svg
        kit/third_party/phosphor-icons/bold/dots-three-vertical-bold.svg
        kit/third_party/phosphor-icons/bold/eye-bold.svg
        kit/third_party/phosphor-icons/bold/eye-slash-bold.svg
        kit/third_party/phosphor-icons/bold/film-slate-bold.svg
        kit/third_party/phosphor-icons/bold/film-strip-bold.svg
        kit/third_party/phosphor-icons/bold/folder-bold.svg
        kit/third_party/phosphor-icons/bold/gear-bold.svg
        kit/third_party/phosphor-icons/bold/graph-bold.svg
        kit/third_party/phosphor-icons/bold/hand-bold.svg
        kit/third_party/phosphor-icons/bold/hand-grabbing-bold.svg
        kit/third_party/phosphor-icons/bold/image-bold.svg
        kit/third_party/phosphor-icons/bold/info-bold.svg
        kit/third_party/phosphor-icons/bold/link-simple-bold.svg
        kit/third_party/phosphor-icons/bold/list-bold.svg
        kit/third_party/phosphor-icons/bold/lock-simple-bold.svg
        kit/third_party/phosphor-icons/bold/lock-simple-open-bold.svg
        kit/third_party/phosphor-icons/bold/magnifying-glass-bold.svg
        kit/third_party/phosphor-icons/bold/magnet-bold.svg
        kit/third_party/phosphor-icons/bold/minus-bold.svg
        kit/third_party/phosphor-icons/bold/music-notes-bold.svg
        kit/third_party/phosphor-icons/bold/pause-bold.svg
        kit/third_party/phosphor-icons/bold/play-bold.svg
        kit/third_party/phosphor-icons/bold/plus-bold.svg
        kit/third_party/phosphor-icons/bold/repeat-bold.svg
        kit/third_party/phosphor-icons/bold/skip-back-bold.svg
        kit/third_party/phosphor-icons/bold/skip-forward-bold.svg
        kit/third_party/phosphor-icons/bold/sliders-horizontal-bold.svg
        kit/third_party/phosphor-icons/bold/speaker-simple-high-bold.svg
        kit/third_party/phosphor-icons/bold/speaker-simple-slash-bold.svg
        kit/third_party/phosphor-icons/bold/square-split-horizontal-bold.svg
        kit/third_party/phosphor-icons/bold/square-split-vertical-bold.svg
        kit/third_party/phosphor-icons/bold/stack-bold.svg
        kit/third_party/phosphor-icons/bold/text-t-bold.svg
        kit/third_party/phosphor-icons/bold/trash-bold.svg
        kit/third_party/phosphor-icons/bold/warning-bold.svg
        kit/third_party/phosphor-icons/bold/warning-circle-bold.svg
        kit/third_party/phosphor-icons/bold/x-bold.svg
        kit/third_party/phosphor-icons/fill/caret-down-fill.svg
        kit/third_party/phosphor-icons/fill/circle-fill.svg
        kit/third_party/phosphor-icons/fill/caret-left-fill.svg
        kit/third_party/phosphor-icons/fill/caret-right-fill.svg
        kit/third_party/phosphor-icons/fill/caret-up-down-fill.svg
        kit/third_party/phosphor-icons/fill/caret-up-fill.svg
        kit/third_party/phosphor-icons/fill/check-fill.svg
        kit/third_party/phosphor-icons/fill/clock-fill.svg
        kit/third_party/phosphor-icons/fill/corners-in-fill.svg
        kit/third_party/phosphor-icons/fill/corners-out-fill.svg
        kit/third_party/phosphor-icons/fill/cube-fill.svg
        kit/third_party/phosphor-icons/fill/cursor-fill.svg
        kit/third_party/phosphor-icons/fill/diamond-fill.svg
        kit/third_party/phosphor-icons/fill/dots-three-vertical-fill.svg
        kit/third_party/phosphor-icons/fill/eye-fill.svg
        kit/third_party/phosphor-icons/fill/eye-slash-fill.svg
        kit/third_party/phosphor-icons/fill/film-slate-fill.svg
        kit/third_party/phosphor-icons/fill/film-strip-fill.svg
        kit/third_party/phosphor-icons/fill/folder-fill.svg
        kit/third_party/phosphor-icons/fill/gear-fill.svg
        kit/third_party/phosphor-icons/fill/graph-fill.svg
        kit/third_party/phosphor-icons/fill/hand-fill.svg
        kit/third_party/phosphor-icons/fill/hand-grabbing-fill.svg
        kit/third_party/phosphor-icons/fill/image-fill.svg
        kit/third_party/phosphor-icons/fill/info-fill.svg
        kit/third_party/phosphor-icons/fill/link-simple-fill.svg
        kit/third_party/phosphor-icons/fill/list-fill.svg
        kit/third_party/phosphor-icons/fill/lock-simple-fill.svg
        kit/third_party/phosphor-icons/fill/lock-simple-open-fill.svg
        kit/third_party/phosphor-icons/fill/magnifying-glass-fill.svg
        kit/third_party/phosphor-icons/fill/magnet-fill.svg
        kit/third_party/phosphor-icons/fill/minus-fill.svg
        kit/third_party/phosphor-icons/fill/music-notes-fill.svg
        kit/third_party/phosphor-icons/fill/pause-fill.svg
        kit/third_party/phosphor-icons/fill/play-fill.svg
        kit/third_party/phosphor-icons/fill/plus-fill.svg
        kit/third_party/phosphor-icons/fill/repeat-fill.svg
        kit/third_party/phosphor-icons/fill/skip-back-fill.svg
        kit/third_party/phosphor-icons/fill/skip-forward-fill.svg
        kit/third_party/phosphor-icons/fill/sliders-horizontal-fill.svg
        kit/third_party/phosphor-icons/fill/speaker-simple-high-fill.svg
        kit/third_party/phosphor-icons/fill/speaker-simple-slash-fill.svg
        kit/third_party/phosphor-icons/fill/square-split-horizontal-fill.svg
        kit/third_party/phosphor-icons/fill/square-split-vertical-fill.svg
        kit/third_party/phosphor-icons/fill/stack-fill.svg
        kit/third_party/phosphor-icons/fill/text-t-fill.svg
        kit/third_party/phosphor-icons/fill/trash-fill.svg
        kit/third_party/phosphor-icons/fill/warning-circle-fill.svg
        kit/third_party/phosphor-icons/fill/warning-fill.svg
        kit/third_party/phosphor-icons/fill/x-fill.svg
)


# The bundled interface faces, enumerated the same way and under the same Bloom-owned prefix. The
# shipped set is deliberately minimal (visual-language.md): Book/Bold/Oblique for the interface
# family -- one family, three styles, unlike the per-weight families the previous interface face
# shipped -- and Regular/Medium for the monospaced one.
qt_add_resources(bloom_ui_kit "kinetik_fonts"
    PREFIX "/bloom/kit"
    BASE "kit/third_party"
    FILES
        kit/third_party/inter/LICENSE
        kit/third_party/inter/Inter-Regular.ttf
        kit/third_party/inter/Inter-Medium.ttf
        kit/third_party/inter/Inter-SemiBold.ttf
        kit/third_party/inter/manifest.json
        kit/third_party/geist-mono/GeistMono-Medium.ttf
        kit/third_party/geist-mono/GeistMono-Regular.ttf
)

# Generates the "Open Source Licenses..." embedded catalog (task U2, issue #118, decision 3) BEFORE
# bloom_ui's sources are declared, so BLOOM_THIRD_PARTY_LICENSE_CATALOG_CPP below names a file that
# already exists at generate time.
include("${CMAKE_CURRENT_SOURCE_DIR}/generate_third_party_license_catalog.cmake")

add_library(bloom_ui STATIC)

target_sources(
    bloom_ui
    PRIVATE
        background_preview_controller.cpp
        application_shutdown_coordinator.cpp
        composition_authoring.cpp
        composition_editor_support.cpp
        composition_plan_cache.cpp
        composition_preview_controller.cpp
        composition_preview_pipeline.cpp
        composition_preview_playback.cpp
        composition_preview_result.cpp
        composition_session.cpp
        composition_session_keyframes.cpp
        editor_area.cpp
        editor_chrome.cpp
        editor_registry.cpp
        frame_export_controller.cpp
        frameless_window_support.cpp
        jobs_editor.cpp
        licenses_window.cpp
        main_window.cpp
        assets_editor.cpp
        node_editor.cpp
        node_editor_canvas.cpp
        node_editor_items.cpp
        node_editor_interactions.cpp
        node_editor_add.cpp
        node_editor_menus.cpp
        playback_controller.cpp
        preview_frame_cache.cpp
        project_host.cpp
        ram_preview_controller.cpp
        properties_editor.cpp
        properties_editor_sections.cpp
        properties_editor_sources.cpp
        properties_registry_row.cpp
        properties_editor_registry.cpp
        properties_upstream.cpp
        properties_driven_values.cpp
        properties_driven_rows.cpp
        composition_parameter_vocabulary.cpp
        composition_driver_probe.cpp
        properties_anchor_grid.cpp
        properties_sections.cpp
        qualified_display_processor_bootstrap.cpp
        task_monitor_model.cpp
        task_ui_bridge.cpp
        timeline_editor.cpp
        timeline_property_rows.cpp
        timeline_editor_menus.cpp
        timeline_frame_math.cpp
        timeline_ruler.cpp
        timeline_keyframe_gestures.cpp
        timeline_work_area.cpp
        timeline_navigation.cpp
        composition_commands.cpp
        viewer_overlays.cpp
        viewer_editor.cpp
        window_status_bar.cpp
        workspace_host.cpp
        "${BLOOM_THIRD_PARTY_LICENSE_CATALOG_CPP}"
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bloom/ui/background_preview_controller.hpp
            include/bloom/ui/application_shutdown_coordinator.hpp
            include/bloom/ui/composition_authoring.hpp
            include/bloom/ui/composition_editors.hpp
            include/bloom/ui/composition_plan_cache.hpp
            include/bloom/ui/composition_preview_controller.hpp
            include/bloom/ui/composition_preview_pipeline.hpp
            include/bloom/ui/composition_session.hpp
            include/bloom/ui/editor_area.hpp
            include/bloom/ui/editor_registry.hpp
            include/bloom/ui/frame_export_controller.hpp
            include/bloom/ui/frameless_window_support.hpp
            include/bloom/ui/jobs_editor.hpp
            include/bloom/ui/licenses_window.hpp
            include/bloom/ui/main_window.hpp
            include/bloom/ui/assets_editor.hpp
            include/bloom/ui/node_editor.hpp
            include/bloom/ui/playback_controller.hpp
            include/bloom/ui/preview_frame_cache.hpp
            include/bloom/ui/project_host.hpp
            include/bloom/ui/ram_preview_controller.hpp
            include/bloom/ui/properties_editor.hpp
            include/bloom/ui/qualified_display_processor_bootstrap.hpp
            include/bloom/ui/task_monitor_model.hpp
            include/bloom/ui/task_ui_bridge.hpp
            include/bloom/ui/third_party_license_catalog.hpp
            include/bloom/ui/timeline_editor.hpp
            include/bloom/ui/timeline_frame_math.hpp
            include/bloom/ui/timeline_ruler.hpp
            include/bloom/ui/composition_commands.hpp
            include/bloom/ui/viewer_overlays.hpp
            include/bloom/ui/viewer_editor.hpp
            include/bloom/ui/window_status_bar.hpp
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
