// Tests for direct viewer manipulation of layer position (docs/architecture/animation-and-time.md,
// "Direct Manipulation And Preview Overrides"; issue #82, task D1):
//  - CompositionPreviewController's override threading (attaches only to Interactive requests built
//    while an interaction is armed; never to Visible; gone after commit/cancel; an
//    admission-rejected override surfaces the error state without killing the interaction).
//  - ViewerEditor's synthesized-mouse gesture (press/move/release, empty/unselected no-op, a
//    mid-drag resize cancels).
// Mirrors composition_preview_controller_tests.cpp's PipelineFixture/Expectations/waitUntil idiom
// and timeline_ruler_tests.cpp's synthesized-mouse-event idiom.

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/project.hpp>
#include <bloom/render/display_buffer.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/viewer_editor.hpp>

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointF>
#include <QResizeEvent>
#include <QSize>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <source_location>
#include <string>
#include <thread>
#include <utility>
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

bloom::document::CompositionFormat wideFormat() {
    const auto format = bloom::document::CompositionFormat::create(1000, 500);
    if (!format.has_value()) {
        std::abort();
    }
    return *format;
}

bloom::document::NewProject makeTestProject(std::string projectName) {
    return bloom::document::makeNewProject(
        std::move(projectName), "Main", bloom::core::RationalTime::fromInteger(10), wideFormat());
}

struct LayerIds final {
    bloom::document::LayerId layer;
    bloom::document::ParameterId position;
};

// Adds the layer directly through the CommandStack BEFORE any CompositionSession exists, so the
// session's constructor-time snapshot() already reflects it (a session that already exists caches
// its own snapshot_ and only refreshes it through its own execute()/undo()/redo(); mutating the
// stack behind its back would leave it stale).
[[nodiscard]] LayerIds addSolidLayer(bloom::document::Document& document,
                                     bloom::commands::CommandStack& stack,
                                     const bloom::document::CompositionId compositionId,
                                     const bloom::document::Vec2d position) {
    using namespace bloom;
    commands::Transaction transaction("Add test layer", document.snapshot().revision());
    transaction.emplace<commands::AddSolidLayer>(compositionId, "Solid",
                                                 core::Color4d{0.2, 0.3, 0.4, 1.0}, position);
    const auto result = stack.execute(std::move(transaction));
    const auto layer = result.outputId<document::LayerId>(commands::kAddSolidLayerLayerOutput);
    const auto positionId =
        result.outputId<document::ParameterId>(commands::kAddSolidLayerPositionParameterOutput);
    if (!(result.changed() && layer.has_value() && positionId.has_value())) {
        std::abort();
    }
    return {*layer, *positionId};
}

struct PipelineFixture final {
    bloom::runtime::NodeDefinitionRegistry definitions;
    bloom::runtime::SnapshotCompiler compiler;
    bloom::runtime::CpuCompositionEvaluator evaluator;
    bloom::runtime::CpuReferenceDisplayPreparer displayPreparer;
    // Never published to Ready in this fixture (issue #97, task C3): direct manipulation only
    // needs the geometry mapping, which is alternative-agnostic; keeping this pipeline on the
    // reference path is unrelated to what this test exercises.
    bloom::runtime::QualifiedDisplayProcessorProvider qualifiedProcessorProvider;
    bloom::ui::PreviewPreparationFunction pipeline;

    PipelineFixture() : compiler(definitions) {
        if (!bloom::runtime::registerBuiltInNodeDefinitions(definitions)) {
            std::abort();
        }
        definitions.freeze();
        pipeline = bloom::ui::makeCompositionPreviewPipeline(compiler, evaluator, displayPreparer,
                                                             qualifiedProcessorProvider);
    }
};

bool isReady(const bloom::ui::CompositionPreviewController& controller) {
    return controller.state().activity == bloom::ui::PreviewActivity::Ready;
}

std::uint64_t currentGeneration(const bloom::ui::CompositionPreviewController& controller) {
    const auto& desiredIdentity = controller.state().desiredIdentity;
    return desiredIdentity.has_value() ? desiredIdentity->requestGeneration : 0;
}

void reachQuiescence(bloom::ui::CompositionPreviewController& controller,
                     bloom::ui::TaskUiBridge& bridge, bloom::runtime::TaskScheduler& scheduler,
                     Expectations& expectations) {
    bool quiescentSignal = false;
    QObject::connect(&bridge, &bloom::ui::TaskUiBridge::shutdownQuiescent, &bridge,
                     [&quiescentSignal] { quiescentSignal = true; });
    controller.beginShutdown();
    bridge.beginShutdown();
    expectations.expect(waitUntil([&] { return quiescentSignal && scheduler.isQuiescent(); }),
                        "direct manipulation fixture reaches asynchronous scheduler quiescence");
}

