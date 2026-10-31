// The graph editor's interaction contract. Every assertion here is about the DOCUMENT or about a
// number the view derives from the sampler -- never about a rasterized pixel -- except the one
// case whose whole claim is that nothing is drawn.

#include <timeline_graph_math.hpp>
#include <timeline_keyframe_time.hpp>

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/animation.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/project.hpp>
#include <bloom/runtime/animation_sampling.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/curve_compilation.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/radio_group.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/timeline_editor.hpp>
#include <bloom/ui/timeline_frame_math.hpp>
#include <bloom/ui/timeline_graph_view.hpp>
#include <bloom/ui/timeline_ruler.hpp>

#include <QApplication>
#include <QContextMenuEvent>
#include <QElapsedTimer>
#include <QImage>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QWheelEvent>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace bloom;
using namespace std::chrono_literals;

int failures = 0;

void expect(const bool condition, const std::string_view message,
            const std::source_location location = std::source_location::current()) {
    if (condition)
        return;
    ++failures;
    std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
}

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "graph view fixture failure: " << message << '\n';
    std::exit(1);
}

[[nodiscard]] core::RationalTime time(const std::int64_t numerator,
                                      const std::int64_t denominator = 1) {
    const auto value = core::RationalTime::create(numerator, denominator);
    if (!value.has_value())
        fail("test time must be valid");
    return *value;
}

runtime::TaskSchedulerConfig testSchedulerConfig() {
    return {.cpuWorkerCount = 1,
            .blockingIoWorkerCount = 1,
            .cpuQueueCapacity = 16,
            .blockingIoQueueCapacity = 4,
            .terminalHistoryCapacity = 32,
            .diagnosticsPerTask = 8,
            .groupRegistryCapacity = 8};
}

struct PipelineFixture final {
    runtime::NodeDefinitionRegistry definitions;
    runtime::SnapshotCompiler compiler;
    runtime::CpuCompositionEvaluator evaluator;
    runtime::CpuReferenceDisplayPreparer displayPreparer;
    runtime::QualifiedDisplayProcessorProvider qualifiedProcessorProvider;
    ui::PreviewPreparationFunction pipeline;

    PipelineFixture() : compiler(definitions) {
        if (!runtime::registerBuiltInNodeDefinitions(definitions))
            fail("node definitions must register");
        definitions.freeze();
        pipeline = ui::makeCompositionPreviewPipeline(compiler, evaluator, displayPreparer,
                                                      qualifiedProcessorProvider);
    }
};

struct SessionFixture final {
    document::Document document;
    commands::CommandStack commands;
    ui::CompositionSession session;
    runtime::TaskScheduler scheduler;
    ui::TaskUiBridge bridge;
    PipelineFixture pipeline;
    ui::CompositionPreviewController controller;

    explicit SessionFixture(document::NewProject newProject)
        : document(std::move(newProject.project)), commands(document),
          session(document, commands, newProject.initialCompositionId),
          scheduler(testSchedulerConfig()), bridge(scheduler, nullptr, 1ms),
          controller(session, scheduler, bridge, pipeline.pipeline) {}
};

// The graph type and Normalize persist; every fixture starts from the defaults, before the editor
// (and with it the graph view) reads them.
void resetGraphSettings() {
    QSettings settings;
    settings.remove(QStringLiteral("timeline/graph-type"));
    settings.remove(QStringLiteral("timeline/graph-normalize"));
}

// A timeline with its one layer expanded and the graph editor on, plus the handles a test needs
// to drive it. Built through the real editor rather than a bare view, because "the graph replaces
// the lanes" is part of what is under test.
struct GraphFixture final {
    SessionFixture session;
    ui::TimelineEditor editor;
    ui::TimelineGraphView* view = nullptr;

    explicit GraphFixture(document::NewProject project)
        : session((resetGraphSettings(), std::move(project))),
          editor(session.session, session.controller) {
        editor.resize(1600, 700);
        editor.show();
        QCoreApplication::processEvents();
    }

