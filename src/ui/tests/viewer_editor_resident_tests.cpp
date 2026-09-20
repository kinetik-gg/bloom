// Actual ViewerEditor tests for the prepared GPU-resident integration. These
// build the real CompositionSession/CompositionPreviewController pipeline and
// construct a real ViewerEditor, then prove: the EditorNativeSurface gate is
// inert with no live target, a CPU (non-resident) frame is never mistaken for a
// resident one, the CPU paint path is retained, and an injected private port
// seam configures the controller without a device. The real-device
// first-present/ack path is the separate native test.

#include <bloom/ui/viewer_editor.hpp>

#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/commands/command_stack.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/qualified_display_processor_provider.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <bloom/render/display_buffer.hpp>
#include <bloom/render/gpu_present_image.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/runtime/gpu_presentation_coordinator.hpp>
#include <bloom/runtime/prepared_preview_frame.hpp>
#include <bloom/ui/viewer_gpu_resident.hpp>

#include "viewer_gpu_presenter_port.hpp"

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QImage>
#include <QPainter>
#include <QPixmap>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

namespace {

using namespace std::chrono_literals;
using namespace bloom;

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << "Failure: " << message << '\n';
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

// Counts MouseButtonPress events delivered to the widget it filters, proving native input is
// re-dispatched through Qt (which runs receiver event filters) rather than only the handler.
class PressFilter final : public QObject {
  public:
    int presses = 0;

  protected:
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::MouseButtonPress) {
            ++presses;
        }
        return false;
    }
};

// A port that reports no usable borrowed instance, so the presenter stays
// Unsupported and no native target is created. Used only to prove the
// controller is configured and inert without a device.
class UnsupportedPort final : public ui::ViewerGpuPort {
  public:
    [[nodiscard]] render::GpuBorrowedInstanceView instanceView() const override { return {}; }
    [[nodiscard]] runtime::GpuPresentationPortResult attach(const render::GpuBorrowedSurface&,
                                                            std::uint32_t, std::uint32_t) override {
        runtime::GpuPresentationPortResult result;
        result.code = runtime::GpuPresentationPortCode::Rejected;
        return result;
    }
    [[nodiscard]] runtime::GpuPresentationPortResult
    update(runtime::GpuPresentationTargetId, std::uint64_t,
           runtime::GpuPresentationUpdate) override {
        runtime::GpuPresentationPortResult result;
        result.code = runtime::GpuPresentationPortCode::Rejected;
        return result;
    }
    [[nodiscard]] runtime::GpuPresentationPortResult
    resize(runtime::GpuPresentationTargetId, std::uint64_t, std::uint32_t, std::uint32_t) override {
        runtime::GpuPresentationPortResult result;
        result.code = runtime::GpuPresentationPortCode::Rejected;
        return result;
    }
    [[nodiscard]] runtime::GpuPresentationPortResult retire(runtime::GpuPresentationTargetId,
                                                            std::uint64_t) override {
        runtime::GpuPresentationPortResult result;
        result.code = runtime::GpuPresentationPortCode::Rejected;
        return result;
    }
    [[nodiscard]] runtime::GpuPresentationPortResult
    forget(runtime::GpuPresentationTargetId) override {
        runtime::GpuPresentationPortResult result;
        result.code = runtime::GpuPresentationPortCode::Rejected;
        return result;
    }
    [[nodiscard]] runtime::GpuPresentationTargetSnapshot
    status(runtime::GpuPresentationTargetId) const override {
        return {};
    }
    [[nodiscard]] bool ownerAlive() const noexcept override { return true; }
};

document::CompositionFormat smallFormat() {
    const auto format = document::CompositionFormat::create(4, 3);
    if (!format.has_value()) {
        std::abort();
    }
    return *format;
}

