#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/animation.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/project.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/timeline_ruler.hpp>

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointF>
#include <QString>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <optional>
#include <source_location>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace std::chrono_literals;

class Expectations final {
  public:
    void expect(const bool condition, const std::string& message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

template <typename Predicate> bool waitUntil(Predicate predicate) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 4'000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (std::invoke(predicate)) {
            return true;
        }
        std::this_thread::yield();
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return std::invoke(predicate);
}

bloom::runtime::TaskSchedulerConfig testSchedulerConfig() {
    return {.cpuWorkerCount = 1,
            .blockingIoWorkerCount = 1,
            .cpuQueueCapacity = 16,
            .blockingIoQueueCapacity = 4,
            .terminalHistoryCapacity = 32,
            .diagnosticsPerTask = 8,
            .groupRegistryCapacity = 8};
}

bloom::document::CompositionFormat smallFormat() {
    const auto format = bloom::document::CompositionFormat::create(4, 3);
    if (!format.has_value()) {
        std::abort();
    }
    return *format;
}

[[nodiscard]] bloom::core::RationalTime time(const std::int64_t numerator,
                                             const std::int64_t denominator = 1) {
    const auto value = bloom::core::RationalTime::create(numerator, denominator);
    if (!value.has_value()) {
        std::abort();
    }
    return *value;
}

bloom::document::NewProject makeTestProject(std::string projectName,
                                            const bloom::core::RationalTime duration) {
    return bloom::document::makeNewProject(std::move(projectName), "Main", duration, smallFormat());
}

struct PipelineFixture final {
    bloom::runtime::NodeDefinitionRegistry definitions;
    bloom::runtime::SnapshotCompiler compiler;
    bloom::runtime::CpuCompositionEvaluator evaluator;
    bloom::runtime::CpuReferenceDisplayPreparer displayPreparer;
    bloom::ui::PreviewPreparationFunction pipeline;

    PipelineFixture() : compiler(definitions) {
        if (!bloom::runtime::registerBuiltInNodeDefinitions(definitions)) {
            std::abort();
        }
        definitions.freeze();
        pipeline = bloom::ui::makeCompositionPreviewPipeline(compiler, evaluator, displayPreparer);
    }
};

// A fixture bundling everything a TimelineRuler/TimelineKeyframePanel test needs: a real document,
// command stack, session, scheduler, bridge, and preview controller (offscreen, matching the
// idiom in composition_preview_controller_tests.cpp and viewer_editor's own construction pattern).
struct SessionFixture final {
    bloom::document::Document document;
    bloom::commands::CommandStack commands;
    bloom::ui::CompositionSession session;
    bloom::runtime::TaskScheduler scheduler;
    bloom::ui::TaskUiBridge bridge;
    PipelineFixture pipeline;
    bloom::ui::CompositionPreviewController controller;

    explicit SessionFixture(bloom::document::NewProject newProject)
        : document(std::move(newProject.project)), commands(document),
          session(document, commands, newProject.initialCompositionId),
          scheduler(testSchedulerConfig()), bridge(scheduler, nullptr, 1ms),
          controller(session, scheduler, bridge, pipeline.pipeline) {}
};

struct LayerIds final {
    bloom::document::LayerId layer;
    bloom::document::ParameterId position;
    bloom::document::ParameterId opacity;
};

[[nodiscard]] LayerIds addSolidLayer(bloom::document::Document& document,
                                     bloom::commands::CommandStack& stack,
                                     const bloom::document::CompositionId compositionId) {
    using namespace bloom;
    commands::Transaction transaction("Add test layer", document.snapshot().revision());
    transaction.emplace<commands::AddSolidLayer>(
        compositionId, "Solid", core::Color4d{0.2, 0.3, 0.4, 1.0}, document::Vec2d{2.0, 1.5});
    const auto result = stack.execute(std::move(transaction));
    const auto layer = result.outputId<document::LayerId>(commands::kAddSolidLayerLayerOutput);
    const auto position =
        result.outputId<document::ParameterId>(commands::kAddSolidLayerPositionParameterOutput);
    const auto opacity =
        result.outputId<document::ParameterId>(commands::kAddSolidLayerOpacityParameterOutput);
    if (!(result.changed() && layer.has_value() && position.has_value() && opacity.has_value())) {
        std::abort();
    }
    return {*layer, *position, *opacity};
}

