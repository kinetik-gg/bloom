// ViewerGpuResidentController implementation; Qt/Vulkan interop stays in ViewerGpuPresenter.

#include <bloom/ui/viewer_gpu_resident.hpp>

#include <bloom/ui/viewer_gpu_resident_overlay.hpp>

#include "viewer_gpu_presenter_port.hpp"

#include <bloom/render/gpu_present_image.hpp>

#include "viewer_gpu_resident_cover.hpp"

#include <QWidget>

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace bloom::ui {

bool presentSequenceAcknowledged(const std::uint64_t appliedSequence,
                                 const std::uint64_t enqueuedSequence) noexcept {
    return enqueuedSequence != 0 && appliedSequence >= enqueuedSequence;
}

std::optional<ResidentFrameGeometry>
residentFrameGeometry(const runtime::PreparedPreviewFrame& frame) noexcept {
    // Provenance is the safe discriminator; residentFrame() is only defined for the resident arm.
    if (frame.provenance().provider != runtime::PreviewDisplayProvider::GpuResident) {
        return std::nullopt;
    }
    const auto& resident = frame.residentFrame();
    if (resident == nullptr || !resident->isDisplayValid()) {
        return std::nullopt;
    }
    const auto& lease = resident->lease();
    if (!lease.isValid()) {
        return std::nullopt;
    }
    const auto window = lease.displayWindow();
    if (!window.has_value()) {
        return std::nullopt;
    }
    const auto extent = render::ImageExtent::create(lease.width(), lease.height());
    if (!extent) {
        return std::nullopt;
    }
    return ResidentFrameGeometry{.displayExtent = *extent.value(),
                                 .displayWindow = *window,
                                 .pixelAspect = lease.pixelAspect(),
                                 .lease = lease};
}

namespace {

[[nodiscard]] std::uint32_t deviceExtent(const double logical, const double ratio) noexcept {
    const double safeRatio = ratio > 0.0 ? ratio : 1.0;
    const double device = std::round(logical * safeRatio);
    if (!(device > 0.0) ||
        device > static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
        return 0U;
    }
    return static_cast<std::uint32_t>(device);
}

} // namespace

struct ViewerGpuResidentController::Impl final {
    Dependencies dependencies;
    std::shared_ptr<ViewerGpuPort> port;
    std::unique_ptr<ViewerGpuPresenter> presenter;
    InputSink inputSink;
    PresentAck presentAck;
    CpuFallback cpuFallback;
    CpuCoverSnapshot coverSnapshot;
    // The cover is parented to this alien host so Qt's WA_NativeWindow sibling enforcement only
    // reaches the cover, never the host's parent (the editor) or the editor's other children.
    ViewerGpuCpuCoverHost* coverHost = nullptr;
    ViewerGpuCpuCover* cover = nullptr;
    // True only while a native transition (attach/resize/resume/refusal) needs the CPU cover.
    bool coverRequired = true;
    // True once the cover pixmap holds the current CPU paint. It is captured on the hidden->visible
    // transition only, so a retained/refused transition that re-raises the cover does not
    // re-rasterize the snapshot every poll tick.
    bool coverCaptured = false;
    std::function<void()> stateChanged;
    ViewerGpuPresenter::State lastReportedState = ViewerGpuPresenter::State::Uninitialized;

    // The last request/lease we intend to present. Cheap: an opaque lease plus
    // plain params.
    runtime::GpuResidentFrameLease pendingLease;
    render::GpuPresentImageParams pendingParams;
    std::uint32_t targetWidth = 0;
    std::uint32_t targetHeight = 0;
    std::uint64_t requestGeneration = 0;
    std::uint64_t enqueuedSequence = 0;
    bool presentAcked = false;
    // A present waiting for the target to become Active; enqueued once when presents are admitted.
    bool presentPending = false;
    std::shared_ptr<const runtime::GpuPresentationOverlay> pendingOverlayToPresent;

    // Bounded overlay pipeline: one active raster plus one newest pending; stale completions drop.
    std::optional<runtime::TaskHandle<std::shared_ptr<const runtime::GpuPresentationOverlay>>>
        activeOverlayTask;
    std::uint64_t activeOverlayGeneration = 0;
    std::uint64_t activeOverlayRequestGeneration = 0;
    std::optional<OverlayRasterRequest> newestOverlay;
    std::uint64_t newestOverlayRequestGeneration = 0;
    std::uint64_t nextOverlayToken = 0;
    std::uint64_t presentedOverlaySignature = 0;
    std::shared_ptr<const runtime::GpuPresentationOverlay> presentedOverlay;

    std::size_t presentAttempts = 0;
    std::size_t acceptedPresents = 0;
    std::size_t overlayRasters = 0;
    bool lastResident = false;
    std::string diagnostic;

    [[nodiscard]] bool usable() const noexcept {
        return dependencies.client != nullptr || port != nullptr;
    }

    void failCpu(const std::string& message) {
        diagnostic = message;
        if (cover != nullptr) {
            coverRequired = true;
            showCover();
        }
        if (cpuFallback) {
            cpuFallback();
        }
    }

    // Returns true only on the conceal->reveal transition that actually showed and restacked the
    // cover. A cover that is already visible is left completely untouched.
    [[nodiscard]] bool ensureCover(const QRect& rect) {
        if (!coverSnapshot) {
            return false;
        }
        if (coverHost == nullptr) {
            coverHost = new ViewerGpuCpuCoverHost(dependencies.containerParent);
        }
        if (coverHost->geometry() != rect) {
            coverHost->setGeometry(rect);
        }
        if (cover == nullptr) {
            cover = new ViewerGpuCpuCover(coverHost);
            cover->setObjectName(QStringLiteral("bloomViewerGpuCpuCover"));
            coverCaptured = false;
        }
        const QRect local(QPoint(0, 0), rect.size());
        if (cover->geometry() != local) {
            cover->setGeometry(local);
        }
        // Snapshot once per conceal->reveal transition; a cover that is merely re-raised while it
        // already holds the current paint is not re-rasterized (and a hidden ancestor cannot be
        // mistaken for a transition, unlike a bare isVisible() test).
        if (!coverCaptured) {
            cover->setSnapshot(coverSnapshot());
            coverCaptured = true;
        }
        if (!cover->isVisible()) {
            if (!coverHost->isVisible()) {
                coverHost->show();
            }
            cover->show();
            cover->raise();
            return true;
        }
        return false;
    }

    void showCover() {
        if (cover == nullptr) {
            return;
        }
        if (!coverCaptured) {
            cover->setSnapshot(coverSnapshot());
            coverCaptured = true;
        }
        if (!cover->isVisible()) {
            if (coverHost != nullptr && !coverHost->isVisible()) {
                coverHost->show();
            }
            cover->show();
            cover->raise();
        }
    }

    void hideCover() {
        if (cover != nullptr) {
            cover->hide();
        }
        if (coverHost != nullptr) {
            coverHost->hide();
        }
        // The next reveal must capture fresh paint, whatever changed while it was concealed.
        coverCaptured = false;
    }

    [[nodiscard]] bool ensurePresenter(const std::uint32_t width, const std::uint32_t height) {
        if (presenter != nullptr) {
            if (presenter->initialized()) {
                return true;
            }
            return presenter->initialize();
        }
        if (!usable() || width == 0U || height == 0U) {
            diagnostic = "no presentation client/port or positive target extent was supplied";
            return false;
        }
        ViewerGpuPresenter::Config config;
        config.vulkan_loader_path = dependencies.vulkanLoaderPath;
        config.target_width = width;
        config.target_height = height;
        config.device_pixel_ratio =
            dependencies.devicePixelRatio > 0.0 ? dependencies.devicePixelRatio : 1.0;
        if (port != nullptr) {
            presenter = std::make_unique<ViewerGpuPresenter>(port, config);
        } else {
            presenter = std::make_unique<ViewerGpuPresenter>(dependencies.client, config);
        }
        // Parent the container BEFORE the window is exposed and the surface
        // attached.
        presenter->setContainerParent(dependencies.containerParent);
        presenter->setInputCallback([this](const ViewerGpuInputEvent& event) {
            if (inputSink) {
                inputSink(event);
            }
        });
        if (!presenter->initialize()) {
            diagnostic = presenter->diagnostic();
            return false;
        }
        return true;
    }

    // Enqueues one present; the returned bool is mailbox admission only, the result arrives via
    // poll().
    [[nodiscard]] bool
    enqueuePresent(std::shared_ptr<const runtime::GpuPresentationOverlay> overlay) {
        if (presenter == nullptr || !presenter->acceptingPresent()) {
            failCpu(presenter != nullptr ? presenter->diagnostic()
                                         : "no presentation target is active");
            if (presentAck) {
                presentAck(false, diagnostic);
            }
            return false;
        }
        ++presentAttempts;
        const bool admitted = presenter->present(pendingLease, pendingParams, std::move(overlay));
        if (!admitted) {
            failCpu(presenter->diagnostic());
            if (presentAck) {
                presentAck(false, diagnostic);
            }
            return false;
        }
        ++acceptedPresents;
        enqueuedSequence = presenter->lastEnqueuedSequence();
        presentAcked = false;
        return true;
    }

    // Enqueues the pending present once the target admits presents (first attach
    // / after resize).
    void pumpPendingPresent() {
        if (!presentPending || presenter == nullptr || !presenter->acceptingPresent()) {
            return;
        }
        presentPending = false;
        static_cast<void>(enqueuePresent(pendingOverlayToPresent));
    }

    void startOverlayRaster(OverlayRasterRequest request, const std::uint64_t forGeneration) {
        if (dependencies.scheduler == nullptr) {
            // No worker: present the frame without an overlay rather than stalling
            // it.
            newestOverlay.reset();
            pendingOverlayToPresent = nullptr;
            presentPending = true;
            pumpPendingPresent();
            return;
        }
        const std::uint64_t generation = ++activeOverlayGeneration;
        activeOverlayRequestGeneration = forGeneration;
        ++overlayRasters;
        const OverlayRasterRequest sharedRequest = std::move(request);
        auto submission =
            dependencies.scheduler->submit<std::shared_ptr<const runtime::GpuPresentationOverlay>>(
                runtime::TaskRequest(
                    "viewer-gpu-overlay-raster",
                    runtime::TaskOwner{.kind = runtime::TaskOwnerKind::PanelRequest,
                                       .id = runtime::TaskOwnerId::fromRaw(
                                           (std::uint64_t{1} << 63U) | 0x5A17ULL)}),
                [sharedRequest](runtime::TaskContext& context)
                    -> runtime::TaskResult<std::shared_ptr<const runtime::GpuPresentationOverlay>> {
                    auto overlay = rasterizeResidentOverlay(
                        sharedRequest, [&context] { return context.isCancellationRequested(); });
                    if (overlay == nullptr) {
                        return runtime::TaskResult<std::shared_ptr<
                            const runtime::GpuPresentationOverlay>>::failed(runtime::TaskDiagnostic{
                            .code = "bloom.ui.overlay-raster",
                            .severity = runtime::DiagnosticSeverity::Error,
                            .summary = "the viewer overlay could not be "
                                       "rasterized",
                            .detail = {},
                            .suggestedAction = {}});
                    }
                    return runtime::TaskResult<std::shared_ptr<
                        const runtime::GpuPresentationOverlay>>::succeeded(std::move(overlay));
                });
        static_cast<void>(generation);
        if (!submission.accepted()) {
            diagnostic = "the overlay raster task was not admitted; presenting "
                         "without overlay";
            pendingOverlayToPresent = nullptr;
            presentPending = true;
            pumpPendingPresent();
            return;
        }
        activeOverlayTask = submission.handle;
    }

    // Starts the newest pending overlay raster if none is active.
    void pumpOverlayStart() {
        if (activeOverlayTask.has_value() || !newestOverlay.has_value()) {
            return;
        }
        OverlayRasterRequest request = std::move(*newestOverlay);
        newestOverlay.reset();
        startOverlayRaster(std::move(request), newestOverlayRequestGeneration);
    }

    // Drains a completed overlay raster and enqueues the follow-up present when
    // it is still current.
    void pumpOverlayCompletion() {
        if (!activeOverlayTask.has_value()) {
            return;
        }
        auto result = activeOverlayTask->tryTakeResult();
        if (!result.has_value()) {
            return;
        }
        activeOverlayTask.reset();
        const bool current = activeOverlayRequestGeneration == requestGeneration;
        if (!current) {
            // Stale raster for a frame/params we have already replaced: drop it,
            // never present it onto the newer frame.
            pumpOverlayStart();
            return;
        }
        if (result->state() == runtime::TaskState::Succeeded && result->value().has_value() &&
            *result->value() != nullptr) {
            presentedOverlay = *result->value();
            presentedOverlaySignature = newestOverlaySignatureForRequest;
            pendingOverlayToPresent = presentedOverlay;
            presentPending = true;
            pumpPendingPresent();
        } else {
            // Keep the image visible (it was already presented without an overlay)
            // and report the overlay failure honestly; never silently drop the
            // required handles.
            diagnostic = "the viewer overlay could not be rasterized; the frame is "
                         "shown without "
                         "its overlays";
        }
        pumpOverlayStart();
    }

    // The signature associated with the current request (set by present()).
    std::uint64_t newestOverlaySignatureForRequest = 0;

    void pumpPresentAck() {
        if (presentAcked || presenter == nullptr || enqueuedSequence == 0) {
            return;
        }
        const auto state = presenter->state();
        if (state == ViewerGpuPresenter::State::Retained ||
            state == ViewerGpuPresenter::State::Unsupported ||
            state == ViewerGpuPresenter::State::Uninitialized) {
            presentAcked = true;
            failCpu(presenter->diagnostic());
            if (presentAck) {
                presentAck(false, diagnostic);
            }
            return;
        }
        if (presentPending) {
            // A newer present is authored but not yet enqueued (the target is still attaching or
            // resizing). The previous present's sequence must not acknowledge the newer request, or
            // the cover would hide over a stale frame.
            return;
        }
        if (!presenter->acceptingPresent()) {
            return; // still attaching/resizing; wait
        }
        if (presentSequenceAcknowledged(presenter->appliedSequence(), enqueuedSequence)) {
            presentAcked = true;
            diagnostic = presenter->diagnostic();
            coverRequired = false;
            hideCover();
            if (presentAck) {
                presentAck(true, diagnostic);
            }
        }
    }
};

ViewerGpuResidentController::ViewerGpuResidentController() : impl_(std::make_unique<Impl>()) {}

ViewerGpuResidentController::~ViewerGpuResidentController() {
    if (impl_ && impl_->activeOverlayTask.has_value()) {
        impl_->activeOverlayTask->cancel();
    }
}

void ViewerGpuResidentController::setDependencies(Dependencies dependencies) {
    impl_->dependencies = std::move(dependencies);
}

void ViewerGpuResidentController::setPresentationPortForTest(std::shared_ptr<ViewerGpuPort> port) {
    impl_->port = std::move(port);
}

void ViewerGpuResidentController::setInputSink(InputSink sink) {
    impl_->inputSink = std::move(sink);
}

void ViewerGpuResidentController::setPresentAck(PresentAck ack) {
    impl_->presentAck = std::move(ack);
}

void ViewerGpuResidentController::setCpuFallback(CpuFallback callback) {
    impl_->cpuFallback = std::move(callback);
}

void ViewerGpuResidentController::setCpuCoverSnapshot(CpuCoverSnapshot snapshot) {
    impl_->coverSnapshot = std::move(snapshot);
}

QWidget* ViewerGpuResidentController::cpuCoverForTest() const noexcept { return impl_->cover; }

void ViewerGpuResidentController::concealCpuCover() {
    impl_->coverRequired = false;
    impl_->hideCover();
}

void ViewerGpuResidentController::revealCpuCover(const QRect& containerRect) {
    // Keep the cover required while the transition is in flight, then raise it. ensureCover()
    // captures the CPU paint once per conceal->reveal transition, so a retained/refused transition
    // that re-raises the cover on every poll tick never re-rasterizes it (and never churns platform
    // buffers); a fresh capture happens only after a conceal, so a blank/no-frame state stays blank
    // rather than flashing the previous native frame.
    impl_->coverRequired = true;
    static_cast<void>(impl_->ensureCover(containerRect));
}

bool ViewerGpuResidentController::cpuCoverVisibleForTest() const noexcept {
    return impl_->cover != nullptr && impl_->cover->isVisible();
}

void ViewerGpuResidentController::setStateChanged(std::function<void()> callback) {
    impl_->stateChanged = std::move(callback);
}

void ViewerGpuResidentController::poll() {
    impl_->pumpOverlayCompletion();
    impl_->pumpOverlayStart();
    impl_->pumpPendingPresent();
    impl_->pumpPresentAck();
    const auto state = presenterState();
    if (state != impl_->lastReportedState) {
        impl_->lastReportedState = state;
        if (impl_->stateChanged) {
            impl_->stateChanged();
        }
    }
}

bool ViewerGpuResidentController::configured() const noexcept { return impl_->usable(); }

bool ViewerGpuResidentController::unsupported() const noexcept {
    return impl_->presenter != nullptr &&
           impl_->presenter->state() == ViewerGpuPresenter::State::Unsupported;
}

bool ViewerGpuResidentController::hasLiveTarget() const noexcept {
    if (impl_->presenter == nullptr) {
        return false;
    }
    switch (impl_->presenter->state()) {
    case ViewerGpuPresenter::State::Uninitialized:
    case ViewerGpuPresenter::State::Unsupported:
    case ViewerGpuPresenter::State::Retired:
        return false;
    case ViewerGpuPresenter::State::Attaching:
    case ViewerGpuPresenter::State::Active:
    case ViewerGpuPresenter::State::Resizing:
    case ViewerGpuPresenter::State::Retiring:
    case ViewerGpuPresenter::State::Retained:
        return true;
    }
    return false;
}

ViewerGpuPresenter::State ViewerGpuResidentController::presenterState() const noexcept {
    return impl_->presenter != nullptr ? impl_->presenter->state()
                                       : ViewerGpuPresenter::State::Uninitialized;
}

const std::string& ViewerGpuResidentController::diagnostic() const noexcept {
    return impl_->diagnostic;
}

QWidget* ViewerGpuResidentController::container() const noexcept {
    return impl_->presenter != nullptr ? impl_->presenter->container() : nullptr;
}

bool ViewerGpuResidentController::requestTargetResize(const std::uint32_t deviceWidth,
                                                      const std::uint32_t deviceHeight) {
    if (impl_->presenter == nullptr || deviceWidth == 0U || deviceHeight == 0U) {
        return false;
    }
    if (!impl_->presenter->requestResize(deviceWidth, deviceHeight)) {
        impl_->diagnostic = impl_->presenter->diagnostic();
        return false;
    }
    impl_->targetWidth = deviceWidth;
    impl_->targetHeight = deviceHeight;
    impl_->presentAcked = false;
    impl_->coverRequired = true;
    return true;
}

bool ViewerGpuResidentController::present(const runtime::PreparedPreviewFrame& frame,
                                          const ResidentPresentRequest& request) {
    impl_->lastResident = false;
    const auto geometry = residentFrameGeometry(frame);
    if (!geometry.has_value()) {
        impl_->diagnostic = "the displayed frame is not a live resident frame";
        impl_->failCpu(impl_->diagnostic);
        return false;
    }
    impl_->lastResident = true;
    if (!impl_->usable()) {
        impl_->failCpu("no presentation client/port is configured");
        return false;
    }
    const std::uint32_t width =
        deviceExtent(request.containerRect.width(), request.devicePixelRatio);
    const std::uint32_t height =
        deviceExtent(request.containerRect.height(), request.devicePixelRatio);
    if (width == 0U || height == 0U) {
        impl_->failCpu("the native container has a zero target extent");
        return false;
    }
    if (!impl_->ensurePresenter(width, height)) {
        impl_->failCpu(impl_->diagnostic);
        return false;
    }
    // Parent to the injected host and give final geometry BEFORE first attach; never reparent a
    // live surface. The native CPU cover is raised above it until a genuine present ack.
    const bool coverRevealed =
        impl_->coverRequired && impl_->ensureCover(request.containerRect.toRect());
    if (QWidget* container = impl_->presenter->container(); container != nullptr) {
        const QRect target(request.containerRect.topLeft().toPoint(),
                           request.containerRect.size().toSize());
        if (container->geometry() != target) {
            container->setGeometry(target);
        }
        const bool containerRevealed = !container->isVisible();
        if (containerRevealed) {
            container->show();
        }
        // Restack the cover only on a genuine reveal transition (the cover was just shown, or the
        // container was just mapped above it). QWidget::raise() is stack-order dependent: when the
        // widget is already topmost it early-returns, but when it has to reorder it marks the whole
        // widget dirty. A stable, already-visible, topmost cover needs no restack, so no restack is
        // issued per present.
        if (impl_->cover != nullptr && (coverRevealed || containerRevealed)) {
            impl_->cover->raise();
        }
    }
    if (impl_->targetWidth == 0U || impl_->targetHeight == 0U) {
        // First present: the presenter was initialized with this exact extent, so
        // there is no old swapchain to retire.
        impl_->targetWidth = width;
        impl_->targetHeight = height;
    } else if (width != impl_->targetWidth || height != impl_->targetHeight) {
        // The swapchain resize gate: retire the old extent and wait for the new one to become
        // Active before presenting. Raise the CPU cover above the resizing surface (its last
        // acknowledged swapchain image is stale for the new extent) and fall through to author the
        // re-present with the new extent; pumpPendingPresent() enqueues it as soon as the target is
        // Active again. Waiting on a presenter state-change edge alone loses the re-present when
        // the owner finishes the resize within one poll interval.
        if (!requestTargetResize(width, height)) {
            impl_->failCpu(impl_->diagnostic);
            return false;
        }
        static_cast<void>(impl_->ensureCover(request.containerRect.toRect()));
    }

    // Destination is the PAR-folded display rect in native-container device pixels; source is the
    // resident image's display window.
    const QRectF local = request.destination.translated(-request.containerRect.topLeft());
    render::GpuPresentImageParams params;
    params.targetWidth = width;
    params.targetHeight = height;
    params.destination = render::GpuPresentRect{
        .x = static_cast<float>(local.x() * request.devicePixelRatio),
        .y = static_cast<float>(local.y() * request.devicePixelRatio),
        .width = static_cast<float>(local.width() * request.devicePixelRatio),
        .height = static_cast<float>(local.height() * request.devicePixelRatio)};
    params.source = render::GpuPresentSourceWindow{
        .x = static_cast<double>(geometry->displayWindow.originX()),
        .y = static_cast<double>(geometry->displayWindow.originY()),
        .width = static_cast<double>(geometry->displayExtent.width()),
        .height = static_cast<double>(geometry->displayExtent.height())};
    params.pixelAspect = geometry->pixelAspect;
    params.channel = request.channel;
    params.background = request.background;
    params.backgroundColor = request.backgroundColor;
    params.checkerColorA = request.checkerColorA;
    params.checkerColorB = request.checkerColorB;
    params.checkerTilePixels = request.checkerTilePixels;
    params.checkerOriginX = static_cast<float>(-request.containerRect.left());
    params.checkerOriginY = static_cast<float>(-request.containerRect.top());
    impl_->pendingLease = geometry->lease;
    impl_->pendingParams = params;
    ++impl_->requestGeneration;
    impl_->newestOverlaySignatureForRequest = request.overlaySignature;

    // The overlay recording is only re-rasterized when its signature changed;
    // otherwise the retained overlay and its token are reused, so a steady view
    // never floods the worker.
    if (request.overlay.valid && request.overlaySignature != 0 &&
        request.overlaySignature == impl_->presentedOverlaySignature &&
        impl_->presentedOverlay != nullptr) {
        impl_->newestOverlay.reset();
        impl_->pendingOverlayToPresent = impl_->presentedOverlay;
        impl_->presentPending = true;
        impl_->pumpPendingPresent();
        return true;
    }

    if (request.overlay.valid && request.overlay.token != 0 &&
        impl_->dependencies.scheduler != nullptr) {
        OverlayRasterRequest raster;
        raster.picture = request.overlay.picture;
        raster.logicalSize = request.overlay.logicalSize;
        raster.width = width;
        raster.height = height;
        raster.token = ++impl_->nextOverlayToken;
        raster.byteBudget = kResidentOverlayByteBudget;
        impl_->newestOverlay = std::move(raster);
        impl_->newestOverlayRequestGeneration = impl_->requestGeneration;
        // Present the frame as soon as the target admits presents, without the
        // stale overlay; the new overlay follows when the bounded raster completes.
        // Never present an old overlay onto a new frame.
        impl_->pendingOverlayToPresent = nullptr;
        impl_->presentPending = true;
        impl_->pumpOverlayStart();
        impl_->pumpPendingPresent();
        return true;
    }

    impl_->newestOverlay.reset();
    impl_->pendingOverlayToPresent = nullptr;
    impl_->presentPending = true;
    impl_->pumpPendingPresent();
    return true;
}

EditorNativeSurface::PrepareOutcome ViewerGpuResidentController::prepareForMutation(
    const std::uint64_t generation, const EditorNativeSurface::PrepareCallback& completion) {
    if (impl_->presenter == nullptr || !hasLiveTarget()) {
        if (completion) {
            completion(generation,
                       EditorNativeSurface::PrepareResult{true, "no live presentation target"});
        }
        return EditorNativeSurface::PrepareOutcome::NoLiveTarget;
    }
    const bool admitted = impl_->presenter->prepareForMutation(
        [generation, completion](const ViewerGpuPresenter::MutationResult& result) {
            if (completion) {
                completion(generation,
                           EditorNativeSurface::PrepareResult{
                               result.outcome == ViewerGpuPresenter::MutationOutcome::SafeToMutate,
                               result.diagnostic});
            }
        });
    if (!admitted) {
        impl_->diagnostic = impl_->presenter->diagnostic();
        return EditorNativeSurface::PrepareOutcome::Refused;
    }
    return EditorNativeSurface::PrepareOutcome::RetirePending;
}

void ViewerGpuResidentController::resumeAfterMutation() {
    if (impl_->presenter == nullptr) {
        return;
    }
    const auto state = impl_->presenter->state();
    if (state == ViewerGpuPresenter::State::Retired ||
        state == ViewerGpuPresenter::State::Unsupported ||
        state == ViewerGpuPresenter::State::Uninitialized) {
        if (impl_->activeOverlayTask.has_value()) {
            impl_->activeOverlayTask->cancel();
            impl_->activeOverlayTask.reset();
        }
        impl_->presenter.reset();
        impl_->pendingLease = runtime::GpuResidentFrameLease{};
        impl_->presentedOverlay.reset();
        impl_->presentedOverlaySignature = 0;
        impl_->enqueuedSequence = 0;
        impl_->presentAcked = false;
        // The next present's extent initializes the next presenter; a stale extent must not gate.
        impl_->targetWidth = 0;
        impl_->targetHeight = 0;
        impl_->presentPending = false;
        impl_->coverRequired = true;
    }
}

std::string ViewerGpuResidentController::mutationDiagnostic() const {
    if (impl_->presenter == nullptr) {
        return "no presentation target";
    }
    return impl_->presenter->diagnostic();
}

void ViewerGpuResidentController::resetRetiredPresenter() { resumeAfterMutation(); }

std::size_t ViewerGpuResidentController::presentAttemptCount() const noexcept {
    return impl_->presentAttempts;
}

std::size_t ViewerGpuResidentController::acceptedPresentCount() const noexcept {
    return impl_->acceptedPresents;
}

bool ViewerGpuResidentController::lastPresentWasResident() const noexcept {
    return impl_->lastResident;
}

bool ViewerGpuResidentController::presentationAcknowledged() const noexcept {
    return impl_->presentAcked;
}

std::uint64_t ViewerGpuResidentController::presentedSequence() const noexcept {
    return impl_->enqueuedSequence;
}

std::uint64_t ViewerGpuResidentController::nativeAppliedSequence() const noexcept {
    return impl_->presenter != nullptr ? impl_->presenter->appliedSequence() : 0U;
}

std::uint64_t ViewerGpuResidentController::nativePresentCount() const noexcept {
    return impl_->presenter != nullptr ? impl_->presenter->presentCount() : 0U;
}

std::uint64_t ViewerGpuResidentController::nativeLastEnqueuedSequence() const noexcept {
    return impl_->presenter != nullptr ? impl_->presenter->lastEnqueuedSequence() : 0U;
}

std::size_t ViewerGpuResidentController::overlayRasterCount() const noexcept {
    return impl_->overlayRasters;
}

} // namespace bloom::ui