// ---------------------------------------------------------------------------------------------
// Controller override threading
// ---------------------------------------------------------------------------------------------

// docs/architecture/animation-and-time.md: the fitted display rectangle a real gesture would use is
// ViewerEditor's job (covered by the gesture tests below); here a manually built mapping is enough
// to prove the CONTROLLER threads whatever the session reports.
bloom::ui::PositionInteractionMapping makeMapping(const bloom::document::CompositionFormat format) {
    using namespace bloom;
    const auto window = render::ImageWindow::create(0, 0, format.width(), format.height());
    if (!window) {
        std::abort();
    }
    const auto descriptor =
        render::ReferenceDisplayBufferDescriptor::create(*window.value(), format.pixelAspect());
    if (!descriptor) {
        std::abort();
    }
    return ui::PositionInteractionMapping{
        .displayRect = QRectF(0.0, 0.0, 200.0, 100.0),
        .compositionFormat = format,
        .resolution = runtime::CompositionFormatResolution{},
        .pixelAspect = format.pixelAspect(),
        .displayDescriptor = *descriptor.value(),
    };
}

void testControllerAttachesOverrideOnlyToArmedInteractiveRequests(Expectations& expectations) {
    using namespace bloom;
    auto newProject = makeTestProject("Controller Override Threading");
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    const auto ids = addSolidLayer(document, commands, compositionId, document::Vec2d{10.0, 20.0});
    ui::CompositionSession session(document, commands, compositionId);
    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);
    PipelineFixture fixture;

    struct Invocation final {
        std::uint64_t generation = 0;
        bool hasOverride = false;
        std::optional<document::Vec2d> overrideValue;
    };
    std::mutex invocationMutex;
    std::vector<Invocation> invocations;
    ui::CompositionPreviewController controller(
        session, scheduler, bridge,
        [&invocationMutex, &invocations, pipeline = fixture.pipeline](
            const document::Snapshot& snapshot,
            const runtime::PreviewRequestIdentity& desiredIdentity,
            const std::size_t pixelStorageByteLimit,
            const std::optional<runtime::SnapshotParameterOverride>& interactionOverride,
            runtime::TaskContext& context) mutable {
            {
                std::scoped_lock lock(invocationMutex);
                std::optional<document::Vec2d> value;
                if (interactionOverride.has_value()) {
                    if (const auto* vector =
                            std::get_if<document::Vec2d>(&interactionOverride->value)) {
                        value = *vector;
                    }
                }
                invocations.push_back(
                    {desiredIdentity.requestGeneration, interactionOverride.has_value(), value});
            }
            return pipeline(snapshot, desiredIdentity, pixelStorageByteLimit, interactionOverride,
                            context);
        });

    const auto invocationForGeneration =
        [&](const std::uint64_t generation) -> std::optional<Invocation> {
        std::scoped_lock lock(invocationMutex);
        for (const auto& invocation : invocations) {
            if (invocation.generation == generation) {
                return invocation;
            }
        }
        return std::nullopt;
    };

    expectations.expect(waitUntil([&] { return isReady(controller); }),
                        "the initial Visible frame becomes ready");
    {
        const auto initial = invocationForGeneration(currentGeneration(controller));
        expectations.expect(initial.has_value() && !initial->hasOverride,
                            "the initial Visible request (no interaction yet) carries no override");
    }

    session.selectLayer(ids.layer);
    const auto mapping = makeMapping(wideFormat());
    controller.beginInteractiveScrub();
    expectations.expect(!session.beginPositionInteraction(mapping).has_value(),
                        "the interaction begins for the selected layer");
    {
        const auto generation = currentGeneration(controller);
        expectations.expect(waitUntil([&] { return isReady(controller); }),
                            "the armed begin's Interactive request reaches Ready");
        const auto invocation = invocationForGeneration(generation);
        expectations.expect(
            invocation.has_value() && invocation->hasOverride &&
                invocation->overrideValue == document::Vec2d{10.0, 20.0},
            "an armed interaction attaches the (unmoved) override to its Interactive "
            "request build");
    }

    session.updatePositionInteraction(40.0, 20.0);
    {
        const auto generation = currentGeneration(controller);
        expectations.expect(waitUntil([&] { return isReady(controller); }),
                            "the updated Interactive request reaches Ready");
        const auto invocation = invocationForGeneration(generation);
        const document::Vec2d expected{10.0 + 40.0 / 200.0 * 1000.0, 20.0 + 20.0 / 100.0 * 500.0};
        expectations.expect(invocation.has_value() && invocation->hasOverride &&
                                invocation->overrideValue == expected,
                            "the update's Interactive request carries the freshly recomputed "
                            "override (read fresh, not cached)");
    }

    // A Visible request bypasses the override entirely, even while the interaction is still active.
    controller.requestRefresh();
    {
        const auto generation = currentGeneration(controller);
        expectations.expect(waitUntil([&] { return isReady(controller); }),
                            "the Visible refresh reaches Ready");
        const auto invocation = invocationForGeneration(generation);
        expectations.expect(invocation.has_value() && !invocation->hasOverride,
                            "a Visible request never carries the active interaction's override");
        expectations.expect(session.positionInteractionActive(),
                            "the Visible refresh does not disturb the still-active interaction");
    }

    // Commit clears the interaction; the next (still-armed) Interactive-priority request carries no
    // override.
    expectations.expect(session.commitPositionInteraction(), "commit succeeds");
    controller.notifyScrubEnded();
    expectations.expect(waitUntil([&] { return isReady(controller); }),
                        "the post-commit request reaches Ready");
    expectations.expect(!session.positionInteractionOverride().has_value(),
                        "the override is gone after commit");

    reachQuiescence(controller, bridge, scheduler, expectations);
}