document::NewProject makeTestProject(std::string projectName) {
    return document::makeNewProject(std::move(projectName), "Main",
                                    core::RationalTime::fromInteger(10), smallFormat());
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

void testViewerEditorResidentGateIsInertAndKeepsCpuPaint(Expectations& expectations) {
    auto newProject = makeTestProject("Viewer GPU resident prep");
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);

    runtime::NodeDefinitionRegistry definitions;
    expectations.expect(runtime::registerBuiltInNodeDefinitions(definitions),
                        "the fixture registers built-in node definitions");
    definitions.freeze();
    runtime::SnapshotCompiler compiler(definitions);
    const runtime::CpuCompositionEvaluator evaluator;
    const runtime::CpuReferenceDisplayPreparer displayPreparer;
    runtime::QualifiedDisplayProcessorProvider qualifiedProvider;
    auto pipeline =
        ui::makeCompositionPreviewPipeline(compiler, evaluator, displayPreparer, qualifiedProvider);
    ui::CompositionPreviewController controller(session, scheduler, bridge, pipeline);
    ui::ViewerEditor viewer(session, controller);
    viewer.resize(320, 240);
    viewer.show();

    expectations.expect(waitUntil([&] {
                            return controller.state().activity == ui::PreviewActivity::Ready &&
                                   controller.state().frame != nullptr;
                        }),
                        "a CPU frame becomes ready in the real viewer fixture");

    // 1. No client: the integration is inert and the EditorNativeSurface gate is
    // a no-op.
    expectations.expect(!viewer.gpuResidentConfiguredForTest(),
                        "the viewer GPU route is unconfigured without an injected client");
    expectations.expect(!viewer.hasLiveNativeTarget(), "no live native target is claimed");
    bool called = false;
    bool safe = false;
    const auto outcome = viewer.prepareNativeSurfaceMutation(
        3, [&](const std::uint64_t generation, const ui::EditorNativeSurface::PrepareResult& r) {
            called = true;
            safe = r.safeToMutate;
            expectations.expect(generation == 3, "the generation is echoed");
        });
    expectations.expect(outcome == ui::EditorNativeSurface::PrepareOutcome::NoLiveTarget,
                        "the host may mutate synchronously with no live target");
    expectations.expect(called && safe, "the completion reports safe-to-mutate");

    // 2. The ready frame is a CPU arm: it must never be mistaken for resident
    // geometry.
    const auto frame = viewer.displayedFrameForTest();
    expectations.expect(frame != nullptr, "the viewer has a displayed frame");
    if (frame != nullptr) {
        expectations.expect(frame->provenance().provider !=
                                runtime::PreviewDisplayProvider::GpuResident,
                            "the ready frame is a CPU arm, not the resident arm");
        expectations.expect(!ui::residentFrameGeometry(*frame).has_value(),
                            "no resident geometry is inferred from a CPU frame");
    }

    // 3. The CPU paint path is retained (a real offscreen grab is non-null).
    const QImage cpuImage = viewer.grab().toImage();
    expectations.expect(!cpuImage.isNull(), "the viewer paints its CPU frame offscreen");

    // 3b. The actual ViewerEditor cover handoff pixmap renders the real CPU
    // content (not a blank or recursively captured native child).
    const QPixmap cover = viewer.renderCpuCoverSnapshotForTest();
    expectations.expect(!cover.isNull(), "the actual ViewerEditor cover handoff pixmap renders");
    if (!cover.isNull()) {
        const QImage coverImage = cover.toImage();
        const QColor center =
            coverImage.pixelColor(coverImage.width() / 2, coverImage.height() / 2);
        expectations.expect(center.alpha() > 0,
                            "the cover pixmap is not blank (real CPU content present)");
    }

    // 4. Injecting the private port seam configures the controller but creates no
    // live target, because the ready frame is not resident and no present is
    // attempted.
    viewer.setGpuPresentationPortForTest(std::make_shared<UnsupportedPort>());
    expectations.expect(viewer.gpuResidentConfiguredForTest(),
                        "the injected port configures the controller");
    viewer.pollGpuResidentForTest();
    expectations.expect(!viewer.hasLiveNativeTarget(),
                        "no live target is created for a non-resident frame");
    expectations.expect(!viewer.residentPresentationActiveForTest(),
                        "resident presentation is never claimed for a CPU frame");
    const QImage afterImage = viewer.grab().toImage();
    expectations.expect(!afterImage.isNull(), "the CPU paint survives the port injection");

    // 5. Resume-after-mutation is safe on an unchanged tree.
    viewer.resumeNativeSurfaceAfterMutation();
    expectations.expect(!viewer.hasLiveNativeTarget(), "resume leaves no live target");

    // 6. A stale CPU fallback completion (different identity) is never accepted as current.
    runtime::PreviewRequestIdentity older;
    older.requestGeneration = 1;
    runtime::PreviewRequestIdentity newer;
    newer.requestGeneration = 2;
    expectations.expect(!ui::cpuFallbackCompletionIsCurrent(older, newer),
                        "a stale fallback completion is not current");
    expectations.expect(ui::cpuFallbackCompletionIsCurrent(newer, newer),
                        "the matching fallback completion is current");

    // 7. Native-present input is re-dispatched through Qt, so a receiver event filter (the
    // workspace's panel-activation filter) sees it. A direct mousePressEvent() call would bypass
    // that filter entirely and leave the panel inactive even though the click landed.
    PressFilter pressFilter;
    viewer.installEventFilter(&pressFilter);
    ui::ViewerGpuInputEvent press;
    press.kind = ui::ViewerGpuInputKind::MousePress;
    press.local = QPointF(12.0, 9.0);
    press.global = QPointF(12.0, 9.0);
    press.button = Qt::LeftButton;
    press.buttons = Qt::LeftButton;
    viewer.forwardGpuInputForTest(press);
    expectations.expect(pressFilter.presses == 1,
                        "forwarded native input runs receiver event filters, not just the handler");

    // 8. With no live target the host may mutate synchronously; the completion is answered in the
    // call (never queued, so a later request cannot supersede it).
    bool externalCalled = false;
    const auto externalOutcome = viewer.prepareNativeSurfaceMutation(
        30, [&](const std::uint64_t generation, const ui::EditorNativeSurface::PrepareResult&) {
            expectations.expect(generation == 30, "the no-live-target generation is echoed");
            externalCalled = true;
        });
    expectations.expect(externalOutcome == ui::EditorNativeSurface::PrepareOutcome::NoLiveTarget,
                        "with no live target the host may mutate synchronously");
    expectations.expect(externalCalled,
                        "the no-live-target completion is answered synchronously, not queued");

    // 8b. While an EXTERNAL retirement is already pending, a duplicate request must be refused
    // WITHOUT bumping the completion generation: superseding the first queued completion would hang
    // the host gate.
    viewer.simulateExternalRetireInFlightForTest();
    const std::uint64_t externalGeneration = viewer.hostMutationGenerationForTest();
    bool firstDuplicateCalled = false;
    expectations.expect(
        viewer.prepareNativeSurfaceMutation(
            50,
            [&](const std::uint64_t, const ui::EditorNativeSurface::PrepareResult&) {
                firstDuplicateCalled = true;
            }) == ui::EditorNativeSurface::PrepareOutcome::Refused,
        "a duplicate external request is refused while one is already pending");
    bool secondDuplicateCalled = false;
    expectations.expect(
        viewer.prepareNativeSurfaceMutation(
            51,
            [&](const std::uint64_t, const ui::EditorNativeSurface::PrepareResult&) {
                secondDuplicateCalled = true;
            }) == ui::EditorNativeSurface::PrepareOutcome::Refused,
        "a further duplicate external request is also refused");
    expectations.expect(viewer.hostMutationGenerationForTest() == externalGeneration,
                        "a refused duplicate never advances the external completion generation");
    expectations.expect(!firstDuplicateCalled && !secondDuplicateCalled,
                        "refused duplicates are never answered as safe");
    viewer.resumeNativeSurfaceAfterMutation();
    expectations.expect(viewer.hostMutationGenerationForTest() != externalGeneration,
                        "an external resume invalidates stale queued completions");

    // 9. While an internal blank retirement owns the presenter's retire slot, a host request folds
    // into it (RetirePending) and is answered later with the truthful result; a second concurrent
    // request is refused explicitly rather than coalesced or answered as safe.
    viewer.simulateNativeRetireInFlightForTest();
    bool foldedCalled = false;
    bool foldedSafe = false;
    const auto folded = viewer.prepareNativeSurfaceMutation(
        40, [&](const std::uint64_t generation, const ui::EditorNativeSurface::PrepareResult& r) {
            expectations.expect(generation == 40, "the folded generation is echoed");
            foldedCalled = true;
            foldedSafe = r.safeToMutate;
        });
    expectations.expect(folded == ui::EditorNativeSurface::PrepareOutcome::RetirePending,
                        "the host request folds into the in-flight internal retirement");
    expectations.expect(!foldedCalled, "the folded completion is never delivered inline");
    bool secondCalled = false;
    const auto second = viewer.prepareNativeSurfaceMutation(
        41, [&](const std::uint64_t, const ui::EditorNativeSurface::PrepareResult&) {
            secondCalled = true;
        });
    expectations.expect(second == ui::EditorNativeSurface::PrepareOutcome::Refused,
                        "a second concurrent host request is refused explicitly");
    expectations.expect(!secondCalled, "the refused request is never answered as safe");
    viewer.finishSimulatedNativeRetireForTest(true, "simulated retire");
    expectations.expect(foldedCalled && foldedSafe,
                        "the folded completion is delivered once with the truthful safe result");
    expectations.expect(!secondCalled, "the refused request stays unanswered");
    QApplication::processEvents();

    controller.beginShutdown();
    bridge.beginShutdown();
    (void)waitUntil([&] { return scheduler.isQuiescent(); });
}