[[nodiscard]] bloom::document::AnimationCurveId
animateParameter(bloom::document::Document& document, bloom::commands::CommandStack& stack,
                 const bloom::document::CompositionId compositionId,
                 const bloom::document::ParameterId parameterId,
                 const bloom::core::RationalTime seedTime) {
    using namespace bloom;
    commands::Transaction transaction("Animate test parameter", document.snapshot().revision());
    transaction.emplace<commands::CreateAnimationForParameter>(compositionId, parameterId,
                                                               seedTime);
    const auto result = stack.execute(std::move(transaction));
    const auto curve = result.outputId<document::AnimationCurveId>(commands::kAnimationCurveOutput);
    if (!(result.changed() && curve.has_value())) {
        std::abort();
    }
    return *curve;
}

void sendClick(QWidget& widget, const qreal pixelX, const qreal pixelY = -1.0) {
    const qreal y = pixelY >= 0.0 ? pixelY : widget.height() / 2.0;
    const QPointF local(pixelX, y);
    QMouseEvent press(QEvent::MouseButtonPress, local, local, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QCoreApplication::sendEvent(&widget, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, local, local, Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QCoreApplication::sendEvent(&widget, &release);
}

void sendPress(QWidget& widget, const qreal pixelX) {
    const QPointF local(pixelX, widget.height() / 2.0);
    QMouseEvent press(QEvent::MouseButtonPress, local, local, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QCoreApplication::sendEvent(&widget, &press);
}

void sendRelease(QWidget& widget, const qreal pixelX) {
    const QPointF local(pixelX, widget.height() / 2.0);
    QMouseEvent release(QEvent::MouseButtonRelease, local, local, Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QCoreApplication::sendEvent(&widget, &release);
}

void sendMove(QWidget& widget, const qreal pixelX) {
    const QPointF local(pixelX, widget.height() / 2.0);
    QMouseEvent move(QEvent::MouseMove, local, local, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&widget, &move);
}

void sendKey(QWidget& widget, const Qt::Key key) {
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
    QCoreApplication::sendEvent(&widget, &press);
}

// 1 second at 24 fps has frame indices 0..23. A ruler resized to 47 logical pixels (width - 1 = 46
// = 2 * 23) makes every EVEN pixel land exactly on a frame and every ODD pixel land exactly at the
// halfway point between two frames -- a deterministic, exactly-representable tie case pinned by
// docs/architecture/animation-and-time.md's tie-to-greater rule.
void testRulerScrubLandsOnExactFrameTimesIncludingATie(Expectations& expectations) {
    using namespace bloom;
    SessionFixture fixture(makeTestProject("Ruler Scrub Test", time(1)));
    expectations.expect(waitUntil([&] {
                            return fixture.controller.state().activity !=
                                   ui::PreviewActivity::Rendering;
                        }),
                        "the initial preview leaves the Rendering activity before scrubbing");

    ui::TimelineRuler ruler(fixture.session, fixture.controller);
    ruler.resize(47, 26);

    sendClick(ruler, 0.0);
    expectations.expect(fixture.session.currentTime() == time(0, 24),
                        "the leftmost pixel scrubs to frame 0");

    sendClick(ruler, 2.0);
    expectations.expect(fixture.session.currentTime() == time(1, 24),
                        "an even pixel scrubs to its exact frame time");

    sendClick(ruler, 1.0);
    expectations.expect(fixture.session.currentTime() == time(1, 24),
                        "an exact pixel-space halfway tie scrubs to the GREATER frame index");

    sendClick(ruler, 45.0);
    expectations.expect(fixture.session.currentTime() == time(23, 24),
                        "the tie immediately below the final frame also resolves to the greater "
                        "index");

    sendClick(ruler, 46.0);
    expectations.expect(fixture.session.currentTime() == time(23, 24),
                        "the rightmost pixel scrubs to the final valid frame, never the excluded "
                        "duration endpoint");

    // A press/move/release drag lands on the final move's exact frame time, and release calls
    // scrub-end (verified indirectly: the request still reaches Ready without ever hanging).
    sendPress(ruler, 0.0);
    QMouseEvent move(QEvent::MouseMove, QPointF(20.0, ruler.height() / 2.0),
                     QPointF(20.0, ruler.height() / 2.0), Qt::NoButton, Qt::LeftButton,
                     Qt::NoModifier);
    QCoreApplication::sendEvent(&ruler, &move);
    expectations.expect(fixture.session.currentTime() == time(10, 24),
                        "a pointer move during a drag scrubs to its own exact frame time");
    sendRelease(ruler, 20.0);
    expectations.expect(
        waitUntil(
            [&] { return fixture.controller.state().activity == ui::PreviewActivity::Ready; }),
        "scrub-end still reaches a ready preview frame for the final scrubbed time");

    fixture.controller.beginShutdown();
    fixture.bridge.beginShutdown();
    expectations.expect(waitUntil([&] { return fixture.scheduler.isQuiescent(); }),
                        "the ruler fixture reaches asynchronous scheduler quiescence");
}

void testKeyframeRowsAppearOnePerAnimatedParameter(Expectations& expectations) {
    using namespace bloom;
    auto newProject = makeTestProject("Keyframe Rows Test", time(10));
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    const auto ids = addSolidLayer(document, commands, compositionId);
    (void)animateParameter(document, commands, compositionId, ids.position, time(0));
    (void)animateParameter(document, commands, compositionId, ids.opacity, time(0));

    ui::CompositionSession session(document, commands, compositionId);
    ui::TimelineKeyframePanel panel(session);
    expectations.expect(panel.findChildren<QWidget*>().empty() && !panel.isVisible(),
                        "no rows appear before any layer is selected");

    session.selectLayer(ids.layer);
    const auto rows = panel.findChildren<QWidget*>();
    expectations.expect(rows.size() == 2 && panel.isVisible(),
                        "one row per animated parameter (position, opacity) appears for the "
                        "selected layer");

    session.clearSelection();
    expectations.expect(panel.findChildren<QWidget*>().empty() && !panel.isVisible(),
                        "rows are removed and the panel hides once nothing is selected");
}

// Selection unification (issue #84, decision 1): a keyframe click no longer sets TimelineEditor-
// local state -- it calls CompositionSession::selectKeyframe(), which REPLACES the primary
// selection like every other select* method, and central CompositionSession::normalizeSelection()
// (run after every session-mediated execute/undo/redo) prunes it once the key no longer resolves,
// without the panel needing any local prune pass.
void testKeyframeClickSelectsByIdAndOneTruthSelectionSwap(Expectations& expectations) {
    using namespace bloom;
    const auto duration = time(10);
    auto newProject = makeTestProject("Keyframe Click Test", duration);
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    const auto ids = addSolidLayer(document, commands, compositionId);
    const auto curveId = animateParameter(document, commands, compositionId, ids.opacity, time(0));

    // A second exact-time key, five seconds into a ten second composition -- its pixel position
    // (fraction 0.5 of the width) is easy to reason about independent of the row's own painting.
    // Inserted directly through the CommandStack (not through a not-yet-constructed session) so
    // CompositionSession's very first cached snapshot already contains it.
    {
        commands::Transaction insertSecond("Insert second opacity key",
                                           document.snapshot().revision());
        insertSecond.emplace<commands::InsertScalarKeyframe>(compositionId, curveId, time(5), 0.4);
        expectations.expect(commands.execute(std::move(insertSecond)).changed(),
                            "fixture inserts a second exact-time opacity key");
    }
    const auto* opacityCurve = document.snapshot()
                                   .project()
                                   .findComposition(compositionId)
                                   ->animationCurves()
                                   .findScalar(curveId);
    expectations.expect(opacityCurve != nullptr && opacityCurve->keyframes.size() == 2,
                        "the opacity curve now holds exactly two keys");
    if (opacityCurve == nullptr || opacityCurve->keyframes.size() != 2) {
        return;
    }
    const auto secondKeyId = opacityCurve->keyframes.back().id;

    ui::CompositionSession session(document, commands, compositionId);
    session.selectLayer(ids.layer);
    expectations.expect(std::holds_alternative<document::LayerId>(session.selection().primary),
                        "the layer starts out as the primary selection");

    ui::TimelineKeyframePanel panel(session);
    panel.resize(200, panel.sizeHint().height() > 0 ? panel.sizeHint().height() : 24);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    const auto rows = panel.findChildren<QWidget*>();
    expectations.expect(rows.size() == 1, "exactly one row exists for the single animated opacity");
    if (rows.size() != 1) {
        return;
    }
    auto* row = rows.front();

    // fraction = 5s / 10s = 0.5; pixel = 0.5 * (width - 1), matching TimelineAxis::pixelForTime.
    const qreal expectedPixel = 0.5 * static_cast<qreal>(row->width() - 1);
    sendClick(*row, expectedPixel);
    const auto* firstClickSelection =
        std::get_if<ui::KeyframeSelection>(&session.selection().primary);
    expectations.expect(firstClickSelection != nullptr && firstClickSelection->curveId == curveId &&
                            firstClickSelection->keyframeId == secondKeyId,
                        "a click at the key's expected pixel position selects it by curve+key id, "
                        "REPLACING the layer as the primary selection");

    // The one-truth swap holds in both directions: selecting the layer again replaces the
    // keyframe, and the keyframe can then replace the layer again.
    session.selectLayer(ids.layer);
    expectations.expect(std::holds_alternative<document::LayerId>(session.selection().primary),
                        "reselecting the layer REPLACES the keyframe as the primary selection");
    sendClick(*row, expectedPixel);
    expectations.expect(std::holds_alternative<ui::KeyframeSelection>(session.selection().primary),
                        "the key can be reselected after the layer replaced it");

    // Central prune: undo the very edit that created the selected key. The key vanishes from the
    // document and CompositionSession::normalizeSelection() must clear the now-dangling selection
    // without crashing (the panel keeps no local prune pass of its own).
    expectations.expect(session.undo(), "undoing the second opacity key insertion succeeds");
    expectations.expect(std::holds_alternative<std::monostate>(session.selection().primary),
                        "the selection is pruned centrally once its key no longer exists");

    session.selectLayer(ids.layer);
    const auto rowsAfterPrune = panel.findChildren<QWidget*>();
    expectations.expect(rowsAfterPrune.size() == 1,
                        "the opacity row remains (the curve still holds its seeded first key)");
}

// Delete gesture (issue #84, decision 2): Delete/Backspace on the focused panel executes ONE
// DeleteKeyframe transaction through CompositionSession::deleteSelectedKeyframe(); the command
// layer's last-key refusal (DeleteKeyframe rejects a curve's final key -- src/commands/
// animation_operations.cpp) is pinned with no state change.
void testDeleteGestureRemovesKeyAndRefusesTheLastOne(Expectations& expectations) {
    using namespace bloom;
    auto newProject = makeTestProject("Keyframe Delete Test", time(10));
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    const auto ids = addSolidLayer(document, commands, compositionId);
    const auto curveId = animateParameter(document, commands, compositionId, ids.opacity, time(0));

    document::KeyframeId extraKeyId;
    {
        commands::Transaction insertExtra("Insert extra opacity key",
                                          document.snapshot().revision());
        insertExtra.emplace<commands::InsertScalarKeyframe>(compositionId, curveId, time(2), 0.3);
        const auto inserted = commands.execute(std::move(insertExtra));
        const auto id = inserted.outputId<document::KeyframeId>(commands::kKeyframeOutput);
        expectations.expect(inserted.changed() && id.has_value(),
                            "fixture inserts a second exact-time opacity key");
        if (!id.has_value()) {
            return;
        }
        extraKeyId = *id;
    }

    ui::CompositionSession session(document, commands, compositionId);
    session.selectLayer(ids.layer);
    ui::TimelineKeyframePanel panel(session);

    session.selectKeyframe(curveId, extraKeyId);
    const auto* selected = std::get_if<ui::KeyframeSelection>(&session.selection().primary);
    expectations.expect(selected != nullptr && selected->keyframeId == extraKeyId,
                        "the extra key is selected before the delete gesture");

    const auto historyBeforeDelete = commands.size();
    sendKey(panel, Qt::Key_Delete);
    const auto* curveAfterDelete = session.composition()->animationCurves().findScalar(curveId);
    expectations.expect(curveAfterDelete != nullptr && curveAfterDelete->keyframes.size() == 1,
                        "Delete on the focused panel removes the selected key in one transaction");
    expectations.expect(commands.size() == historyBeforeDelete + 1,
                        "the delete gesture is exactly one undoable transaction");
    expectations.expect(std::holds_alternative<std::monostate>(session.selection().primary),
                        "the selection is cleared once its key is deleted");

    expectations.expect(session.undo(), "the delete gesture undoes cleanly");
    const auto* curveAfterUndo = session.composition()->animationCurves().findScalar(curveId);
    if (curveAfterUndo == nullptr) {
        expectations.expect(false, "the opacity curve remains addressable after undo");
        return;
    }
    const auto restored =
        std::ranges::find(curveAfterUndo->keyframes, extraKeyId, &document::ScalarKeyframe::id);
    expectations.expect(curveAfterUndo->keyframes.size() == 2 &&
                            restored != curveAfterUndo->keyframes.end() &&
                            restored->time == time(2) && restored->value == 0.3,
                        "undo restores the deleted key with the SAME KeyframeId and exact time");

    // Bring the curve back down to its single seeded key, then pin the last-key refusal.
    session.selectKeyframe(curveId, extraKeyId);
    expectations.expect(session.deleteSelectedKeyframe(), "the extra key can be deleted again");
    const auto* seedCurve = session.composition()->animationCurves().findScalar(curveId);
    expectations.expect(seedCurve != nullptr && seedCurve->keyframes.size() == 1,
                        "the curve is back down to its single seeded key");
    if (seedCurve == nullptr || seedCurve->keyframes.size() != 1) {
        return;
    }
    const auto seedKeyId = seedCurve->keyframes.front().id;
    const auto seedKeyTime = seedCurve->keyframes.front().time;
    session.selectKeyframe(curveId, seedKeyId);

    const auto historyBeforeRefusal = commands.size();
    sendKey(panel, Qt::Key_Backspace);
    const auto* curveAfterRefusal = session.composition()->animationCurves().findScalar(curveId);
    expectations.expect(curveAfterRefusal != nullptr && curveAfterRefusal->keyframes.size() == 1 &&
                            curveAfterRefusal->keyframes.front().id == seedKeyId &&
                            curveAfterRefusal->keyframes.front().time == seedKeyTime,
                        "Backspace on the curve's last key leaves it completely unchanged");
    expectations.expect(commands.size() == historyBeforeRefusal,
                        "the command layer's last-key refusal creates no transaction");
    const auto* selectionAfterRefusal =
        std::get_if<ui::KeyframeSelection>(&session.selection().primary);
    expectations.expect(selectionAfterRefusal != nullptr &&
                            selectionAfterRefusal->keyframeId == seedKeyId,
                        "a refused delete leaves the selection intact");
}

// Drag-move gesture (issue #84, decision 3). Uses a 1-second/24fps composition and a 461-logical-
// pixel-wide row (width - 1 = 460 = 20 * maxIndex(23)) so that pixel 20*i lands EXACTLY on frame i
// under frameIndexForPixel()'s checked-integer mapping (proved algebraically: product = 20*i*23 =
// 460*i is an exact multiple of span 460, so quotient = i with zero remainder -- the same tie-to-
// greater contract testRulerScrubLandsOnExactFrameTimesIncludingATie() pins for the ruler itself,
// just scaled up by 10x so drag deltas clear QApplication::startDragDistance() on every platform).
// PRESS positions instead use the forward, continuous pixelForTime() mapping to hit a key at its
// own exact time.
void testDragMoveGestureSnapsCommitsUndoesAndRefuses(Expectations& expectations) {
    using namespace bloom;
    auto newProject = makeTestProject("Keyframe Drag Test", time(1));
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    const auto ids = addSolidLayer(document, commands, compositionId);
    // Seeds k0 at frame 0 (t=0, value 1.0, Linear) -- the occupied-time refusal target below.
    const auto curveId = animateParameter(document, commands, compositionId, ids.opacity, time(0));

    // A frame-exact key (frame 20) that the drag gesture never touches. Deliberately inserted
    // FIRST and kept the LATEST key in the curve throughout this whole test (every drag target
    // below stays below frame 20), so it -- not dragKeyId -- absorbs the command layer's
    // documented "final key's outgoing interpolation is always normalized to Linear" rule
    // (docs/architecture/animation-and-time.md). Inserting it before dragKeyId matters: that rule
    // fires on every mutation, including insertion, so dragKeyId must never be momentarily the
    // final key or its own requested Hold interpolation below would be silently normalized away.
    document::KeyframeId sentinelKeyId;
    {
        commands::Transaction insertSentinel("Insert sentinel opacity key",
                                             document.snapshot().revision());
        insertSentinel.emplace<commands::InsertScalarKeyframe>(compositionId, curveId, time(20, 24),
                                                               0.6);
        const auto inserted = commands.execute(std::move(insertSentinel));
        const auto id = inserted.outputId<document::KeyframeId>(commands::kKeyframeOutput);
        expectations.expect(inserted.changed() && id.has_value(),
                            "fixture inserts the sentinel key");
        if (!id.has_value()) {
            return;
        }
        sentinelKeyId = *id;
    }

    document::KeyframeId dragKeyId;
    {
        // A subframe key (t=1/3), interior to [k0, sentinel], with Hold interpolation distinct
        // from every other key's kind, so "value and interpolation unchanged" is a real assertion
        // rather than a coincidence.
        commands::Transaction insertDrag("Insert subframe opacity key",
                                         document.snapshot().revision());
        insertDrag.emplace<commands::InsertScalarKeyframe>(compositionId, curveId, time(1, 3), 0.4,
                                                           document::KeyframeInterpolation::Hold);
        const auto inserted = commands.execute(std::move(insertDrag));
        const auto id = inserted.outputId<document::KeyframeId>(commands::kKeyframeOutput);
        expectations.expect(inserted.changed() && id.has_value(), "fixture inserts the drag key");
        if (!id.has_value()) {
            return;
        }
        dragKeyId = *id;
    }

    ui::CompositionSession session(document, commands, compositionId);
    session.selectLayer(ids.layer);
    ui::TimelineKeyframePanel panel(session);
    panel.resize(461, panel.sizeHint().height() > 0 ? panel.sizeHint().height() : 24);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    const auto rows = panel.findChildren<QWidget*>();
    expectations.expect(rows.size() == 1, "exactly one row exists for the single animated opacity");
    if (rows.size() != 1) {
        return;
    }
    auto* row = rows.front();
    expectations.expect(row->width() == 461, "the row matches the panel's exact pixel width");

    // Forward (continuous) pixelForTime mapping, duration 1s, width 461 (width - 1 = 460): used
    // only to compute PRESS positions that hit a key at its own exact time.
    const auto pressPixelForFraction = [](const double numerator, const double denominator) {
        return 460.0 * numerator / denominator;
    };
    const qreal dragKeyPixel = pressPixelForFraction(1.0, 3.0);    // t = 1/3
    const qreal sentinelPixel = pressPixelForFraction(20.0, 24.0); // frame 20
    // Reverse (tie-to-greater) frameIndexForPixel mapping: pixel 20*i lands exactly on frame i (see
    // the file comment above). Frame 2 stays interior to [k0, sentinel] so the drag key's Hold
    // interpolation is never swept up in the command layer's final-key Linear normalization.
    constexpr qreal kFrame0Pixel = 0.0;
    constexpr qreal kFrame2Pixel = 40.0;

    // Select and confirm the sentinel is untouched by everything below.
    sendClick(*row, sentinelPixel);
    const auto* sentinelSelection =
        std::get_if<ui::KeyframeSelection>(&session.selection().primary);
    expectations.expect(sentinelSelection != nullptr &&
                            sentinelSelection->keyframeId == sentinelKeyId,
                        "the sentinel key can be selected by its own exact-time pixel");

    // Select the drag key and move it to frame 2 (t = 2/24).
    sendClick(*row, dragKeyPixel);
    const auto* dragSelection = std::get_if<ui::KeyframeSelection>(&session.selection().primary);
    expectations.expect(dragSelection != nullptr && dragSelection->keyframeId == dragKeyId,
                        "the subframe drag key is selected by its own exact-time pixel");

    const auto historyBeforeMove = commands.size();
    sendPress(*row, dragKeyPixel);
    sendMove(*row, kFrame2Pixel);
    sendRelease(*row, kFrame2Pixel);

    auto findKey = [&](const document::KeyframeId id) -> std::optional<document::ScalarKeyframe> {
        const auto* curve = session.composition()->animationCurves().findScalar(curveId);
        if (curve == nullptr) {
            return std::nullopt;
        }
        const auto found = std::ranges::find(curve->keyframes, id, &document::ScalarKeyframe::id);
        return found == curve->keyframes.end() ? std::nullopt
                                               : std::optional<document::ScalarKeyframe>(*found);
    };

    auto dragged = findKey(dragKeyId);
    expectations.expect(dragged.has_value() && dragged->time == time(2, 24) &&
                            dragged->value == 0.4 &&
                            dragged->outgoingInterpolation == document::KeyframeInterpolation::Hold,
                        "release snaps the drag key to the exact target frame time, preserving "
                        "its existing value and interpolation");
    expectations.expect(commands.size() == historyBeforeMove + 1,
                        "the drag-move gesture is exactly one undoable transaction");
    auto sentinelAfterMove = findKey(sentinelKeyId);
    expectations.expect(sentinelAfterMove.has_value() && sentinelAfterMove->time == time(20, 24) &&
                            sentinelAfterMove->value == 0.6,
                        "an untouched key elsewhere on the lane keeps its exact time through the "
                        "move");

    expectations.expect(session.undo(), "the drag-move gesture undoes cleanly");
    auto draggedAfterUndo = findKey(dragKeyId);
    expectations.expect(draggedAfterUndo.has_value() && draggedAfterUndo->time == time(1, 3) &&
                            draggedAfterUndo->value == 0.4 &&
                            draggedAfterUndo->outgoingInterpolation ==
                                document::KeyframeInterpolation::Hold,
                        "undo restores the exact original SUBFRAME time of the same key");
    expectations.expect(session.redo(), "the drag-move gesture redoes cleanly");
    dragged = findKey(dragKeyId);
    expectations.expect(dragged.has_value() && dragged->time == time(2, 24),
                        "redo restores the moved key at its exact target frame time");

    // Duplicate-time refusal: drag the (now frame-2) key onto frame 0, occupied by the seed key.
    const auto historyBeforeRefusal = commands.size();
    sendPress(*row, kFrame2Pixel);
    sendMove(*row, kFrame0Pixel);
    sendRelease(*row, kFrame0Pixel);
    dragged = findKey(dragKeyId);
    expectations.expect(dragged.has_value() && dragged->time == time(2, 24),
                        "a duplicate-time drag target is refused: the key does not move");
    expectations.expect(commands.size() == historyBeforeRefusal,
                        "the duplicate-time refusal creates no transaction");
    const auto* selectionAfterRefusal =
        std::get_if<ui::KeyframeSelection>(&session.selection().primary);
    expectations.expect(selectionAfterRefusal != nullptr &&
                            selectionAfterRefusal->keyframeId == dragKeyId,
                        "a refused move leaves the selection intact");

    // Zero-displacement: drag away and back to the SAME snapped frame before releasing.
    const auto historyBeforeZero = commands.size();
    sendPress(*row, kFrame2Pixel);
    sendMove(*row, kFrame0Pixel);
    sendMove(*row, kFrame2Pixel);
    sendRelease(*row, kFrame2Pixel);
    dragged = findKey(dragKeyId);
    expectations.expect(dragged.has_value() && dragged->time == time(2, 24),
                        "releasing back on the key's own exact frame leaves its time unchanged");
    expectations.expect(commands.size() == historyBeforeZero,
                        "a zero-displacement release commits nothing");

    // Escape mid-drag cancels with no transaction.
    const auto historyBeforeEscape = commands.size();
    sendPress(*row, kFrame2Pixel);
    sendMove(*row, kFrame0Pixel);
    sendKey(*row, Qt::Key_Escape);
    dragged = findKey(dragKeyId);
    expectations.expect(dragged.has_value() && dragged->time == time(2, 24),
                        "Escape mid-drag leaves the key's time unchanged");
    expectations.expect(commands.size() == historyBeforeEscape,
                        "Escape mid-drag creates no transaction");
    // A stray release after Escape must be inert (the row's drag state was already cleared).
    sendRelease(*row, kFrame0Pixel);
    dragged = findKey(dragKeyId);
    expectations.expect(dragged.has_value() && dragged->time == time(2, 24),
                        "a release after Escape has no further effect");
    expectations.expect(commands.size() == historyBeforeEscape,
                        "a release after Escape creates no transaction");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    Expectations expectations;
    testRulerScrubLandsOnExactFrameTimesIncludingATie(expectations);
    testKeyframeRowsAppearOnePerAnimatedParameter(expectations);
    testKeyframeClickSelectsByIdAndOneTruthSelectionSwap(expectations);
    testDeleteGestureRemovesKeyAndRefusesTheLastOne(expectations);
    testDragMoveGestureSnapsCommitsUndoesAndRefuses(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
