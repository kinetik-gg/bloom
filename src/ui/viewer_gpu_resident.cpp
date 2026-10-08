// ViewerGpuResidentController implementation; Qt/Vulkan interop stays in ViewerGpuPresenter.

#include <bloom/ui/viewer_gpu_resident.hpp>

#include <bloom/ui/viewer_gpu_resident_overlay.hpp>

#include "viewer_gpu_presenter_port.hpp"

#include <bloom/render/gpu_present_image.hpp>

#include "viewer_gpu_resident_cover.hpp"

#include <QWidget>
#include <QWindow>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
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

#include "viewer_gpu_resident_state.ipp"

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

void ViewerGpuResidentController::setFrameTrace(std::shared_ptr<PreviewFrameTrace> trace) {
    impl_->trace = std::move(trace);
    impl_->tracePresenterId = impl_->trace ? impl_->trace->allocatePresenterId() : 0;
    impl_->traceSubmissions = {};
    impl_->lastTracedAppliedSequence = 0;
}

void ViewerGpuResidentController::setCpuFallback(CpuFallback callback) {
    impl_->cpuFallback = std::move(callback);
}

void ViewerGpuResidentController::setCpuCoverSnapshot(CpuCoverSnapshot snapshot) {
    impl_->coverSnapshot = std::move(snapshot);
}

QWidget* ViewerGpuResidentController::cpuCoverForTest() const noexcept { return impl_->cover; }
QWindow* ViewerGpuResidentController::cpuCoverWindowForTest() const noexcept {
    return impl_->cover ? impl_->cover->nativeWindow() : nullptr;
}
QWindow* ViewerGpuResidentController::cpuCoverPersistentWindowForTest() const noexcept {
    return impl_->cover ? impl_->cover->persistentWindow() : nullptr;
}
QPixmap ViewerGpuResidentController::cpuCoverSnapshotForTest() const {
    return impl_->cover ? impl_->cover->snapshot() : QPixmap{};
}
std::size_t ViewerGpuResidentController::cpuCoverPaintCountForTest() const noexcept {
    return impl_->cover ? impl_->cover->paintCount() : 0;
}

void ViewerGpuResidentController::concealCpuCover() {
    impl_->coverRequired = false;
    impl_->hideCover();
}

void ViewerGpuResidentController::revealCpuCover(const QRect& containerRect,
                                                 const bool refreshSnapshot) {
    // Keep the cover required while the transition is in flight, then raise it. ensureCover()
    // captures the CPU paint once per conceal->reveal transition, so a retained/refused transition
    // that re-raises the cover on every poll tick never re-rasterizes it (and never churns platform
    // buffers); a fresh capture happens only after a conceal, so a blank/no-frame state stays blank
    // rather than flashing the previous native frame.
    impl_->coverRequired = true;
    if (refreshSnapshot)
        impl_->coverCaptured = false;
    static_cast<void>(impl_->ensureCover(containerRect));
}

bool ViewerGpuResidentController::cpuCoverVisible() const noexcept {
    return impl_->cover != nullptr && impl_->cover->isVisible() && impl_->cover->nativeWindow() &&
           impl_->cover->nativeWindow()->isVisible() && impl_->cover->nativeWindow()->isExposed();
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
    ++impl_->surfaceEpoch;
    impl_->enqueuedSequence = 0; // resize discards old updates without acknowledging them
    impl_->targetWidth = deviceWidth;
    impl_->targetHeight = deviceHeight;
    impl_->presentAcked = false;
    impl_->coverRequired = true;
    impl_->showCover();
    return true;
}