    void expandAndShowGraph() {
        // The layer and its keys are authored after the editor exists, exactly as an artist would;
        // let the editor rebuild its rows before looking for them.
        QCoreApplication::processEvents();
        auto* graph = editor.findChild<QAction*>("timelineGraphEditorAction");
        if (graph == nullptr)
            fail("the graph toggle must exist");
        editor.findChild<QAction*>("timelineKeyframesAction")->setChecked(true);
        const auto axis = editor.rulerForTest()->axisForWidth(editor.laneRegionForTest()->width());
        if (!axis.has_value())
            fail("the lane axis must resolve");
        // Clicking a collapsed layer's summary glyph expands it, which is what creates the
        // parameter rows the graph derives its curves from.
        const QPointF point(axis->pixelForTime(time(1)), ui::kTimelineRowHeight / 2.0);
        for (const auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
            QMouseEvent event(
                type, point, editor.laneRegionForTest()->mapToGlobal(point), Qt::LeftButton,
                type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(editor.laneRegionForTest(), &event);
        }
        QCoreApplication::processEvents();
        graph->setChecked(true);
        QCoreApplication::processEvents();
        view = editor.laneRegionForTest()->graphViewForTest();
        if (view == nullptr)
            fail("the graph view must exist");
        if (view->curves().empty()) {
            std::string summary;
            for (const auto at : editor.laneRegionForTest()->keySummaryTimes(0))
                summary += std::to_string(at.toSeconds()) + " ";
            fail("the graph view must carry at least one curve; rows=" +
                 std::to_string(editor.layerStackForTest()->rowCount()) +
                 " laneWidth=" + std::to_string(editor.laneRegionForTest()->width()) +
                 " summary=[" + summary + "]");
        }
    }

    void mouse(const QEvent::Type type, const QPointF point,
               const Qt::KeyboardModifiers modifiers = Qt::NoModifier,
               const Qt::MouseButton button = Qt::LeftButton) {
        QMouseEvent event(type, point, view->mapToGlobal(point), button,
                          type == QEvent::MouseButtonRelease ? Qt::NoButton : button, modifiers);
        QCoreApplication::sendEvent(view, &event);
    }

    void drag(const QPointF from, const QPointF to,
              const Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        mouse(QEvent::MouseButtonPress, from, modifiers);
        mouse(QEvent::MouseMove, to, modifiers);
        mouse(QEvent::MouseButtonRelease, to, modifiers);
        QCoreApplication::processEvents();
    }
};

[[nodiscard]] document::NewProject makeProject(std::string name) {
    const auto format = document::CompositionFormat::create(4, 3);
    if (!format.has_value())
        fail("the fixture format must be valid");
    return document::makeNewProject(std::move(name), "Main", time(10), *format);
}

[[nodiscard]] const document::ScalarAnimationCurve*
scalarCurve(const ui::CompositionSession& session, const document::AnimationCurveId id) {
    return session.composition()->animationCurves().findScalar(id);
}

[[nodiscard]] document::AnimationCurveId seedOpacity(ui::CompositionSession& session) {
    if (!session.addSolidLayer(QStringLiteral("Curves"), {0.2, 0.3, 0.4, 1}))
        fail("the fixture layer must be added");
    const auto* parameter = session.parameterForSelection(document::kOpacityParameterRole);
    if (parameter == nullptr)
        fail("the fixture opacity parameter must resolve");
    if (!session.pasteKeyframes({{parameter->id, time(1), 0.2},
                                 {parameter->id, time(3), 0.5},
                                 {parameter->id, time(5), 0.8}},
                                session.snapshot().revision()))
        fail("the fixture keys must seed");
    const auto* refreshed = session.composition()->parameters().find(parameter->id);
    const auto* source = refreshed != nullptr
                             ? std::get_if<document::AnimationCurveSource>(&refreshed->source)
                             : nullptr;
    if (source == nullptr)
        fail("the fixture curve must resolve");
    return source->curveId;
}

// 1. The polyline is the SAMPLER, not a second curve: every painted y is exactly pixelForValue of
// the value the evaluator produces at that column's own exact rational time -- on the eased
// segment too, where a second implementation would be most likely to drift.
void testPolylineIsTheSampler() {
    GraphFixture fixture(makeProject("Graph polyline"));
    const auto curveId = seedOpacity(fixture.session.session);
    fixture.expandAndShowGraph();
    auto& session = fixture.session.session;
    const auto* curve = scalarCurve(session, curveId);
    if (curve == nullptr || curve->keyframes.size() != 3)
        fail("the polyline fixture must have three keys");
    session.selectKeyframe(curveId, curve->keyframes.front().id);
    expect(session.setSelectedKeyframeInterpolation(document::KeyframeInterpolation::EaseInOut),
           "the first segment eases");
    QCoreApplication::processEvents();

    const ui::GraphCurveId graphCurve{curveId, std::nullopt};
    const auto polyline = fixture.view->polylineForTest(graphCurve);
    expect(!polyline.empty(), "the view produces one point per pixel column");
    const auto compiled = runtime::compileAnimationCurve(*scalarCurve(session, curveId));
    const auto axis = fixture.editor.rulerForTest()->axisForWidth(fixture.view->width());
    if (!axis.has_value())
        fail("the graph axis must resolve");
    const auto viewport = fixture.view->viewportFor(graphCurve);
    const auto [top, band] = fixture.view->valueBandForTest();
    bool matches = true;
    bool crossedTheEasedSegment = false;
    for (const auto& point : polyline) {
        const auto at = ui::keyPointerOffset(axis->secondsForPixel(point.x()));
        if (!at.has_value())
            continue;
        const auto sampled = runtime::sampleAnimationCurve(compiled, *at);
        if (!sampled.value.has_value() ||
            std::abs(point.y() - ui::pixelForValue(viewport, *sampled.value, top, band)) > 1e-9) {
            matches = false;
            break;
        }
        if (*at > time(1) && *at < time(3))
            crossedTheEasedSegment = true;
    }
    expect(matches && crossedTheEasedSegment,
           "every painted column IS the sampled value, the eased segment included");

    // And the eased segment really is eased: at a quarter of the way in, the closed-form
    // Smoothstep value is not the straight-line one. (At the exact midpoint they agree, which is
    // the symmetry of the ease rather than a missing ease.)
    const auto quarter = time(3, 2);
    const auto sampledQuarter = runtime::sampleAnimationCurve(compiled, quarter);
    expect(sampledQuarter.value.has_value() && std::abs(*sampledQuarter.value - 0.275) > 1e-9,
           "a quarter into the eased segment is the eased value, not the straight-line one");
}

// #358: playback moves the playhead every frame. The graph repaints only the playhead strips and
// never resamples a curve whose table, axis, size and viewport are unchanged.
void testPlaybackNeitherResamplesNorRepaintsTheGraph() {
    GraphFixture fixture(makeProject("Graph playback"));
    static_cast<void>(seedOpacity(fixture.session.session));
    fixture.expandAndShowGraph();
    auto& session = fixture.session.session;
    // Settle first: pending layout, resize and deferred zoom work are not playback.
    QElapsedTimer settle;
    settle.start();
    while (settle.elapsed() < 300)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    static_cast<void>(fixture.view->grab());
    const auto painted = fixture.view->work();
    expect(painted.curveSamples > 0, "the first paint samples the curve");
    for (int frame = 0; frame < 60; ++frame) {
        static_cast<void>(session.setCurrentTime(time(frame, 24)));
        QCoreApplication::processEvents();
    }
    static_cast<void>(fixture.view->grab());
    const auto played = fixture.view->work();
    expect(played.fullInvalidations == painted.fullInvalidations,
           "playback never invalidates the whole graph");
    expect(played.playheadInvalidations >= painted.playheadInvalidations + 59,
           "each playhead move invalidates only its strips");
    expect(played.curveSamples == painted.curveSamples,
           "an unchanged curve is drawn from its cached polyline, never resampled");
}

// 2. A value drag is one gesture: one history entry, and undo puts the key back exactly.
void testValueDragIsOneUndoableTransaction() {
    GraphFixture fixture(makeProject("Graph value drag"));
    const auto curveId = seedOpacity(fixture.session.session);
    fixture.expandAndShowGraph();
    auto& session = fixture.session.session;
    auto& stack = fixture.session.commands;
    const auto before = *scalarCurve(session, curveId);
    const ui::GraphCurveId graphCurve{curveId, std::nullopt};
    const auto center = fixture.view->keyCenter(graphCurve, before.keyframes[1].id);
    if (!center.has_value())
        fail("the dragged key must have a pixel");

    const auto history = stack.size();
    fixture.drag(*center, QPointF(center->x(), center->y() - 20));
    const auto* after = scalarCurve(session, curveId);
    expect(after != nullptr && after->keyframes[1].value != before.keyframes[1].value &&
               after->keyframes[1].id == before.keyframes[1].id,
           "dragging a key vertically re-values exactly that key, keeping its identity");
    expect(stack.size() == history + 1, "and the whole drag is ONE history entry");
    expect(session.undo() && *scalarCurve(session, curveId) == before,
           "one undo restores the curve bit for bit");
}

// 3. Shift locks the dominant axis (T1, item 7): a mostly vertical Shift-drag is value-only and a
// mostly horizontal one is time-only.
void testShiftLocksTheDominantAxis() {
    GraphFixture fixture(makeProject("Graph constrain"));
    const auto curveId = seedOpacity(fixture.session.session);
    fixture.expandAndShowGraph();
    auto& session = fixture.session.session;
    const auto before = *scalarCurve(session, curveId);
    const ui::GraphCurveId graphCurve{curveId, std::nullopt};
    const auto center = fixture.view->keyCenter(graphCurve, before.keyframes[1].id);
    if (!center.has_value())
        fail("the constrained key must have a pixel");
    fixture.drag(*center, QPointF(center->x() + 6, center->y() - 30), Qt::ShiftModifier);
    const auto* after = scalarCurve(session, curveId);
    expect(after != nullptr && after->keyframes[1].time == before.keyframes[1].time &&
               after->keyframes[1].value != before.keyframes[1].value,
           "a mostly vertical Shift-drag moves the value and leaves the exact time alone");
    const auto moved = *after;
    const auto again = fixture.view->keyCenter(graphCurve, moved.keyframes[1].id);
    if (!again.has_value())
        fail("the constrained key must still have a pixel");
    fixture.drag(*again, QPointF(again->x() + 60, again->y() + 7), Qt::ShiftModifier);
    const auto* sideways = scalarCurve(session, curveId);
    const auto key = std::ranges::find(sideways->keyframes, moved.keyframes[1].id,
                                       &document::ScalarKeyframe::id);
    expect(key != sideways->keyframes.end() && key->value == moved.keyframes[1].value &&
               key->time != moved.keyframes[1].time,
           "a mostly horizontal Shift-drag moves the time and leaves the value bit for bit");
}

// 4. Snapping on lands on a frame time; snapping off keeps the sub-frame time the pointer names.
void testSnappingGovernsTheDraggedTime() {
    for (const bool snapping : {true, false}) {
        GraphFixture fixture(makeProject("Graph snapping"));
        const auto curveId = seedOpacity(fixture.session.session);
        fixture.expandAndShowGraph();
        auto& session = fixture.session.session;
        fixture.view->setSnappingEnabled(snapping);
        const auto before = *scalarCurve(session, curveId);
        const ui::GraphCurveId graphCurve{curveId, std::nullopt};
        const auto center = fixture.view->keyCenter(graphCurve, before.keyframes[1].id);
        const auto axis = fixture.editor.rulerForTest()->axisForWidth(fixture.view->width());
        if (!center.has_value() || !axis.has_value())
            fail("the snapped key must have a pixel");
        // A deliberately off-frame destination, far from any other key's time.
        const double destinationSeconds = 3.77;
        fixture.drag(*center, QPointF(axis->pixelForSeconds(destinationSeconds), center->y()));
        const auto* after = scalarCurve(session, curveId);
        if (after == nullptr || after->keyframes.size() != 3) {
            expect(false, "the snapping fixture keeps its three keys");
            continue;
        }
        const auto moved = std::ranges::find(after->keyframes, before.keyframes[1].id,
                                             &document::ScalarKeyframe::id);
        if (moved == after->keyframes.end()) {
            expect(false, "the moved key survives the drag");
            continue;
        }
        const auto frame =
            ui::nearestFrameIndexForTime(axis->frameRate, axis->duration, moved->time);
        const auto exactFrame = frame.has_value()
                                    ? ui::frameTimeForIndex(axis->frameRate, axis->duration, *frame)
                                    : std::nullopt;
        const bool onFrame = exactFrame.has_value() && *exactFrame == moved->time;
        expect(snapping == onFrame,
               snapping ? "snapping on lands the key on an exact frame time"
                        : "snapping off keeps the sub-frame time the pointer named");
    }
}

// 5. A handle drag shapes the segment and eases it, through the command layer's own rule.
void testHandleDragShapesAndEasesItsSegment() {
    GraphFixture fixture(makeProject("Graph handles"));
    const auto curveId = seedOpacity(fixture.session.session);
    fixture.expandAndShowGraph();
    auto& session = fixture.session.session;
    const auto* curve = scalarCurve(session, curveId);
    if (curve == nullptr)
        fail("the handle fixture curve must resolve");
    const auto firstKey = curve->keyframes.front().id;
    session.selectKeyframe(curveId, firstKey);
    expect(session.setSelectedKeyframeInterpolation(document::KeyframeInterpolation::EaseInOut),
           "the shaped segment starts eased so its handles are drawn at all");
    QCoreApplication::processEvents();

    const auto compiledBefore = runtime::compileAnimationCurve(*scalarCurve(session, curveId));
    const auto midpointBefore = runtime::sampleAnimationCurve(compiledBefore, time(2));

    const ui::GraphCurveId graphCurve{curveId, std::nullopt};
    const auto keys = scalarCurve(session, curveId)->keyframes;
    const auto axis = fixture.editor.rulerForTest()->axisForWidth(fixture.view->width());
    if (!axis.has_value())
        fail("the handle axis must resolve");
    const auto controls = ui::bezierControls({keys[0].time.toSeconds(), keys[0].value},
                                             {keys[1].time.toSeconds(), keys[1].value},
                                             keys[0].outgoingHandle, keys[1].incomingHandle);
    const auto viewport = fixture.view->viewportFor(graphCurve);
    const auto [top, band] = fixture.view->valueBandForTest();
    const QPointF handlePoint(axis->pixelForSeconds(controls.startHandle.time),
                              ui::pixelForValue(viewport, controls.startHandle.value, top, band));
    const auto hit = fixture.view->hitTestForTest(handlePoint);
    expect(hit.has_value() && hit->kind == ui::GraphHit::Kind::Handle && hit->outgoing,
           "a selected key's outgoing handle is what the pointer finds first");
    if (!hit.has_value() || hit->kind != ui::GraphHit::Kind::Handle)
        return;

    fixture.drag(handlePoint, QPointF(handlePoint.x(), handlePoint.y() - 25));
    const auto* shaped = scalarCurve(session, curveId);
    expect(shaped != nullptr &&
               !document::isDefaultKeyframeHandle(shaped->keyframes[0].outgoingHandle),
           "the handle drag writes a non-default handle");
    expect(shaped != nullptr && shaped->keyframes[0].outgoingInterpolation ==
                                    document::KeyframeInterpolation::EaseInOut,
           "and the shaped segment's left key is Ease In-Out");
    const auto compiledAfter = runtime::compileAnimationCurve(*shaped);
    const auto midpointAfter = runtime::sampleAnimationCurve(compiledAfter, time(2));
    expect(midpointBefore.value.has_value() && midpointAfter.value.has_value() &&
               *midpointBefore.value != *midpointAfter.value,
           "and the segment's sampled midpoint really moved");
}

// 6. Box-select reaches component curves and records the component with every key.
void testBoxSelectSpansTwoComponentCurves() {
    GraphFixture fixture(makeProject("Graph box select"));
    auto& session = fixture.session.session;
    if (!session.addSolidLayer(QStringLiteral("Vector"), {0.2, 0.3, 0.4, 1}))
        fail("the box fixture layer must be added");
    const auto* position = session.parameterForSelection(document::kPositionParameterRole);
    if (position == nullptr)
        fail("the box fixture position parameter must resolve");
    const auto parameterId = position->id;
    if (!session.pasteKeyframes({{parameterId, time(1), document::Vec2d{1.0, 2.0}},
                                 {parameterId, time(5), document::Vec2d{3.0, 4.0}}},
                                session.snapshot().revision()))
        fail("the box fixture keys must seed");
    fixture.expandAndShowGraph();
    expect(fixture.view->curves().size() >= 2,
           "a Vec2 parameter draws one curve per non-empty component");

    fixture.mouse(QEvent::MouseButtonPress, QPointF(fixture.view->gutterWidth() + 1, 1));
    fixture.mouse(QEvent::MouseMove,
                  QPointF(fixture.view->width() - 1, fixture.view->height() - 1));
    fixture.mouse(QEvent::MouseButtonRelease,
                  QPointF(fixture.view->width() - 1, fixture.view->height() - 1));
    QCoreApplication::processEvents();
    const auto& selected = session.selection().keyframes;
    expect(selected.size() >= 4, "the box takes every key it covers, on every curve");
    expect(std::ranges::all_of(selected, [](const auto& key) { return key.component.has_value(); }),
           "and every selected component key carries its component, never the whole-value address");
    ui::TimelineLayerEntry component;
    component.rowKind = ui::TimelineLayerEntry::Kind::Component;
    component.parameterId = parameterId;
    component.component = document::AnimationComponent::Y;
    fixture.view->setEntries({component});
    expect(fixture.view->curves().size() == 1 &&
               fixture.view->curves().front().component == document::AnimationComponent::Y,
           "a component-only entry draws only its addressed graph curve");
    auto aggregate = component;
    aggregate.rowKind = ui::TimelineLayerEntry::Kind::Parameter;
    aggregate.component.reset();
    fixture.view->setEntries({aggregate, component});
    expect(fixture.view->curves().size() == 2,
           "expanded component entries do not duplicate graph curves");
}

// 7. Fit puts a wheel-zoomed viewport back on the curve.
void testFitRestoresAZoomedViewport() {
    GraphFixture fixture(makeProject("Graph fit"));
    const auto curveId = seedOpacity(fixture.session.session);
    fixture.expandAndShowGraph();
    const ui::GraphCurveId graphCurve{curveId, std::nullopt};
    const auto fitted = fixture.view->viewportFor(graphCurve);
    const QPointF anchor(fixture.view->width() / 2.0, fixture.view->height() / 2.0);
    QWheelEvent wheel(anchor, fixture.view->mapToGlobal(anchor), QPoint(), QPoint(0, 360),
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(fixture.view, &wheel);
    QCoreApplication::processEvents();
    const auto zoomed = fixture.view->viewportFor(graphCurve);
    expect(zoomed != fitted, "a plain vertical wheel zooms the value axis");
    fixture.view->fitCurves();
    expect(fixture.view->viewportFor(graphCurve) == fitted,
           "and Fit puts every curve back on its own keys and control points");
}

// 8. Double-clicking a segment inserts a key valued at the curve's own sampled value there, so
// the picture does not move.
void testDoubleClickInsertsAKeyOnTheCurve() {
    GraphFixture fixture(makeProject("Graph insert"));
    const auto curveId = seedOpacity(fixture.session.session);
    fixture.expandAndShowGraph();
    auto& session = fixture.session.session;
    const auto before = *scalarCurve(session, curveId);
    const auto compiled = runtime::compileAnimationCurve(before);
    const auto axis = fixture.editor.rulerForTest()->axisForWidth(fixture.view->width());
    if (!axis.has_value())
        fail("the insert axis must resolve");
    const auto at = time(2);
    const auto sampled = runtime::sampleAnimationCurve(compiled, at);
    const auto [top, band] = fixture.view->valueBandForTest();
    const QPointF point(axis->pixelForTime(at),
                        ui::pixelForValue(fixture.view->viewportFor({curveId, std::nullopt}),
                                          sampled.value.value_or(0.0), top, band));
    const auto hit = fixture.view->hitTestForTest(point);
    expect(hit.has_value() && hit->kind == ui::GraphHit::Kind::Segment,
           "a point on the drawn curve hit-tests as its segment");
    fixture.mouse(QEvent::MouseButtonDblClick, point);
    QCoreApplication::processEvents();
    const auto* after = scalarCurve(session, curveId);
    const auto inserted =
        after != nullptr ? std::ranges::find(after->keyframes, at, &document::ScalarKeyframe::time)
                         : decltype(after->keyframes.begin()){};
    expect(after != nullptr && after->keyframes.size() == before.keyframes.size() + 1 &&
               inserted != after->keyframes.end() &&
               std::abs(inserted->value - sampled.value.value_or(-1.0)) < 1e-9,
           "the inserted key carries the curve's own sampled value, so nothing moves");
}

// 9. The key context menu (T1, item 5) offers exactly Interpolation, Easing, Easy Ease / In / Out
// and Delete; Reset Handles is gone; the final key's Constant and Bezier are not offered for it.
void testContextMenuOffersTheCurveCommands() {
    GraphFixture fixture(makeProject("Graph menu"));
    const auto curveId = seedOpacity(fixture.session.session);
    fixture.expandAndShowGraph();
    auto& session = fixture.session.session;
    const auto keys = scalarCurve(session, curveId)->keyframes;
    session.selectKeyframe(curveId, keys.back().id);
    QCoreApplication::processEvents();

    QMenu menu;
    fixture.view->populateContextMenu(menu);
    QStringList top;
    QMenu* interpolation = nullptr;
    QMenu* easing = nullptr;
    for (auto* action : menu.actions()) {
        if (action->isSeparator())
            continue;
        top << (action->objectName().isEmpty() ? action->text() : action->objectName());
        if (action->menu() != nullptr && action->menu()->objectName() == "graphInterpolationMenu")
            interpolation = action->menu();
        if (action->menu() != nullptr && action->menu()->objectName() == "graphEasingMenu")
            easing = action->menu();
    }
    expect(top == QStringList({QStringLiteral("Interpolation"), QStringLiteral("Easing"),
                               QStringLiteral("timelineEasyEaseAction"),
                               QStringLiteral("timelineEasyEaseInAction"),
                               QStringLiteral("timelineEasyEaseOutAction"),
                               QStringLiteral("graphDeleteAction")}),
           "the key menu is Interpolation, Easing, the three Easy Eases and Delete, in order");
    expect(interpolation != nullptr && interpolation->actions().size() == 3 &&
               interpolation->actions()[0]->text() == QStringLiteral("Constant") &&
               interpolation->actions()[1]->text() == QStringLiteral("Linear") &&
               interpolation->actions()[2]->text() == QStringLiteral("Bezier"),
           "Interpolation offers Constant, Linear and Bezier");
    expect(interpolation != nullptr && !interpolation->actions()[0]->isEnabled() &&
               interpolation->actions()[1]->isEnabled() && !interpolation->actions()[2]->isEnabled(),
           "the final key keeps only its canonical Linear");
    expect(easing != nullptr && !easing->menuAction()->isEnabled(),
           "Easing asks for two or more keys");
    const auto findings = menu.findChildren<QAction*>(QStringLiteral("graphResetHandlesAction"));
    expect(findings.isEmpty(), "Reset Handles is gone");

    // Two keys: Easing > Constant holds the selected segments, the final key untouched.
    session.selectKeyframes({{curveId, keys[0].id, std::nullopt}, {curveId, keys[2].id, std::nullopt}});
    QMenu two;
    fixture.view->populateContextMenu(two);
    QAction* constant = nullptr;
    for (auto* action : two.findChildren<QAction*>(QStringLiteral("graphEasingAction")))
        if (action->text() == QStringLiteral("Constant"))
            constant = action;
    expect(constant != nullptr && constant->isEnabled(), "two keys enable the Easing presets");
    if (constant != nullptr)
        constant->trigger();
    const auto* held = scalarCurve(session, curveId);
    expect(held != nullptr &&
               held->keyframes[0].outgoingInterpolation == document::KeyframeInterpolation::Hold &&
               held->keyframes[2].outgoingInterpolation == document::KeyframeInterpolation::Linear,
           "Easing > Constant holds the selected keys' segments and leaves the final key Linear");
}

// T1, item 9: the Easing list offers Linear and Constant, then every Blender function as Ease In /
// Ease Out / Ease In-Out, and a pick writes the equation and its ease end as ONE undo entry.
void testEasingPresets() {
    GraphFixture fixture(makeProject("Graph easing"));
    const auto curveId = seedOpacity(fixture.session.session);
    fixture.expandAndShowGraph();
    auto& session = fixture.session.session;
    const auto keys = scalarCurve(session, curveId)->keyframes;
    session.selectKeyframes({{curveId, keys[0].id, std::nullopt}, {curveId, keys[1].id, std::nullopt}});
    QMenu menu;
    fixture.view->populateContextMenu(menu);
    QMenu* easing = menu.findChild<QMenu*>(QStringLiteral("graphEasingMenu"));
    if (easing == nullptr)
        fail("the Easing submenu must exist");
    QStringList entries;
    QStringList families;
    QMenu* elastic = nullptr;
    for (auto* action : easing->actions()) {
        if (action->isSeparator())
            continue;
        if (action->menu() != nullptr) {
            families << action->text();
            if (action->text() == QStringLiteral("Elastic"))
                elastic = action->menu();
        } else {
            entries << action->text();
        }
    }
    expect(entries == QStringList({QStringLiteral("Linear"), QStringLiteral("Constant")}),
           "Linear and Constant lead the Easing list");
    expect(families == QStringList({"Sine", "Quad", "Cubic", "Quart", "Quint", "Expo", "Circ",
                                    "Back", "Bounce", "Elastic"}),
           "then Blender's ten easing functions, in Blender's order");
    QAction* out = nullptr;
    if (elastic != nullptr && elastic->actions().size() == 3) {
        expect(elastic->actions()[0]->text() == QStringLiteral("Ease In") &&
                   elastic->actions()[1]->text() == QStringLiteral("Ease Out") &&
                   elastic->actions()[2]->text() == QStringLiteral("Ease In-Out"),
               "each as Ease In, Ease Out and Ease In-Out");
        out = elastic->actions()[1];
    } else {
        expect(false, "every family offers its three ease ends");
    }
    const auto history = fixture.session.commands.size();
    if (out != nullptr)
        out->trigger();
    const auto* eased = scalarCurve(session, curveId);
    expect(fixture.session.commands.size() == history + 1, "an Easing pick is ONE undo entry");
    expect(eased != nullptr &&
               eased->keyframes[0].outgoingInterpolation ==
                   document::KeyframeInterpolation::Elastic &&
               eased->keyframes[0].easing.easing == document::KeyframeEasing::Out &&
               eased->keyframes[1].outgoingInterpolation ==
                   document::KeyframeInterpolation::Elastic,
           "Elastic > Ease Out writes the equation and its ease end on the selected segments");
    expect(eased != nullptr && eased->keyframes[2].outgoingInterpolation ==
                                   document::KeyframeInterpolation::Linear,
           "and leaves an unselected key alone");
    const auto compiled = runtime::compileAnimationCurve(*eased);
    const auto sampled = runtime::sampleAnimationCurve(compiled, time(2));
    expect(sampled.value.has_value() &&
               std::abs(fixture.view->plottedAt({curveId, std::nullopt}, 2.0) - *sampled.value) <
                   1e-9,
           "and the curve the graph draws IS the engine's evaluation of it");
}

// T1, item 5: Easy Ease (F9) eases both sides at 33.33% / zero speed; Easy Ease In (Shift+F9) eases
// only the arriving side and keeps the far end of that segment linear.
void testEasyEaseActions() {
    for (const auto* name : {"timelineEasyEaseAction", "timelineEasyEaseInAction"}) {
        GraphFixture fixture(makeProject("Graph easy ease"));
        const auto curveId = seedOpacity(fixture.session.session);
        fixture.expandAndShowGraph();
        auto& session = fixture.session.session;
        const auto keys = scalarCurve(session, curveId)->keyframes;
        session.selectKeyframe(curveId, keys[1].id);
        auto* action = fixture.editor.findChild<QAction*>(QString::fromLatin1(name));
        if (action == nullptr)
            fail("the Easy Ease actions must exist");
        const auto history = fixture.session.commands.size();
        action->trigger();
        const auto* eased = scalarCurve(session, curveId);
        expect(fixture.session.commands.size() == history + 1, "an Easy Ease is ONE undo entry");
        expect(eased != nullptr && document::isDefaultKeyframeHandle(eased->keyframes[1].incomingHandle) &&
                   eased->keyframes[0].outgoingInterpolation ==
                       document::KeyframeInterpolation::EaseInOut,
               "the arriving side is eased and its segment is a Bezier");
        expect(eased != nullptr && eased->keyframes[0].outgoingHandle ==
                                       ui::linearTangentHandle(keys[0].value, keys[1].value, true),
               "and the segment's far end keeps its straight-line tangent");
        const bool both = std::string_view(name) == "timelineEasyEaseAction";
        expect(eased != nullptr &&
                   (eased->keyframes[1].outgoingInterpolation ==
                    document::KeyframeInterpolation::EaseInOut) == both,
               both ? "Easy Ease also eases the leaving side"
                    : "Easy Ease In leaves the leaving side linear");
    }
    GraphFixture fixture(makeProject("Graph ease shortcuts"));
    const auto shortcut = [&](const char* name) {
        auto* action = fixture.editor.findChild<QAction*>(QString::fromLatin1(name));
        return action != nullptr ? action->shortcut() : QKeySequence();
    };
    expect(shortcut("timelineEasyEaseAction") == QKeySequence(Qt::Key_F9) &&
               shortcut("timelineEasyEaseInAction") == QKeySequence(Qt::SHIFT | Qt::Key_F9) &&
               shortcut("timelineEasyEaseOutAction") ==
                   QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F9),
           "F9, Shift+F9 and Ctrl+Shift+F9, as in After Effects");
}

// 8. Keys hidden means nothing key-coloured is painted in the lane.
void testHiddenKeysPaintNoKeyframeInk() {
    GraphFixture fixture(makeProject("Graph hidden keys"));
    const auto curveId = seedOpacity(fixture.session.session);
    static_cast<void>(curveId);
    auto* keys = fixture.editor.findChild<QAction*>("timelineKeyframesAction");
    if (keys == nullptr)
        fail("the keys toggle must exist");
    const auto axis =
        fixture.editor.rulerForTest()->axisForWidth(fixture.editor.laneRegionForTest()->width());
    if (!axis.has_value())
        fail("the hidden-key axis must resolve");
    keys->setChecked(false);
    QCoreApplication::processEvents();
    const auto image = fixture.editor.laneRegionForTest()->grab().toImage();
    const auto keyframeInk = ui::kit::color(ui::kit::Color::Keyframe).rgb();
    bool painted = false;
    for (int y = 0; y < image.height() && !painted; ++y)
        for (int x = 0; x < image.width(); ++x)
            if ((image.pixel(x, y) & 0x00FFFFFFU) == (keyframeInk & 0x00FFFFFFU)) {
                painted = true;
                break;
            }
    expect(!painted, "with keys hidden the lane region paints no Keyframe-coloured pixel at all");
}

// T1, item 1: a key drag previews LIVE -- the curve, the value readout and the Viewer's override all
// follow the pointer -- while the document and the history stay untouched until the release, which
// commits exactly what was previewed as ONE undo entry.
void testDragPreviewsLiveAndCommitsOnce() {
    GraphFixture fixture(makeProject("Graph live"));
    auto& session = fixture.session.session;
    const auto curveId = seedOpacity(session);
    std::optional<document::ParameterId> owner;
    for (const auto& record : session.composition()->parameters().records())
        if (const auto* source = std::get_if<document::AnimationCurveSource>(&record.source);
            source != nullptr && source->curveId == curveId)
            owner = record.id;
    if (!owner.has_value())
        fail("the live fixture parameter must resolve");
    const auto parameterId = *owner;
    fixture.expandAndShowGraph();
    static_cast<void>(session.setCurrentTime(time(3)));
    QCoreApplication::processEvents();
    const auto before = *scalarCurve(session, curveId);
    const ui::GraphCurveId graphCurve{curveId, std::nullopt};
    const auto center = fixture.view->keyCenter(graphCurve, before.keyframes[1].id);
    if (!center.has_value())
        fail("the live key must have a pixel");
    const auto history = fixture.session.commands.size();
    fixture.mouse(QEvent::MouseButtonPress, *center);
    fixture.mouse(QEvent::MouseMove, QPointF(center->x(), center->y() - 30));
    QCoreApplication::processEvents();
    expect(session.keyframePreviewActive(), "a drag in progress is a live preview");
    expect(*scalarCurve(session, curveId) == before &&
               fixture.session.commands.size() == history,
           "and nothing reaches the document or the history while the button is held");
    const auto moving = fixture.view->keyCenter(graphCurve, before.keyframes[1].id);
    expect(moving.has_value() && std::abs(moving->y() - (center->y() - 30)) < 1.0,
           "the key and its curve follow the pointer");
    const auto readout = session.liveValue(parameterId);
    const auto* live = readout.has_value() ? std::get_if<double>(&*readout) : nullptr;
    expect(live != nullptr && *live > before.keyframes[1].value,
           "the value readout at the playhead follows the drag");
    const auto overrides = session.valueEditOverrides();
    expect(overrides.size() == 1 && overrides.front().parameterId == parameterId,
           "and the Viewer previews it through the parameter override channel");
    fixture.mouse(QEvent::MouseButtonRelease, QPointF(center->x(), center->y() - 30));
    QCoreApplication::processEvents();
    const auto* after = scalarCurve(session, curveId);
    expect(!session.keyframePreviewActive() && fixture.session.commands.size() == history + 1,
           "the release ends the preview and commits ONE undo entry");
    expect(after != nullptr && live != nullptr && std::abs(after->keyframes[1].value - *live) < 1e-12,
           "carrying exactly the value the preview showed");
}

// T1, item 2: the Speed Graph plots the curve's speed from the sampler, and the header switches
// between the two graph types.
void testSpeedGraph() {
    GraphFixture fixture(makeProject("Graph speed"));
    const auto curveId = seedOpacity(fixture.session.session);
    fixture.expandAndShowGraph();
    auto* header = fixture.view->headerForTest();
    auto* types = header != nullptr ? header->findChild<ui::kit::KRadioGroup*>(
                                          QStringLiteral("timelineGraphTypeSwitch"))
                                    : nullptr;
    if (types == nullptr)
        fail("the graph header must carry the graph type switch");
    expect(types->count() == 2 && types->optionText(0) == QStringLiteral("Value Graph") &&
               types->optionText(1) == QStringLiteral("Speed Graph"),
           "the header offers the Value Graph and the Speed Graph");
    types->setCurrentIndex(1);
    QCoreApplication::processEvents();
    expect(fixture.view->graphType() == ui::GraphType::Speed &&
               QSettings().value(QStringLiteral("timeline/graph-type")).toString() ==
                   QStringLiteral("speed"),
           "choosing Speed Graph switches the canvas and persists");
    const ui::GraphCurveId graphCurve{curveId, std::nullopt};
    // 0.2 -> 0.5 linearly over [1, 3]: 0.15 per second.
    expect(std::abs(fixture.view->plottedAt(graphCurve, 2.0) - 0.15) < 1e-6,
           "a linear segment plots its slope");
    const auto polyline = fixture.view->polylineForTest(graphCurve);
    const auto axis = fixture.editor.rulerForTest()->axisForWidth(fixture.view->width());
    const auto lens = fixture.view->lensFor(graphCurve);
    int near = 0;
    int onSlope = 0;
    for (const auto& point : polyline)
        if (axis.has_value() && std::abs(axis->secondsForPixel(point.x()) - 2.0) < 0.05) {
            ++near;
            if (std::abs(point.y() - lens.pixelFor(0.15)) < 0.5)
                ++onSlope;
        }
    expect(near > 0 && onSlope == near, "and the painted speed line sits at that slope");

    // A Speed Graph handle is influence/speed: dragging it up raises the key's leaving speed.
    auto& session = fixture.session.session;
    const auto keys = scalarCurve(session, curveId)->keyframes;
    session.selectKeyframe(curveId, keys[0].id);
    expect(session.setSelectedKeyframeInterpolation(document::KeyframeInterpolation::EaseInOut),
           "the shaped segment starts as a Bezier");
    QCoreApplication::processEvents();
    const auto handle = fixture.view->handleCenter(graphCurve, keys[0].id, true);
    if (!handle.has_value())
        fail("a selected eased key shows its speed handle");
    const auto shapedLens = fixture.view->lensFor(graphCurve);
    fixture.drag(*handle, QPointF(handle->x(), handle->y() - 20), Qt::ShiftModifier);
    const auto* shaped = scalarCurve(session, curveId);
    const auto speed = ui::handleSpeed(shaped->keyframes[0].outgoingHandle, 2.0, true);
    expect(speed.has_value() && std::abs(*speed - shapedLens.quantityFor(handle->y() - 20)) < 1e-6,
           "the dragged handle writes the speed under the pointer");
    expect(std::abs(shaped->keyframes[0].outgoingHandle.time - keys[0].outgoingHandle.time) < 1e-12,
           "and a vertical Shift-drag keeps its influence");
}

// T1, item 3: Normalize draws each curve into -1...1, and a drag still writes real units.
void testNormalizeDrawsScaledAndWritesReal() {
    GraphFixture fixture(makeProject("Graph normalize"));
    const auto curveId = seedOpacity(fixture.session.session);
    fixture.expandAndShowGraph();
    auto* toggle = fixture.view->headerForTest()->findChild<ui::kit::KCheckBox*>(
        QStringLiteral("timelineGraphNormalizeToggle"));
    if (toggle == nullptr)
        fail("the graph header must carry Normalize");
    toggle->click();
    QCoreApplication::processEvents();
    expect(fixture.view->normalized(), "the header toggle turns Normalize on");
    auto& session = fixture.session.session;
    const auto keys = scalarCurve(session, curveId)->keyframes;
    const ui::GraphCurveId graphCurve{curveId, std::nullopt};
    const auto lens = fixture.view->lensFor(graphCurve);
    const auto [top, band] = fixture.view->valueBandForTest();
    const auto low = fixture.view->keyCenter(graphCurve, keys[0].id);
    const auto high = fixture.view->keyCenter(graphCurve, keys[2].id);
    expect(low.has_value() && high.has_value() &&
               std::abs(low->y() - ui::pixelForValue(ui::normalizedViewport(), -1.0, top, band)) <
                   1e-9 &&
               std::abs(high->y() - ui::pixelForValue(ui::normalizedViewport(), 1.0, top, band)) <
                   1e-9,
           "the curve's lowest key draws at -1 and its highest at 1");
    const auto middle = fixture.view->keyCenter(graphCurve, keys[1].id);
    if (!middle.has_value())
        fail("the normalized key must have a pixel");
    fixture.drag(*middle, QPointF(middle->x(), middle->y() - 20));
    const auto* after = scalarCurve(session, curveId);
    expect(after != nullptr &&
               std::abs(after->keyframes[1].value - (keys[1].value + 20 * lens.quantityPerPixel())) <
                   1e-9,
           "a normalized drag writes the REAL value its travel means");
}

// T1, item 4: the value gutter covers nothing. The shared time axis keeps the gutter's width clear
// before frame 0 while the graph is shown, so a frame-0 key is visible and grabbable.
void testFrameZeroKeysClearTheGutter() {
    GraphFixture fixture(makeProject("Graph frame zero"));
    auto& session = fixture.session.session;
    if (!session.addSolidLayer(QStringLiteral("Zero"), {0.2, 0.3, 0.4, 1}))
        fail("the frame-zero layer must be added");
    const auto* parameter = session.parameterForSelection(document::kOpacityParameterRole);
    if (parameter == nullptr)
        fail("the frame-zero parameter must resolve");
    const auto parameterId = parameter->id;
    if (!session.pasteKeyframes({{parameterId, time(0), 0.1},
                                 {parameterId, time(1), 0.5},
                                 {parameterId, time(2), 0.9}},
                                session.snapshot().revision()))
        fail("the frame-zero keys must seed");
    const auto* source = std::get_if<document::AnimationCurveSource>(
        &session.composition()->parameters().find(parameterId)->source);
    if (source == nullptr)
        fail("the frame-zero curve must resolve");
    const auto curveId = source->curveId;
    fixture.expandAndShowGraph();
    auto* ruler = fixture.editor.rulerForTest();
    expect(ruler->leadingInset() == fixture.view->gutterWidth(),
           "with the graph shown the time axis leads with the gutter's width");
    const ui::GraphCurveId graphCurve{curveId, std::nullopt};
    const auto first = scalarCurve(session, curveId)->keyframes.front();
    const auto center = fixture.view->keyCenter(graphCurve, first.id);
    expect(center.has_value() &&
               center->x() - ui::kit::kKeyDiamondRadius > fixture.view->gutterWidth(),
           "a frame-0 key sits wholly clear of the value gutter");
    const auto hit = center ? fixture.view->hitTestForTest(*center) : std::nullopt;
    expect(hit.has_value() && hit->kind == ui::GraphHit::Kind::Key && hit->keyframeId == first.id,
           "and it is grabbable");
    fixture.editor.findChild<QAction*>("timelineKeyframesAction")->setChecked(true);
    QCoreApplication::processEvents();
    expect(ruler->leadingInset() == 0, "the key lanes get their full width back");
}

// T1, item 6: the header toggle beside Fullscreen (and Shift+F3) switches the right pane between
// the keyframes and the graph.
void testGraphToggleSwitchesThePane() {
    GraphFixture fixture(makeProject("Graph toggle"));
    static_cast<void>(seedOpacity(fixture.session.session));
    QCoreApplication::processEvents();
    auto* toggle = fixture.editor.findChild<ui::kit::KIconButton*>("timelineGraphToggle");
    auto* action = fixture.editor.findChild<QAction*>("timelineGraphPaneAction");
    if (toggle == nullptr || action == nullptr)
        fail("the graph toggle and its action must exist");
    expect(action->shortcut() == QKeySequence(Qt::SHIFT | Qt::Key_F3),
           "the switch is Shift+F3, as in After Effects");
    toggle->click();
    QCoreApplication::processEvents();
    expect(fixture.editor.laneRegionForTest()->graphEditorEnabled() && toggle->isChecked(),
           "the toggle shows the graph");
    toggle->click();
    QCoreApplication::processEvents();
    expect(!fixture.editor.laneRegionForTest()->graphEditorEnabled() && !toggle->isChecked() &&
               fixture.editor.findChild<QAction*>("timelineKeyframesAction")->isChecked(),
           "and switching it off shows the keyframes again: a switch, never a blank pane");
}

// T1, item 7: Shift on a handle snaps its on-screen angle to 45 degree steps.
void testShiftSnapsHandleAngles() {
    GraphFixture fixture(makeProject("Graph handle snap"));
    const auto curveId = seedOpacity(fixture.session.session);
    fixture.expandAndShowGraph();
    auto& session = fixture.session.session;
    const auto keys = scalarCurve(session, curveId)->keyframes;
    session.selectKeyframe(curveId, keys[0].id);
    expect(session.setSelectedKeyframeInterpolation(document::KeyframeInterpolation::EaseInOut),
           "the snapped segment starts as a Bezier");
    QCoreApplication::processEvents();
    const ui::GraphCurveId graphCurve{curveId, std::nullopt};
    const auto handle = fixture.view->handleCenter(graphCurve, keys[0].id, true);
    const auto key = fixture.view->keyCenter(graphCurve, keys[0].id);
    if (!handle.has_value() || !key.has_value())
        fail("the snapped handle must have a pixel");
    // Pull the handle up to just short of the diagonal through its key: Shift lands it on it.
    const double reach = handle->x() - key->x();
    fixture.drag(*handle, QPointF(handle->x(), key->y() - reach + 4), Qt::ShiftModifier);
    const auto after = fixture.view->handleCenter(graphCurve, keys[0].id, true);
    const auto anchor = fixture.view->keyCenter(graphCurve, keys[0].id);
    if (!after.has_value() || !anchor.has_value())
        fail("the snapped handle must survive");
    const double dx = after->x() - anchor->x();
    const double dy = after->y() - anchor->y();
    expect(std::abs(std::abs(dx) - std::abs(dy)) < 0.5 && dx > 0.0 && dy < 0.0,
           "a handle pulled up near 45 degrees lands exactly on the 45 degree diagonal");
}

// T1, item 8: X red, Y green, single-value curves from the GraphCurve palette in turn, and the
// selected curve brought forward. Tokens only.
void testCurveColors() {
    GraphFixture fixture(makeProject("Graph colors"));
    auto& session = fixture.session.session;
    if (!session.addSolidLayer(QStringLiteral("Colors"), {0.2, 0.3, 0.4, 1}))
        fail("the colour fixture layer must be added");
    const auto* position = session.parameterForSelection(document::kPositionParameterRole);
    const auto* opacity = session.parameterForSelection(document::kOpacityParameterRole);
    const auto* rotation = session.parameterForSelection(document::kRotationParameterRole);
    if (position == nullptr || opacity == nullptr || rotation == nullptr)
        fail("the colour fixture parameters must resolve");
    if (!session.pasteKeyframes({{position->id, time(1), document::Vec2d{1.0, 2.0}},
                                 {position->id, time(4), document::Vec2d{3.0, 5.0}},
                                 {opacity->id, time(1), 0.2},
                                 {opacity->id, time(4), 0.9},
                                 {rotation->id, time(1), 0.0},
                                 {rotation->id, time(4), 90.0}},
                                session.snapshot().revision()))
        fail("the colour fixture keys must seed");
    fixture.expandAndShowGraph();
    std::optional<ui::GraphCurveId> x, y;
    std::vector<ui::GraphCurveId> singles;
    for (const auto& curve : fixture.view->curves()) {
        if (curve.component == document::AnimationComponent::X)
            x = curve;
        else if (curve.component == document::AnimationComponent::Y)
            y = curve;
        else if (!curve.component.has_value())
            singles.push_back(curve);
    }
    if (!x || !y || singles.size() < 2)
        fail("the colour fixture must draw X, Y and two single-value curves");
    expect(fixture.view->curveColor(*x, false) == ui::kit::color(ui::kit::Color::ComponentX) &&
               fixture.view->curveColor(*y, false) == ui::kit::color(ui::kit::Color::ComponentY),
           "X is red and Y is green");
    expect(fixture.view->curveColor(singles[0], false) ==
                   ui::kit::color(ui::kit::Color::GraphCurve1) &&
               fixture.view->curveColor(singles[1], false) ==
                   ui::kit::color(ui::kit::Color::GraphCurve2),
           "single-value curves take distinct palette tokens in curve order");
    expect(fixture.view->curveColor(*x, true) ==
               ui::kit::emphasizedStrokeFor(ui::kit::color(ui::kit::Color::ComponentX)),
           "a selected curve is drawn brighter");
    const auto keys = session.composition()->animationCurves().findVec2(x->curveId);
    session.selectKeyframe(x->curveId, document::AnimationComponent::X,
                           keys->component(document::AnimationComponent::X)->keyframes[0].id);
    const auto selected = fixture.view->selectedCurves();
    expect(selected.size() == 1 && selected.front() == *x,
           "the curve carrying the selected key is the selected curve");
}

// T1, item 10: the grid. Vertical lines stand on the ruler's own labelled ticks, horizontal lines on
// the gutter's value ticks, in the Border token.
void testGridFollowsTheRulerAndTheGutter() {
    GraphFixture fixture(makeProject("Graph grid"));
    const auto curveId = seedOpacity(fixture.session.session);
    fixture.expandAndShowGraph();
    auto* ruler = fixture.editor.rulerForTest();
    const auto ticks = ruler->majorTickTimes(fixture.view->width());
    const auto axis = ruler->axisForWidth(fixture.view->width());
    if (ticks.empty() || !axis.has_value())
        fail("the ruler must label ticks across the graph");
    std::size_t aligned = 0;
    for (const auto& rect : ruler->majorTickLabelRectsForTest())
        for (const auto tick : ticks)
            if (std::abs(rect.center().x() - axis->pixelForTime(tick)) < 0.5)
                ++aligned;
    expect(aligned > 0 && aligned <= ticks.size(),
           "every ruler label the graph sees stands on one of the graph's vertical lines");
    const auto image = fixture.view->grab().toImage();
    const auto border = ui::kit::color(ui::kit::Color::Border).rgb() & 0x00FFFFFFU;
    const double ratio = image.devicePixelRatio();
    const auto [top, band] = fixture.view->valueBandForTest();
    bool vertical = false;
    for (const auto tick : ticks) {
        const int x = static_cast<int>(std::round(axis->pixelForTime(tick)) * ratio);
        const int yProbe = static_cast<int>((top + band * 0.37) * ratio);
        if (x > fixture.view->gutterWidth() * ratio && x < image.width() &&
            (image.pixel(x, yProbe) & 0x00FFFFFFU) == border)
            vertical = true;
    }
    expect(vertical, "the vertical grid lines are painted in Border at the ruler's ticks");
    const auto lens = fixture.view->lensFor({curveId, std::nullopt});
    bool horizontal = false;
    for (const double value :
         ui::valueTicks(lens.viewport, ui::niceTickStep(lens.viewport.span(),
                                                        std::max(2, static_cast<int>(
                                                                        band / ui::kit::px(
                                                                                   ui::kit::Size::HeaderRow)))))) {
        const int y = static_cast<int>(
            std::round(ui::pixelForValue(lens.viewport, value, top, band)) * ratio);
        const int xProbe = static_cast<int>((fixture.view->gutterWidth() + 3) * ratio);
        if (y > 0 && y < image.height() && (image.pixel(xProbe, y) & 0x00FFFFFFU) == border)
            horizontal = true;
    }
    expect(horizontal, "and the horizontal ones at the gutter's value labels");
}

// T1, item 11: F frames the selected keys on both axes; the middle button pans both axes.
void testFitSelectionAndPan() {
    GraphFixture fixture(makeProject("Graph fit selection"));
    const auto curveId = seedOpacity(fixture.session.session);
    fixture.expandAndShowGraph();
    auto& session = fixture.session.session;
    const auto keys = scalarCurve(session, curveId)->keyframes;
    session.selectKeyframes({{curveId, keys[1].id, std::nullopt}});
    fixture.view->setFocus();
    QKeyEvent press(QEvent::KeyPress, Qt::Key_F, Qt::NoModifier);
    QCoreApplication::sendEvent(fixture.view, &press);
    QCoreApplication::processEvents();
    auto axis = fixture.editor.rulerForTest()->axisForWidth(fixture.view->width());
    expect(axis.has_value() && axis->t0 < 3.0 && axis->t1 > 3.0 && axis->t1 - axis->t0 < 2.0,
           "F frames the selected key in time, close in");
    const auto viewport = fixture.view->viewportFor({curveId, std::nullopt});
    expect(viewport.minimum < keys[1].value && viewport.maximum > keys[1].value,
           "and in value");
    const double t0 = axis->t0;
    const QPointF from(fixture.view->width() / 2.0, fixture.view->height() / 2.0);
    fixture.mouse(QEvent::MouseButtonPress, from, Qt::NoModifier, Qt::MiddleButton);
    fixture.mouse(QEvent::MouseMove, from + QPointF(40, 25), Qt::NoModifier, Qt::MiddleButton);
    fixture.mouse(QEvent::MouseButtonRelease, from + QPointF(40, 25), Qt::NoModifier,
                  Qt::MiddleButton);
    axis = fixture.editor.rulerForTest()->axisForWidth(fixture.view->width());
    expect(axis.has_value() && axis->t0 < t0, "the middle button drags time with the hand");
    expect(fixture.view->viewportFor({curveId, std::nullopt}) != viewport,
           "and values with it");
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    application.setOrganizationName(QStringLiteral("BloomGraphTests"));
    application.setApplicationName(QStringLiteral("TimelineGraphView"));
    // Isolated settings: the header toggles persist, and a test that read the developer's own
    // preferences would pass or fail on what they happened to have clicked last.
    const QTemporaryDir settingsDirectory;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
    testPolylineIsTheSampler();
    testPlaybackNeitherResamplesNorRepaintsTheGraph();
    testValueDragIsOneUndoableTransaction();
    testShiftLocksTheDominantAxis();
    testSnappingGovernsTheDraggedTime();
    testHandleDragShapesAndEasesItsSegment();
    testBoxSelectSpansTwoComponentCurves();
    testFitRestoresAZoomedViewport();
    testDoubleClickInsertsAKeyOnTheCurve();
    testContextMenuOffersTheCurveCommands();
    testHiddenKeysPaintNoKeyframeInk();
    testEasingPresets();
    testEasyEaseActions();
    testDragPreviewsLiveAndCommitsOnce();
    testSpeedGraph();
    testNormalizeDrawsScaledAndWritesReal();
    testFrameZeroKeysClearTheGutter();
    testGraphToggleSwitchesThePane();
    testShiftSnapsHandleAngles();
    testCurveColors();
    testGridFollowsTheRulerAndTheGutter();
    testFitSelectionAndPan();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