void testResidentFrameGeometryResolvesTheSameMappingDescriptor(Expectations& expectations) {
    // Direct manipulation resolves its display descriptor from EITHER the CPU packed buffer view OR
    // the GPU-resident lease's immutable geometry. A resident frame has no CPU buffer
    // (displayBufferView() is nullopt by construction), so currentMapping() previously refused
    // every resident gesture; this pins that the resident arm resolves geometry-only, with no
    // readback and no fabricated buffer. A synthetic ResidentFrameGeometry is used because the
    // production resident lease requires a real GPU device, but the descriptor depends only on the
    // geometry the lease exposes.
    const auto extent = render::ImageExtent::create(1920, 1080);
    const auto window = render::ImageWindow::create(-2, 5, 1920, 1080);
    expectations.expect(extent.hasValue() && window.hasValue(), "fixture geometry is valid");
    if (!extent.hasValue() || !window.hasValue()) {
        return;
    }
    const auto pixelAspect = core::PixelAspectRatio::square();
    const auto expected =
        render::ReferenceDisplayBufferDescriptor::create(*window.value(), pixelAspect);
    expectations.expect(expected.hasValue(), "the fixture descriptor is valid");
    if (!expected.hasValue()) {
        return;
    }

    const runtime::PreviewDisplayBufferView cpuView{
        .displayWindow = *window.value(),
        .pixelAspect = pixelAspect,
        .layout = render::PackedImageLayout{0, 0, 0},
        .pixels = {},
        .isOcioQualified = false,
    };

    const ui::ResidentFrameGeometry residentGeometry{
        .displayExtent = *extent.value(),
        .displayWindow = *window.value(),
        .pixelAspect = pixelAspect,
        .lease = {},
    };

    const auto fromCpu = ui::viewerDisplayDescriptorForFrame(cpuView, std::nullopt);
    const auto fromResident = ui::viewerDisplayDescriptorForFrame(std::nullopt, residentGeometry);
    expectations.expect(fromCpu.has_value() && fromResident.has_value(),
                        "both the CPU buffer and the resident lease resolve a descriptor");
    if (fromCpu.has_value() && fromResident.has_value()) {
        expectations.expect(*fromCpu == *fromResident,
                            "resident geometry maps exactly like the equivalent CPU buffer view");
        expectations.expect(*fromResident == *expected.value(),
                            "the resident descriptor is the lease's own geometry, not a readback");
    }
    expectations.expect(
        !ui::viewerDisplayDescriptorForFrame(std::nullopt, std::nullopt).has_value(),
        "neither source yields no descriptor");
}