bool ViewerGpuResidentController::present(const runtime::PreparedPreviewFrame& frame,
                                          const ResidentPresentRequest& request) {
    impl_->lastResident = false;
    const std::uint32_t width =
        deviceExtent(request.containerRect.width(), request.devicePixelRatio);
    const std::uint32_t height =
        deviceExtent(request.containerRect.height(), request.devicePixelRatio);
    if (!request.overlay.valid &&
        (!impl_->requestedSafetyDomain || impl_->requestedSafetyDomain->overlayRequired))
        ++impl_->removalEpoch;
    impl_->requestedSafetyDomain =
        Impl::RequestedSafetyDomain{frame.desiredIdentity().projectId,
                                    frame.desiredIdentity().compositionId,
                                    request.overlayDomainRevision,
                                    impl_->surfaceEpoch,
                                    impl_->removalEpoch,
                                    width,
                                    height,
                                    request.containerRect,
                                    request.devicePixelRatio,
                                    request.overlay.valid};
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
    if (width == 0U || height == 0U) {
        impl_->failCpu("the native container has a zero target extent");
        return false;
    }
    if (!impl_->ensurePresenter(width, height)) {
        impl_->failCpu(impl_->diagnostic);
        return false;
    }
    // Parent to the injected host and give final geometry BEFORE first attach; never reparent a
    // live surface. Establish the persistent raster child and temporary initial cover before ack.
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
        // Maintain the alien wrapper order only on genuine reveal. Its persistent native child
        // covers the GPU parent independently of QWidget/container stacking. QWidget::raise can
        // dirty the parent when its widget order changes, so never issue it per present.
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
        if (!impl_->requestedSafetyDomain) {
            impl_->failCpu("the native request domain was cleared during cover capture");
            return false;
        }
        impl_->requestedSafetyDomain->surfaceEpoch = impl_->surfaceEpoch;
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
    Impl::Publication publication;
    publication.frame = std::make_shared<const runtime::PreparedPreviewFrame>(frame);
    publication.projectId = frame.desiredIdentity().projectId;
    publication.compositionId = frame.desiredIdentity().compositionId;
    publication.params = params;
    publication.source = request.overlay;
    publication.geometry = geometry;
    publication.containerRect = request.containerRect;
    publication.devicePixelRatio = request.devicePixelRatio;
    publication.destination = request.destination;
    publication.domainRevision = request.overlayDomainRevision;
    publication.surfaceEpoch = impl_->surfaceEpoch;
    publication.removalEpoch = impl_->removalEpoch;
    publication.generation = ++impl_->requestGeneration;
    if (impl_->pendingPublication && !Impl::compatible(*impl_->pendingPublication, publication))
        impl_->presentPending = false;
    impl_->desiredPublication = publication;
    impl_->presentAcked = false;
    // A changed required overlay is published atomically with the captured image/params; the
    // previous complete pair remains visible while the existing bounded worker runs.
    if (request.overlay.valid) {
        if (impl_->rasterPublication && impl_->presentedOverlay &&
            Impl::equivalent(*impl_->rasterPublication, publication)) {
            impl_->newestPublication.reset();
            impl_->stagePublication(std::move(publication), impl_->presentedOverlay);
        } else {
            impl_->newestPublication = std::move(publication);
            impl_->pumpOverlayStart();
        }
        return true;
    }
    // Explicit removal is a complete publication, never an invitation to reuse old handles.
    impl_->newestPublication.reset();
    impl_->rasterPublication.reset();
    impl_->presentedOverlay.reset();
    impl_->stagePublication(std::move(publication), nullptr);
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
        ++impl_->surfaceEpoch;
        impl_->activePublication.reset();
        impl_->newestPublication.reset();
        impl_->desiredPublication.reset();
        impl_->rasterPublication.reset();
        impl_->pendingPublication.reset();
        impl_->appliedPublicationFrame.reset();
        impl_->appliedDisplay.reset();
        impl_->requestedSafetyDomain.reset();
        impl_->frameSubmissions = {};
        impl_->lastObservedAppliedSequence = 0;
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
    return impl_->presentAcked && publicationCurrent();
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

std::shared_ptr<const runtime::PreparedPreviewFrame>
ViewerGpuResidentController::appliedFrame() const noexcept {
    return impl_->appliedPublicationFrame;
}

const std::optional<ResidentAppliedDisplay>&
ViewerGpuResidentController::appliedDisplay() const noexcept {
    return impl_->appliedDisplay;
}

bool ViewerGpuResidentController::canRetainAppliedDisplay() const noexcept {
    return impl_->canRetainAppliedDisplay();
}

bool ViewerGpuResidentController::nativeDisplayAvailable() const noexcept {
    return impl_->nativeDisplayAvailable();
}

void ViewerGpuResidentController::clearPublicationDomain() {
    ++impl_->removalEpoch;
    impl_->desiredPublication.reset();
    impl_->requestedSafetyDomain.reset();
    impl_->newestPublication.reset();
    impl_->pendingPublication.reset();
    impl_->presentPending = false;
    impl_->presentAcked = false;
    impl_->coverRequired = true;
    impl_->coverCaptured = false;
    if (impl_->activeOverlayTask)
        impl_->activeOverlayTask->cancel();
}

QRectF ViewerGpuResidentController::appliedDestination() const noexcept {
    return impl_->appliedPublicationDestination;
}

bool ViewerGpuResidentController::publicationCurrent() const noexcept {
    return impl_->canRetainAppliedDisplay() && impl_->desiredPublication &&
           impl_->appliedMatchesRequestedSafetyDomain() && impl_->appliedPublicationFrame &&
           impl_->appliedPublicationGeneration == impl_->requestGeneration;
}

std::size_t ViewerGpuResidentController::overlayRasterCount() const noexcept {
    return impl_->overlayRasters;
}

} // namespace bloom::ui
