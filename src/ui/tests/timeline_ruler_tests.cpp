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

void testKeyframeClickSelectsExpectedPixelAndSurvivesRebuild(Expectations& expectations) {
    using namespace bloom;
    const auto duration = time(10);
    auto newProject = makeTestProject("Keyframe Click Test", duration);
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    const auto ids = addSolidLayer(document, commands, compositionId);
    (void)animateParameter(document, commands, compositionId, ids.opacity, time(0));

    ui::CompositionSession session(document, commands, compositionId);
    session.selectLayer(ids.layer);

    // A second exact-time key, five seconds into a ten second composition -- its pixel position
    // (fraction 0.5 of the width) is easy to reason about independent of the row's own painting.
    expectations.expect(session.setCurrentTime(time(5)), "session accepts the second key's time");
    expectations.expect(session.setSelectedOpacity(0.4), "the second opacity key is inserted");
    const auto* opacityParameter = session.composition()->parameters().find(ids.opacity);
    const auto* animationSource =
        opacityParameter == nullptr
            ? nullptr
            : std::get_if<document::AnimationCurveSource>(&opacityParameter->source);
    const auto* opacityCurve =
        animationSource == nullptr
            ? nullptr
            : session.composition()->animationCurves().findScalar(animationSource->curveId);
    expectations.expect(opacityCurve != nullptr && opacityCurve->keyframes.size() == 2,
                        "the opacity curve now holds exactly two keys");
    if (opacityCurve == nullptr || opacityCurve->keyframes.size() != 2) {
        return;
    }
    const auto secondKeyId = opacityCurve->keyframes.back().id;

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
    expectations.expect(panel.selectedKeyframe().has_value() &&
                            *panel.selectedKeyframe() == secondKeyId,
                        "a click at the key's expected pixel position selects it by id");

    // Rebuild the rows via an unrelated document edit + undo, which leaves the opacity curve
    // untouched: the id-keyed selection must survive both rebuilds.
    expectations.expect(
        session.addTextLayer(QStringLiteral("Unrelated"), QStringLiteral("Unrelated")),
        "an unrelated edit triggers a rebuild via snapshotChanged");
    expectations.expect(panel.selectedKeyframe().has_value() &&
                            *panel.selectedKeyframe() == secondKeyId,
                        "the id-keyed selection survives a rebuild when the key still exists");
    expectations.expect(session.undo(), "the unrelated edit undoes cleanly");
    expectations.expect(panel.selectedKeyframe().has_value() &&
                            *panel.selectedKeyframe() == secondKeyId,
                        "the id-keyed selection also survives the undo's rebuild");

    // Now undo the edit that created the selected key itself: the key no longer exists anywhere,
    // and the panel must silently clear the selection rather than crash.
    expectations.expect(session.undo(), "undoing the second opacity edit removes its key");
    expectations.expect(!panel.selectedKeyframe().has_value(),
                        "the selection is pruned once its key no longer exists, without crashing");

    // AddTextLayer's own command (like AddSolidLayer) moves the primary selection to the layer it
    // creates, and undoing it back out of existence leaves nothing selected (CompositionSession::
    // normalizeSelection()) -- reselect the original layer to isolate this assertion to the row
    // rebuild itself, not session selection semantics covered elsewhere.
    session.selectLayer(ids.layer);
    const auto rowsAfterPrune = panel.findChildren<QWidget*>();
    expectations.expect(rowsAfterPrune.size() == 1,
                        "the opacity row remains (the curve still holds its seeded first key)");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    Expectations expectations;
    testRulerScrubLandsOnExactFrameTimesIncludingATie(expectations);
    testKeyframeRowsAppearOnePerAnimatedParameter(expectations);
    testKeyframeClickSelectsExpectedPixelAndSurvivesRebuild(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