void testCompositionFrameChromePaintsWithoutADevice(Expectations& expectations) {
    // The resident overlay is the only paint on the GPU route; it records the same composition
    // frame border and empty-state invitation the CPU path draws. Both are painted here with no
    // device, so the shared helpers can never silently stop emitting visible ink.
    const QRectF displayRect(30.0, 20.0, 180.0, 100.0);
    const QRectF canvasRect(0.0, 0.0, 240.0, 140.0);

    QImage canvas(240, 140, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);
    QPainter painter(&canvas);
    ui::paintViewerCompositionFrame(painter, displayRect);
    ui::paintViewerEmptyInvitation(painter, canvasRect, QStringLiteral("Create a layer to begin"));
    painter.end();
    expectations.expect(canvas.pixelColor(30, 20).alpha() > 0,
                        "the composition frame border paints visible ink");

    bool invitationInk = false;
    for (int y = 40; y < 100 && !invitationInk; ++y) {
        for (int x = 40; x < 200; ++x) {
            if (canvas.pixelColor(x, y).alpha() > 0) {
                invitationInk = true;
                break;
            }
        }
    }
    expectations.expect(invitationInk, "the empty-state invitation paints visible ink");

    QImage bare(240, 140, QImage::Format_ARGB32_Premultiplied);
    bare.fill(Qt::transparent);
    QPainter barePainter(&bare);
    ui::paintViewerCompositionFrame(barePainter, displayRect);
    ui::paintViewerEmptyInvitation(barePainter, canvasRect, QString());
    barePainter.end();
    bool interiorInk = false;
    for (int y = 40; y < 100 && !interiorInk; ++y) {
        for (int x = 40; x < 200; ++x) {
            if (bare.pixelColor(x, y).alpha() > 0) {
                interiorInk = true;
                break;
            }
        }
    }
    expectations.expect(!interiorInk,
                        "an empty invitation paints no interior ink over the composition");
}

