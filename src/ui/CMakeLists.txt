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
        kit/dropdown.cpp
        kit/dropdown_popup.cpp
        kit/fonts.cpp
        kit/icons.cpp
        kit/painting.cpp
        kit/radio_group.cpp
        kit/range_selector.cpp
        kit/slider.cpp
        kit/switch_control.cpp
        kit/theme.cpp
        kit/tokens.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bloom/ui/kit/button.hpp
            include/bloom/ui/kit/dropdown.hpp
            include/bloom/ui/kit/dropdown_popup.hpp
            include/bloom/ui/kit/fonts.hpp
            include/bloom/ui/kit/icons.hpp
            include/bloom/ui/kit/painting.hpp
            include/bloom/ui/kit/radio_group.hpp
            include/bloom/ui/kit/range_selector.hpp
            include/bloom/ui/kit/slider.hpp
            include/bloom/ui/kit/switch_control.hpp
            include/bloom/ui/kit/theme.hpp
            include/bloom/ui/kit/tokens.hpp
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
        kit/third_party/phosphor-icons/regular/caret-down.svg
        kit/third_party/phosphor-icons/regular/caret-left.svg
        kit/third_party/phosphor-icons/regular/caret-right.svg
        kit/third_party/phosphor-icons/regular/caret-up.svg
        kit/third_party/phosphor-icons/regular/check.svg
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
        kit/third_party/phosphor-icons/regular/hand-grabbing.svg
        kit/third_party/phosphor-icons/regular/hand.svg
        kit/third_party/phosphor-icons/regular/image.svg
        kit/third_party/phosphor-icons/regular/info.svg
        kit/third_party/phosphor-icons/regular/link-simple.svg
        kit/third_party/phosphor-icons/regular/list.svg
        kit/third_party/phosphor-icons/regular/lock-simple-open.svg
        kit/third_party/phosphor-icons/regular/lock-simple.svg
        kit/third_party/phosphor-icons/regular/magnifying-glass.svg
        kit/third_party/phosphor-icons/regular/minus.svg
        kit/third_party/phosphor-icons/regular/music-notes.svg
        kit/third_party/phosphor-icons/regular/pause.svg
        kit/third_party/phosphor-icons/regular/play.svg
        kit/third_party/phosphor-icons/regular/plus.svg
        kit/third_party/phosphor-icons/regular/repeat.svg
        kit/third_party/phosphor-icons/regular/skip-back.svg
        kit/third_party/phosphor-icons/regular/skip-forward.svg
        kit/third_party/phosphor-icons/regular/speaker-simple-high.svg
        kit/third_party/phosphor-icons/regular/speaker-simple-slash.svg
        kit/third_party/phosphor-icons/regular/square-split-horizontal.svg
        kit/third_party/phosphor-icons/regular/square-split-vertical.svg
        kit/third_party/phosphor-icons/regular/text-t.svg
        kit/third_party/phosphor-icons/regular/trash.svg
        kit/third_party/phosphor-icons/regular/warning-circle.svg
        kit/third_party/phosphor-icons/regular/warning.svg
        kit/third_party/phosphor-icons/regular/x.svg
        kit/third_party/phosphor-icons/fill/caret-down-fill.svg
        kit/third_party/phosphor-icons/fill/caret-left-fill.svg
        kit/third_party/phosphor-icons/fill/caret-right-fill.svg
        kit/third_party/phosphor-icons/fill/caret-up-fill.svg
        kit/third_party/phosphor-icons/fill/check-fill.svg
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
        kit/third_party/phosphor-icons/fill/hand-fill.svg
        kit/third_party/phosphor-icons/fill/hand-grabbing-fill.svg
        kit/third_party/phosphor-icons/fill/image-fill.svg
        kit/third_party/phosphor-icons/fill/info-fill.svg
        kit/third_party/phosphor-icons/fill/link-simple-fill.svg
        kit/third_party/phosphor-icons/fill/list-fill.svg
        kit/third_party/phosphor-icons/fill/lock-simple-fill.svg
        kit/third_party/phosphor-icons/fill/lock-simple-open-fill.svg
        kit/third_party/phosphor-icons/fill/magnifying-glass-fill.svg
        kit/third_party/phosphor-icons/fill/minus-fill.svg
        kit/third_party/phosphor-icons/fill/music-notes-fill.svg
        kit/third_party/phosphor-icons/fill/pause-fill.svg
        kit/third_party/phosphor-icons/fill/play-fill.svg
        kit/third_party/phosphor-icons/fill/plus-fill.svg
        kit/third_party/phosphor-icons/fill/repeat-fill.svg
        kit/third_party/phosphor-icons/fill/skip-back-fill.svg
        kit/third_party/phosphor-icons/fill/skip-forward-fill.svg
        kit/third_party/phosphor-icons/fill/speaker-simple-high-fill.svg
        kit/third_party/phosphor-icons/fill/speaker-simple-slash-fill.svg
        kit/third_party/phosphor-icons/fill/square-split-horizontal-fill.svg
        kit/third_party/phosphor-icons/fill/square-split-vertical-fill.svg
        kit/third_party/phosphor-icons/fill/text-t-fill.svg
        kit/third_party/phosphor-icons/fill/trash-fill.svg
        kit/third_party/phosphor-icons/fill/warning-circle-fill.svg
        kit/third_party/phosphor-icons/fill/warning-fill.svg
        kit/third_party/phosphor-icons/fill/x-fill.svg
)


# The bundled interface faces, enumerated the same way and under the same Bloom-owned prefix. The
# shipped weight set is deliberately minimal (visual-language.md): Regular/Medium/SemiBold for the
# interface family and Regular/Medium for the monospaced one -- a weight ships when a component
# uses it, not before.
qt_add_resources(bloom_ui_kit "kinetik_fonts"
    PREFIX "/bloom/kit"
    BASE "kit/third_party"
    FILES
        kit/third_party/geist-mono/GeistMono-Medium.ttf
        kit/third_party/geist-mono/GeistMono-Regular.ttf
        kit/third_party/plus-jakarta-sans/PlusJakartaSans-Medium.ttf
        kit/third_party/plus-jakarta-sans/PlusJakartaSans-Regular.ttf
        kit/third_party/plus-jakarta-sans/PlusJakartaSans-SemiBold.ttf
)

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