void testAdmissionRejectedOverrideSurfacesErrorWithoutKillingInteraction(
    Expectations& expectations) {
    using namespace bloom;
    auto newProject = makeTestProject("Admission Rejected Override");
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    const auto ids = addSolidLayer(document, commands, compositionId, document::Vec2d{0.0, 0.0});
    ui::CompositionSession session(document, commands, compositionId);
    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);
    PipelineFixture fixture;

    // Simulates a SnapshotCompiler admission rejection (InvalidParameterOverride /
    // UnsupportedParameterOverride) without needing to engineer a genuine one: whenever the
    // controller threads a non-null override, the pipeline reports Failed, exactly like a real
    // rejected compile would surface through the existing plan-state/diagnostics projection.
    ui::CompositionPreviewController controller(
        session, scheduler, bridge,
        [pipeline = fixture.pipeline](
            const document::Snapshot& snapshot,
            const runtime::PreviewRequestIdentity& desiredIdentity,
            const std::size_t pixelStorageByteLimit,
            const std::optional<runtime::SnapshotParameterOverride>& interactionOverride,
            runtime::TaskContext& context) mutable {
            if (interactionOverride.has_value()) {
                return runtime::TaskResult<ui::PreviewPreparationResultHandle>::failed(
                    {.code = "bloom.preview.test-override-rejected",
                     .severity = runtime::DiagnosticSeverity::Error,
                     .summary = "Simulated parameter override admission rejection",
                     .detail = {},
                     .suggestedAction = {}});
            }
            return pipeline(snapshot, desiredIdentity, pixelStorageByteLimit, interactionOverride,
                            context);
        });

    expectations.expect(waitUntil([&] { return isReady(controller); }),
                        "the initial (override-free) frame becomes ready");

    session.selectLayer(ids.layer);
    const auto mapping = makeMapping(wideFormat());
    controller.beginInteractiveScrub();
    expectations.expect(!session.beginPositionInteraction(mapping).has_value(), "begin succeeds");
    session.updatePositionInteraction(10.0, 10.0);

    expectations.expect(
        waitUntil([&] { return controller.state().activity == ui::PreviewActivity::Failed; }),
        "the rejected override surfaces as a Failed preview, never a crash");
    expectations.expect(session.positionInteractionActive(),
                        "an admission-rejected override never kills the live interaction -- the "
                        "gesture stays draggable");

    session.cancelPositionInteraction();
    controller.notifyScrubEnded();
    expectations.expect(waitUntil([&] { return isReady(controller); }),
                        "clearing the override recovers a Ready preview");

    reachQuiescence(controller, bridge, scheduler, expectations);
}

// ---------------------------------------------------------------------------------------------
// Viewer gesture (synthesized mouse)
// ---------------------------------------------------------------------------------------------