void testViewerEditorBlankCoverIsOpaqueCurrentCpuPaint(Expectations& expectations) {
    // The native CPU cover raised during a blank/no-frame retirement must be the CURRENT CPU paint
    // (the opaque canvas background), never a transparent or stale frame retained from the previous
    // composition. A prior colored composition is displayed and painted first, so a regression that
    // reuses the last CPU frame is caught; a from-empty test would only catch a null allocation.
    document::Document blankDocument(document::Project(document::ProjectId::fromRaw(7), "Blank"));
    commands::CommandStack blankCommands(blankDocument);
    auto newProject = makeTestProject("Blank cover");
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    expectations.expect(
        session.addSolidLayer(QStringLiteral("Solid"), core::Color4d{0.9, 0.05, 0.85, 1.0}),
        "the fixture authors a colored Solid so the prior frame is distinctive");
    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);

    runtime::NodeDefinitionRegistry definitions;
    expectations.expect(runtime::registerBuiltInNodeDefinitions(definitions),
                        "the fixture registers built-in node definitions");
    definitions.freeze();
    runtime::SnapshotCompiler compiler(definitions);
    const runtime::CpuCompositionEvaluator evaluator;
    const runtime::CpuReferenceDisplayPreparer displayPreparer;
    runtime::QualifiedDisplayProcessorProvider qualifiedProvider;
    auto pipeline =
        ui::makeCompositionPreviewPipeline(compiler, evaluator, displayPreparer, qualifiedProvider);
    ui::CompositionPreviewController controller(session, scheduler, bridge, pipeline);
    ui::ViewerEditor viewer(session, controller);
    viewer.resize(320, 240);
    viewer.show();
    controller.requestRefresh();
    expectations.expect(waitUntil([&] {
                            const auto frame = controller.state().frame;
                            return frame != nullptr &&
                                   frame->provenance().provider !=
                                       runtime::PreviewDisplayProvider::GpuResident;
                        }),
                        "a CPU frame is displayed before the blank rebind");
    static_cast<void>(viewer.grab()); // paints the CPU frame, populating the last-CPU-frame cache
    const QPixmap beforeCover = viewer.renderCpuCoverSnapshotForTest();
    expectations.expect(!beforeCover.isNull(), "the composition cover snapshot renders");

    session.rebind(blankDocument, blankCommands, document::CompositionId{});
    expectations.expect(waitUntil([&] { return viewer.displayedFrameForTest() == nullptr; }),
                        "the composition-less rebind clears the displayed frame");
    QApplication::processEvents();
    const QPixmap afterCover = viewer.renderCpuCoverSnapshotForTest();
    expectations.expect(!afterCover.isNull(), "the blank-state cover snapshot renders");
    if (!beforeCover.isNull() && !afterCover.isNull()) {
        const QImage after = afterCover.toImage();
        expectations.expect(after.pixelColor(after.width() / 2, after.height() / 2).alpha() == 255,
                            "the blank-state cover is fully opaque");
        expectations.expect(
            after != beforeCover.toImage(),
            "the blank-state cover does not retain the previous composition pixels");
    }

    controller.beginShutdown();
    bridge.beginShutdown();
    (void)waitUntil([&] { return scheduler.isQuiescent(); });
}

