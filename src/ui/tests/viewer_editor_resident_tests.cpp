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

#include <bloom/render/gpu_present_image.hpp>
#include <bloom/runtime/gpu_presentation_coordinator.hpp>

#include "viewer_gpu_presenter_port.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
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

    controller.beginShutdown();
    bridge.beginShutdown();
    (void)waitUntil([&] { return scheduler.isQuiescent(); });
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    Expectations expectations;
    testViewerEditorResidentGateIsInertAndKeepsCpuPaint(expectations);
    if (expectations.failures() == 0) {
        std::cout << "PASS: actual ViewerEditor resident integration (CPU/inert gate)\n";
        return 0;
    }
    std::cerr << expectations.failures() << " failure(s)\n";
    return 1;
}