void sendMouse(QWidget& widget, const QEvent::Type type, const QPointF& local,
               const Qt::MouseButton button, const Qt::MouseButtons buttons) {
    QMouseEvent mouseEvent(type, local, local, button, buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(&widget, &mouseEvent);
}

void sendPress(QWidget& widget, const QPointF& local) {
    sendMouse(widget, QEvent::MouseButtonPress, local, Qt::LeftButton, Qt::LeftButton);
}

void sendMove(QWidget& widget, const QPointF& local) {
    sendMouse(widget, QEvent::MouseMove, local, Qt::NoButton, Qt::LeftButton);
}

void sendRelease(QWidget& widget, const QPointF& local) {
    sendMouse(widget, QEvent::MouseButtonRelease, local, Qt::LeftButton, Qt::NoButton);
}

// Mirrors ViewerEditor::currentMapping()'s display-rect derivation so the test can compute the
// expected composition displacement the same way production code does, without hardcoding it.
//
// Task U3 (issue #119), decision 2, THE SEAM: canvasRect() replaces the pre-U3 28px-inset
// viewerFrame() (the canvas is now full-bleed, minus only the bottom status bar strip -- see
// ViewerEditor::canvasRect()/statusBarRect() in viewer_editor.cpp), and
// viewTransformedDisplayRect(..., transform) replaces the unconditional fitDisplayRect() call --
// the frozen mapping rectangle now reflects the ACTIVE view transform at gesture begin, not always
// the fit-to-window rectangle. `transform` defaults to the identity Fit transform, so every
// pre-existing call site below (all at implicit zoom 1x/pan 0) is unchanged in behavior.
QRectF expectedDisplayRect(const QWidget& viewer,
                           const bloom::ui::CompositionPreviewController& controller,
                           const bloom::ui::ViewTransform& transform = {}) {
    using namespace bloom;
    const auto& frame = controller.state().frame;
    if (frame == nullptr) {
        return {};
    }
    const auto viewResult = frame->displayBuffer().view();
    if (!viewResult) {
        return {};
    }
    const auto view = *viewResult.value();
    const auto descriptorResult = view.descriptor();
    if (!descriptorResult.has_value()) {
        return {};
    }
    const qreal statusBarHeight = ui::kit::px(ui::kit::Size::Control);
    const QRectF frameRect = QRectF(viewer.rect()).adjusted(0.0, 0.0, 0.0, -statusBarHeight);
    return ui::viewTransformedDisplayRect(frameRect, descriptorResult->displayWindow().extent(),
                                          descriptorResult->pixelAspect(), transform);
}

// document::Document is non-movable/non-copyable (its constructor and snapshot() own the live
// project truth), so the fixture owns document/commands/session end to end and any test-specific
// layer is added afterward through the session's own addSolidLayer() -- the real command surface,
// which also keeps session.snapshot() correctly caching what it just created (see addSolidLayer()
// above for why mutating a CommandStack behind an existing session's back is the wrong shape).
struct GestureFixture final {
    bloom::document::Document document;
    bloom::commands::CommandStack commands;
    bloom::ui::CompositionSession session;
    bloom::runtime::TaskScheduler scheduler;
    bloom::ui::TaskUiBridge bridge;
    PipelineFixture pipeline;
    bloom::ui::CompositionPreviewController controller;
    bloom::ui::ViewerEditor viewer;

    explicit GestureFixture(bloom::document::NewProject newProject)
        : document(std::move(newProject.project)), commands(document),
          session(document, commands, newProject.initialCompositionId),
          scheduler(testSchedulerConfig()), bridge(scheduler, nullptr, 1ms),
          controller(session, scheduler, bridge, pipeline.pipeline), viewer(session, controller) {
        viewer.resize(400, 300);
    }
};

void testDragMovesSelectedSolidLayerCommitsAndUndoes(Expectations& expectations) {
    using namespace bloom;
    GestureFixture fixture(makeTestProject("Viewer Drag Test"));
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the initial frame becomes ready before dragging");
    // addSolidLayer() places the new layer at the composition center (1000x500 -> (500, 250)) and
    // selects it, exactly like a real "Add Solid Layer" action would.
    expectations.expect(
        fixture.session.addSolidLayer(QStringLiteral("Solid"), core::Color4d{0.2, 0.3, 0.4, 1.0}),
        "the fixture layer is added and selected");
    // addSolidLayer() advances the document revision, which makes the frame currentMapping() would
    // use stale until a fresh Ready frame for the new revision arrives.
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the frame becomes ready again for the post-add revision");
    const auto* layerId = std::get_if<document::LayerId>(&fixture.session.selection().primary);
    expectations.expect(layerId != nullptr, "addSolidLayer() leaves the new layer selected");
    if (layerId == nullptr) {
        return;
    }
    const auto* position = fixture.session.parameterForSelection(document::kPositionParameterRole);
    expectations.expect(position != nullptr, "the new solid layer exposes a position parameter");
    if (position == nullptr) {
        return;
    }
    const auto positionId = position->id;
    const auto base = fixture.session.constantVec2Value(positionId);
    expectations.expect(base.has_value(),
                        "the new layer's position starts as a resolvable constant");
    if (!base.has_value()) {
        return;
    }

    const QRectF displayRect = expectedDisplayRect(fixture.viewer, fixture.controller);
    expectations.expect(!displayRect.isEmpty(), "the fixture's display rectangle is non-empty");

    const QPointF pressPoint(200.0, 150.0);
    const QPointF releasePoint(260.0, 110.0);
    const QPointF delta = releasePoint - pressPoint;

    const auto revisionBeforeDrag = fixture.session.snapshot().revision();
    sendPress(fixture.viewer, pressPoint);
    expectations.expect(fixture.session.positionInteractionActive(),
                        "pressing on the viewer with a selected layer begins the interaction");
    sendMove(fixture.viewer, releasePoint);
    sendRelease(fixture.viewer, releasePoint);

    expectations.expect(!fixture.session.positionInteractionActive(),
                        "release ends the interaction");
    expectations.expect(fixture.session.snapshot().revision().value() ==
                            revisionBeforeDrag.value() + 1,
                        "release commits exactly one document transaction -- one undo step");

    const document::Vec2d expected{base->x + delta.x() / displayRect.width() * 1000.0,
                                   base->y + delta.y() / displayRect.height() * 500.0};
    expectations.expect(fixture.session.constantVec2Value(positionId) == expected,
                        "the committed position matches base plus the screen-to-composition "
                        "mapped total displacement");

    expectations.expect(fixture.session.undo(), "the drag's commit undoes cleanly");
    expectations.expect(fixture.session.constantVec2Value(positionId) == *base,
                        "undo restores the exact pre-drag position");

    fixture.controller.beginShutdown();
    fixture.bridge.beginShutdown();
    expectations.expect(waitUntil([&] { return fixture.scheduler.isQuiescent(); }),
                        "the drag fixture reaches asynchronous scheduler quiescence");
}

// Selects the dropdown item whose visible text is `label` (never hardcodes an index -- the fixed
// Fit/25/50/100/200/400 order is ViewerEditor's own construction detail, not this test's).
void selectZoomPreset(bloom::ui::kit::KDropdown& dropdown, const QString& label) {
    for (int index = 0; index < dropdown.count(); ++index) {
        if (dropdown.itemText(index) == label) {
            dropdown.setCurrentIndex(index);
            return;
        }
    }
    std::abort();
}

// THE SEAM, pinned directly (task U3, issue #119, decision 2): a direct-manipulation drag begun at
// a NON-IDENTITY zoom (200%, no fit-to-window) still lands exactly under the cursor. Before this
// task, ViewerEditor::currentMapping() always froze fitDisplayRect()'s rectangle regardless of any
// zoom; this pins that the frozen mapping now reflects the ACTIVE zoom instead -- at 200% a screen
// displacement maps to HALF the composition displacement it would at Fit/100%, because the same
// document extent now occupies twice the screen pixels.
void testDragAtNonIdentityZoomLandsExactlyUnderCursor(Expectations& expectations) {
    using namespace bloom;
    GestureFixture fixture(makeTestProject("Viewer Drag Zoomed Test"));
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the initial frame becomes ready before dragging");
    expectations.expect(
        fixture.session.addSolidLayer(QStringLiteral("Solid"), core::Color4d{0.2, 0.3, 0.4, 1.0}),
        "the fixture layer is added and selected");
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the frame becomes ready again for the post-add revision");
    const auto* position = fixture.session.parameterForSelection(document::kPositionParameterRole);
    expectations.expect(position != nullptr, "the new solid layer exposes a position parameter");
    if (position == nullptr) {
        return;
    }
    const auto positionId = position->id;
    const auto base = fixture.session.constantVec2Value(positionId);
    expectations.expect(base.has_value(),
                        "the new layer's position starts as a resolvable constant");
    if (!base.has_value()) {
        return;
    }

    selectZoomPreset(*fixture.viewer.zoomDropdownForTest(), QStringLiteral("200%"));
    expectations.expect(!fixture.viewer.viewTransformForTest().fitToWindow &&
                            fixture.viewer.viewTransformForTest().zoom == 2.0,
                        "the fixture is now zoomed to 200%, out of Fit mode");

    const QRectF displayRect = expectedDisplayRect(fixture.viewer, fixture.controller,
                                                   fixture.viewer.viewTransformForTest());
    expectations.expect(!displayRect.isEmpty() && displayRect.width() > 1000.0,
                        "at 200% the display rectangle is visibly larger than the 1000-wide "
                        "composition itself -- this test is genuinely exercising a non-identity "
                        "zoom, not accidentally still at Fit's own scale");

    const QPointF pressPoint(200.0, 150.0);
    const QPointF releasePoint(260.0, 110.0);
    const QPointF delta = releasePoint - pressPoint;

    sendPress(fixture.viewer, pressPoint);
    expectations.expect(
        fixture.session.positionInteractionActive(),
        "pressing on the zoomed viewer with a selected layer begins the interaction");
    // Begin base: the frozen base value is exactly the pre-drag constant, unmoved, before any move
    // event lands.
    const auto beginOverride = fixture.session.positionInteractionOverride();
    expectations.expect(beginOverride.has_value(), "the begun interaction carries an override");
    if (beginOverride.has_value()) {
        const auto* beginValue = std::get_if<document::Vec2d>(&beginOverride->value);
        expectations.expect(beginValue != nullptr && *beginValue == *base,
                            "the interaction's begin-base document-space position is the exact "
                            "unmoved pre-drag constant, regardless of zoom");
    }
    sendMove(fixture.viewer, releasePoint);
    sendRelease(fixture.viewer, releasePoint);

    expectations.expect(!fixture.session.positionInteractionActive(),
                        "release ends the interaction");

    // Committed key: the document-space position lands exactly where the ZOOMED mapping (half the
    // Fit-implied displacement, since the same screen delta now covers less document distance at
    // 200%) says it should -- never the Fit-derived value the pre-U3 seam would have produced.
    const document::Vec2d expectedAtZoom{base->x + delta.x() / displayRect.width() * 1000.0,
                                         base->y + delta.y() / displayRect.height() * 500.0};
    const document::Vec2d expectedAtFit{base->x + delta.x() / 1000.0 * 1000.0,
                                        base->y + delta.y() / 500.0 * 500.0};
    expectations.expect(fixture.session.constantVec2Value(positionId) == expectedAtZoom,
                        "the committed document-space position matches base plus the ZOOM-AWARE "
                        "screen-to-composition mapped displacement");
    expectations.expect(fixture.session.constantVec2Value(positionId) != expectedAtFit,
                        "the committed position is genuinely different from what the pre-U3 "
                        "always-Fit seam would have produced -- proving the seam change actually "
                        "changed the mapping geometry, not just its call signature");

    fixture.controller.beginShutdown();
    fixture.bridge.beginShutdown();
    expectations.expect(waitUntil([&] { return fixture.scheduler.isQuiescent(); }),
                        "the zoomed-drag fixture reaches asynchronous scheduler quiescence");
}

// THE SEAM, combined with a non-zero pan (task U3, issue #119, decision 2): a direct-manipulation
// drag begun at 200% zoom AND a panned view still lands exactly under the cursor. Pan only
// translates the frozen rectangle's position, never its size, so the SAME zoom-derived
// composition-per-screen-pixel ratio as the zoom-only test above applies unchanged; this pins that
// panning (itself driven through the real space-drag gesture, not by reaching into private state)
// does not perturb the mapping's scale, only where the gesture reads its frozen displayRect from.
void testDragAtNonIdentityZoomAndPanLandsExactlyUnderCursor(Expectations& expectations) {
    using namespace bloom;
    GestureFixture fixture(makeTestProject("Viewer Drag Zoomed Panned Test"));
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the initial frame becomes ready before dragging");
    expectations.expect(
        fixture.session.addSolidLayer(QStringLiteral("Solid"), core::Color4d{0.2, 0.3, 0.4, 1.0}),
        "the fixture layer is added and selected");
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the frame becomes ready again for the post-add revision");
    const auto* position = fixture.session.parameterForSelection(document::kPositionParameterRole);
    expectations.expect(position != nullptr, "the new solid layer exposes a position parameter");
    if (position == nullptr) {
        return;
    }
    const auto positionId = position->id;
    const auto base = fixture.session.constantVec2Value(positionId);
    expectations.expect(base.has_value(),
                        "the new layer's position starts as a resolvable constant");
    if (!base.has_value()) {
        return;
    }

    selectZoomPreset(*fixture.viewer.zoomDropdownForTest(), QStringLiteral("200%"));

    // Pan through the real space-drag gesture (mirrors viewer_editor_tests.cpp's own space-pan
    // test) rather than reaching into ViewerEditor's private transform_ -- the resulting pan offset
    // is read back afterward via the same public test accessor used to compute the expected
    // mapping below, so this test never needs to duplicate the pan materialization formula.
    QKeyEvent spaceDown(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
    QCoreApplication::sendEvent(&fixture.viewer, &spaceDown);
    sendPress(fixture.viewer, QPointF(40.0, 40.0));
    sendMove(fixture.viewer, QPointF(75.0, 15.0));
    sendRelease(fixture.viewer, QPointF(75.0, 15.0));
    QKeyEvent spaceUp(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
    QCoreApplication::sendEvent(&fixture.viewer, &spaceUp);

    const auto transform = fixture.viewer.viewTransformForTest();
    expectations.expect(!transform.fitToWindow && transform.zoom == 2.0 &&
                            (transform.pan.x() != 0.0 || transform.pan.y() != 0.0),
                        "the fixture is now both zoomed to 200% and panned by a non-zero offset");

    const QRectF displayRect = expectedDisplayRect(fixture.viewer, fixture.controller, transform);
    expectations.expect(!displayRect.isEmpty(), "the fixture's zoomed+panned display rectangle is "
                                                "non-empty");

    const QPointF pressPoint(220.0, 160.0);
    const QPointF releasePoint(190.0, 205.0);
    const QPointF delta = releasePoint - pressPoint;

    sendPress(fixture.viewer, pressPoint);
    expectations.expect(fixture.session.positionInteractionActive(),
                        "pressing on the zoomed+panned viewer with a selected layer begins the "
                        "interaction");
    const auto beginOverride = fixture.session.positionInteractionOverride();
    if (beginOverride.has_value()) {
        const auto* beginValue = std::get_if<document::Vec2d>(&beginOverride->value);
        expectations.expect(beginValue != nullptr && *beginValue == *base,
                            "the interaction's begin-base document-space position is unaffected "
                            "by zoom or pan");
    }
    sendMove(fixture.viewer, releasePoint);
    sendRelease(fixture.viewer, releasePoint);
    expectations.expect(!fixture.session.positionInteractionActive(),
                        "release ends the interaction");

    const document::Vec2d expected{base->x + delta.x() / displayRect.width() * 1000.0,
                                   base->y + delta.y() / displayRect.height() * 500.0};
    expectations.expect(fixture.session.constantVec2Value(positionId) == expected,
                        "the committed document-space position matches base plus the "
                        "zoom-AND-pan-aware screen-to-composition mapped displacement");

    fixture.controller.beginShutdown();
    fixture.bridge.beginShutdown();
    expectations.expect(waitUntil([&] { return fixture.scheduler.isQuiescent(); }),
                        "the zoomed-and-panned-drag fixture reaches asynchronous scheduler "
                        "quiescence");
}

void testDragOnEmptyOrUnselectedDoesNothing(Expectations& expectations) {
    using namespace bloom;
    GestureFixture fixture(makeTestProject("Viewer Drag Unselected Test"));
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the initial frame becomes ready");
    expectations.expect(
        fixture.session.addSolidLayer(QStringLiteral("Solid"), core::Color4d{0.2, 0.3, 0.4, 1.0}),
        "the fixture layer is added");
    // A layer exists in the composition, but nothing is selected -- matching an empty/unselected
    // viewer, not merely an empty composition.
    fixture.session.clearSelection();
    const auto revisionBeforeDrag = fixture.session.snapshot().revision();

    sendPress(fixture.viewer, QPointF(200.0, 150.0));
    expectations.expect(!fixture.session.positionInteractionActive(),
                        "pressing with nothing selected never begins an interaction");
    sendMove(fixture.viewer, QPointF(260.0, 110.0));
    sendRelease(fixture.viewer, QPointF(260.0, 110.0));

    expectations.expect(fixture.session.snapshot().revision() == revisionBeforeDrag,
                        "a drag on an empty/unselected viewer creates no command");

    fixture.controller.beginShutdown();
    fixture.bridge.beginShutdown();
    expectations.expect(waitUntil([&] { return fixture.scheduler.isQuiescent(); }),
                        "the unselected-drag fixture reaches asynchronous scheduler quiescence");
}

void testMidDragResizeCancelsWithNoCommitAndNoOverrideLeft(Expectations& expectations) {
    using namespace bloom;
    GestureFixture fixture(makeTestProject("Viewer Drag Resize Test"));
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the initial frame becomes ready before dragging");
    expectations.expect(
        fixture.session.addSolidLayer(QStringLiteral("Solid"), core::Color4d{0.2, 0.3, 0.4, 1.0}),
        "the fixture layer is added and selected");
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the frame becomes ready again for the post-add revision");
    const auto* position = fixture.session.parameterForSelection(document::kPositionParameterRole);
    expectations.expect(position != nullptr, "the new solid layer exposes a position parameter");
    if (position == nullptr) {
        return;
    }
    const auto positionId = position->id;
    const auto base = fixture.session.constantVec2Value(positionId);
    const auto revisionBeforeDrag = fixture.session.snapshot().revision();

    sendPress(fixture.viewer, QPointF(200.0, 150.0));
    sendMove(fixture.viewer, QPointF(230.0, 170.0));
    expectations.expect(fixture.session.positionInteractionActive() &&
                            fixture.session.positionInteractionOverride().has_value(),
                        "the interaction is live and overriding mid-drag");

    // resize() alone only updates geometry; QWidget may defer dispatching the resizeEvent()
    // override rather than sending it synchronously, so send one directly -- matching this file's
    // synthesized-event idiom for presses/moves/releases/Escape -- to deterministically exercise
    // ViewerEditor::resizeEvent() within this statement.
    const QSize oldSize = fixture.viewer.size();
    fixture.viewer.resize(250, 200);
    QResizeEvent resize(fixture.viewer.size(), oldSize);
    QCoreApplication::sendEvent(&fixture.viewer, &resize);
    expectations.expect(!fixture.session.positionInteractionActive(),
                        "a mid-drag resize cancels the interaction");
    expectations.expect(!fixture.session.positionInteractionOverride().has_value(),
                        "no override is left after the resize cancels it");

    // The release that follows is a no-op: the gesture already ended at the resize.
    sendRelease(fixture.viewer, QPointF(230.0, 170.0));
    expectations.expect(fixture.session.snapshot().revision() == revisionBeforeDrag,
                        "a mid-drag resize commits nothing");
    expectations.expect(fixture.session.constantVec2Value(positionId) == base,
                        "the position is untouched by the cancelled drag");

    fixture.controller.beginShutdown();
    fixture.bridge.beginShutdown();
    expectations.expect(waitUntil([&] { return fixture.scheduler.isQuiescent(); }),
                        "the resize-cancel fixture reaches asynchronous scheduler quiescence");
}

void testEscapeCancelsMidDrag(Expectations& expectations) {
    using namespace bloom;
    GestureFixture fixture(makeTestProject("Viewer Drag Escape Test"));
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the initial frame becomes ready before dragging");
    expectations.expect(
        fixture.session.addSolidLayer(QStringLiteral("Solid"), core::Color4d{0.2, 0.3, 0.4, 1.0}),
        "the fixture layer is added and selected");
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the frame becomes ready again for the post-add revision");
    const auto revisionBeforeDrag = fixture.session.snapshot().revision();

    sendPress(fixture.viewer, QPointF(200.0, 150.0));
    sendMove(fixture.viewer, QPointF(220.0, 160.0));
    expectations.expect(fixture.session.positionInteractionActive(), "the interaction is live");

    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(&fixture.viewer, &escape);
    expectations.expect(!fixture.session.positionInteractionActive(),
                        "Escape cancels the live interaction");
    expectations.expect(fixture.session.snapshot().revision() == revisionBeforeDrag,
                        "Escape commits nothing");

    fixture.controller.beginShutdown();
    fixture.bridge.beginShutdown();
    expectations.expect(waitUntil([&] { return fixture.scheduler.isQuiescent(); }),
                        "the escape-cancel fixture reaches asynchronous scheduler quiescence");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    Expectations expectations;
    testControllerAttachesOverrideOnlyToArmedInteractiveRequests(expectations);
    testAdmissionRejectedOverrideSurfacesErrorWithoutKillingInteraction(expectations);
    testDragMovesSelectedSolidLayerCommitsAndUndoes(expectations);
    testDragAtNonIdentityZoomLandsExactlyUnderCursor(expectations);
    testDragAtNonIdentityZoomAndPanLandsExactlyUnderCursor(expectations);
    testDragOnEmptyOrUnselectedDoesNothing(expectations);
    testMidDragResizeCancelsWithNoCommitAndNoOverrideLeft(expectations);
    testEscapeCancelsMidDrag(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