// Structural regression for the native CPU cover. Qt's QWidget::setAttribute(WA_NativeWindow)
// calls parentWidget()->enforceNativeChildren(), which marks every child of that parent native.
// The cover must therefore never be parented directly to the editor: a dedicated alien host (whose
// only child is the cover) fences that promotion, and the cover sets WA_DontCreateNativeAncestors
// before enabling WA_NativeWindow so the host and the editor chain stay alien. A repeated reveal
// with an unchanged rect must also be stable: same cover object, no extra native children, and the
// cover remains the host's topmost child.
void testNativeCoverPromotionIsFencedAndStable(Expectations& expectations) {
    QWidget root;
    root.setObjectName(QStringLiteral("coverFenceRoot"));
    // Stack children declared after root so they are destroyed before it and detach themselves from
    // the parent chain (safe reverse destruction order); no child is heap-allocated, so an early
    // return from an expectation cannot leak it.
    QWidget editor(&root);
    editor.setObjectName(QStringLiteral("coverFenceEditor"));
    QWidget siblingA(&editor);
    QWidget siblingB(&editor);
    root.resize(240, 180);
    root.show();
    QApplication::processEvents();

    expectations.expect(!editor.testAttribute(Qt::WA_NativeWindow) && editor.internalWinId() == 0,
                        "the editor starts alien before any cover exists");
    expectations.expect(!siblingA.testAttribute(Qt::WA_NativeWindow) &&
                            !siblingB.testAttribute(Qt::WA_NativeWindow),
                        "the editor siblings start alien before any cover exists");

    ui::ViewerGpuResidentController controller;
    ui::ViewerGpuResidentController::Dependencies dependencies;
    dependencies.containerParent = &editor;
    controller.setDependencies(std::move(dependencies));
    controller.setCpuCoverSnapshot([] {
        QPixmap snapshot(8, 6);
        snapshot.fill(Qt::darkRed);
        return snapshot;
    });

    const QRect coverRect(4, 6, 64, 48);
    controller.revealCpuCover(coverRect);
    QApplication::processEvents();

    QWidget* cover = controller.cpuCoverForTest();
    expectations.expect(cover != nullptr, "the CPU cover is constructed on reveal");
    if (cover == nullptr) {
        return;
    }
    expectations.expect(cover->internalWinId() != 0, "the cover itself is native");
    expectations.expect(!editor.testAttribute(Qt::WA_NativeWindow) && editor.internalWinId() == 0,
                        "constructing the cover does not promote the editor native");
    expectations.expect(!siblingA.testAttribute(Qt::WA_NativeWindow) &&
                            !siblingB.testAttribute(Qt::WA_NativeWindow),
                        "constructing the cover does not promote the editor siblings native");

    QWidget* coverHost = cover->parentWidget();
    expectations.expect(coverHost != nullptr && coverHost != &editor,
                        "the cover is fenced behind a dedicated alien host");
    if (coverHost == nullptr || coverHost == &editor) {
        return;
    }
    expectations.expect(!coverHost->testAttribute(Qt::WA_NativeWindow) &&
                            coverHost->internalWinId() == 0,
                        "the fence host itself stays alien");
    const auto childrenBefore = coverHost->findChildren<QWidget*>(Qt::FindDirectChildrenOnly);
    expectations.expect(childrenBefore.size() == 1,
                        "the fence host contains only the native cover as a widget child");

    controller.revealCpuCover(coverRect);
    QApplication::processEvents();
    expectations.expect(controller.cpuCoverForTest() == cover,
                        "a repeated reveal with an unchanged rect reuses the same cover");
    const auto childrenAfter = coverHost->findChildren<QWidget*>(Qt::FindDirectChildrenOnly);
    expectations.expect(childrenAfter.size() == childrenBefore.size(),
                        "a repeated reveal does not add native children to the host");
    expectations.expect(!childrenAfter.isEmpty() && childrenAfter.last() == cover,
                        "the cover stays the host's topmost child after a repeated reveal");

    controller.concealCpuCover();
    QApplication::processEvents();
    expectations.expect(!controller.cpuCoverVisibleForTest(),
                        "concealing hides the CPU cover (and its fence host)");
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    Expectations expectations;
    testViewerEditorResidentGateIsInertAndKeepsCpuPaint(expectations);
    testResidentFrameGeometryResolvesTheSameMappingDescriptor(expectations);
    testCompositionFrameChromePaintsWithoutADevice(expectations);
    testViewerEditorBlankCoverIsOpaqueCurrentCpuPaint(expectations);
    testNativeCoverPromotionIsFencedAndStable(expectations);
    if (expectations.failures() == 0) {
        std::cout << "PASS: actual ViewerEditor resident integration (CPU/inert gate)\n";
        return 0;
    }
    std::cerr << expectations.failures() << " failure(s)\n";
    return 1;
}
