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
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/timeline_editor.hpp>
#include <bloom/ui/timeline_frame_math.hpp>
#include <bloom/ui/timeline_graph_view.hpp>
#include <bloom/ui/timeline_ruler.hpp>

#include <QApplication>
#include <QContextMenuEvent>
#include <QImage>
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

// A timeline with its one layer expanded and the graph editor on, plus the handles a test needs
// to drive it. Built through the real editor rather than a bare view, because "the graph replaces
// the lanes" is part of what is under test.
struct GraphFixture final {
    SessionFixture session;
    ui::TimelineEditor editor;
    ui::TimelineGraphView* view = nullptr;

    explicit GraphFixture(document::NewProject project)
        : session(std::move(project)), editor(session.session, session.controller) {
        editor.resize(1600, 700);
        editor.show();
        QCoreApplication::processEvents();
    }

    void expandAndShowGraph() {
        // The layer and its keys are authored after the editor exists, exactly as an artist would;
        // let the editor rebuild its rows before looking for them.
        QCoreApplication::processEvents();
        auto* graph = editor.findChild<QToolButton*>("timelineGraphEditorButton");
        if (graph == nullptr)
            fail("the graph toggle must exist");
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

// 3. Ctrl constrains to the dominant axis.
void testCtrlConstrainsToTheDominantAxis() {
    GraphFixture fixture(makeProject("Graph constrain"));
    const auto curveId = seedOpacity(fixture.session.session);
    fixture.expandAndShowGraph();
    auto& session = fixture.session.session;
    const auto before = *scalarCurve(session, curveId);
    const ui::GraphCurveId graphCurve{curveId, std::nullopt};
    const auto center = fixture.view->keyCenter(graphCurve, before.keyframes[1].id);
    if (!center.has_value())
        fail("the constrained key must have a pixel");
    fixture.drag(*center, QPointF(center->x(), center->y() - 30), Qt::ControlModifier);
    const auto* after = scalarCurve(session, curveId);
    expect(after != nullptr && after->keyframes[1].time == before.keyframes[1].time &&
               after->keyframes[1].value != before.keyframes[1].value,
           "a mostly vertical Ctrl-drag moves the value and leaves the exact time alone");
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
    if (!session.pasteKeyframes({{position->id, time(1), document::Vec2d{1.0, 2.0}},
                                 {position->id, time(5), document::Vec2d{3.0, 4.0}}},
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

// 9. The context menu offers the real commands, and disables the two modes the final key cannot
// take rather than offering them and refusing.
void testContextMenuOffersTheCurveCommands() {
    GraphFixture fixture(makeProject("Graph menu"));
    const auto curveId = seedOpacity(fixture.session.session);
    fixture.expandAndShowGraph();
    auto& session = fixture.session.session;
    const auto keys = scalarCurve(session, curveId)->keyframes;
    session.selectKeyframe(curveId, keys.back().id);
    QCoreApplication::processEvents();

    bool sawHoldDisabled = false;
    bool sawLinearEnabled = false;
    bool sawReset = false;
    bool sawDelete = false;
    bool sawFit = false;
    QTimer::singleShot(0, fixture.view, [&] {
        for (auto* menu : fixture.view->findChildren<QMenu*>()) {
            for (auto* action : menu->actions()) {
                if (action->objectName() == QStringLiteral("graphInterpolationAction")) {
                    if (action->text() == QStringLiteral("Hold"))
                        sawHoldDisabled = !action->isEnabled();
                    if (action->text() == QStringLiteral("Linear"))
                        sawLinearEnabled = action->isEnabled();
                }
                sawReset =
                    sawReset || action->objectName() == QStringLiteral("graphResetHandlesAction");
                sawDelete =
                    sawDelete || action->objectName() == QStringLiteral("graphDeleteAction");
                sawFit = sawFit || action->objectName() == QStringLiteral("graphFitAction");
            }
            menu->close();
        }
    });
    const QPoint point(fixture.view->width() / 2, fixture.view->height() / 2);
    QContextMenuEvent context(QContextMenuEvent::Mouse, point, fixture.view->mapToGlobal(point));
    QCoreApplication::sendEvent(fixture.view, &context);
    expect(sawHoldDisabled && sawLinearEnabled,
           "the final key's Hold is disabled while its canonical Linear stays available");
    expect(sawReset && sawDelete && sawFit,
           "Reset Handles, Delete Keyframes and Fit Curves are all offered");
}

// 8. Keys hidden means nothing key-coloured is painted in the lane.
void testHiddenKeysPaintNoKeyframeInk() {
    GraphFixture fixture(makeProject("Graph hidden keys"));
    const auto curveId = seedOpacity(fixture.session.session);
    static_cast<void>(curveId);
    auto* keys = fixture.editor.findChild<QToolButton*>("timelineKeyframesVisibleButton");
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
    testValueDragIsOneUndoableTransaction();
    testCtrlConstrainsToTheDominantAxis();
    testSnappingGovernsTheDraggedTime();
    testHandleDragShapesAndEasesItsSegment();
    testBoxSelectSpansTwoComponentCurves();
    testFitRestoresAZoomedViewport();
    testDoubleClickInsertsAKeyOnTheCurve();
    testContextMenuOffersTheCurveCommands();
    testHiddenKeysPaintNoKeyframeInk();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
