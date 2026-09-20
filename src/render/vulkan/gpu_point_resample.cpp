#include <bloom/render/gpu_point_resample.hpp>

#include "gpu_point_resample_private.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace bloom::render {
namespace {

constexpr std::uint64_t kPointResampleDeadlineNanoseconds = 5ULL * 1000ULL * 1000ULL * 1000ULL;

[[nodiscard]] GpuPointResampleDiagnostic makeDiagnostic(const GpuPointResampleDiagnosticCode code,
                                                        std::string message) {
    return pointResampleDiagnostic(code, std::move(message));
}

// The media-image-proxy CPU oracle extent: max(1, ceil(sourceExtent * scale)). Rejects a scale that
// is non-finite or not strictly positive and an unbounded/overflowing result before any allocation.
[[nodiscard]] bool derivedProxyExtent(const std::uint32_t sourceWidth,
                                      const std::uint32_t sourceHeight,
                                      const double horizontalScale, const double verticalScale,
                                      std::uint32_t& outputWidth,
                                      std::uint32_t& outputHeight) noexcept {
    if (!std::isfinite(horizontalScale) || !std::isfinite(verticalScale) ||
        horizontalScale <= 0.0 || verticalScale <= 0.0) {
        return false;
    }
    const double width =
        std::max(1.0, std::ceil(static_cast<double>(sourceWidth) * horizontalScale));
    const double height =
        std::max(1.0, std::ceil(static_cast<double>(sourceHeight) * verticalScale));
    constexpr auto kMaximum = static_cast<double>(std::numeric_limits<std::uint32_t>::max());
    if (!std::isfinite(width) || !std::isfinite(height) || width > kMaximum || height > kMaximum) {
        return false;
    }
    outputWidth = static_cast<std::uint32_t>(width);
    outputHeight = static_cast<std::uint32_t>(height);
    return true;
}

// Overflow-checked axis metadata size (one int32 per output column plus one per output row).
[[nodiscard]] bool axisMetadataBytes(const std::uint32_t outputWidth,
                                     const std::uint32_t outputHeight,
                                     std::uint64_t& out) noexcept {
    std::uint64_t count = 0;
    if (compositeAddOverflows(outputWidth, outputHeight, count) ||
        compositeMultiplyOverflows(count, sizeof(std::int32_t), out)) {
        return false;
    }
    return true;
}

} // namespace

void GpuPointResample::Impl::fail(const GpuPointResampleDiagnosticCode code, std::string message) {
    jobState = GpuPointResampleJobState::Failure;
    jobDiagnostic = pointResampleDiagnostic(code, std::move(message));
}

void GpuPointResample::Impl::clearJob() {
    jobState = GpuPointResampleJobState::Idle;
    jobDiagnostic = GpuPointResampleDiagnostic{};
    discardRequested.store(false);
}

GpuPointResample::GpuPointResample(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
GpuPointResample::GpuPointResample(GpuPointResample&& other) noexcept = default;
GpuPointResample& GpuPointResample::operator=(GpuPointResample&& other) noexcept {
    if (this != &other) {
        releaseImpl();
        impl_ = std::move(other.impl_);
    }
    return *this;
}
GpuPointResample::~GpuPointResample() { releaseImpl(); }

void GpuPointResample::releaseImpl() noexcept {
    if (impl_ == nullptr) {
        return;
    }
    // Only the owner thread may touch native resources.
    if (!impl_->onOwnerThread()) {
        if (impl_->queueSubmitted || impl_->resourcesLive) {
            // A live submission or partial resource set: retain the whole Impl, bounded by the one
            // occupied in-flight reservation.
            [[maybe_unused]] const auto* const retained = impl_.release();
            return;
        }
        if (impl_->resources.resident != nullptr) {
            // A completed resident result can be moved into the allocation-free bounded
            // owner-drainable retained store and reclaimed later on the owner thread.
            // retainResident moves the unique_ptr ONLY after securing a free slot; on failure the
            // Impl keeps ownership, so the image is never destroyed on this foreign thread.
            const bool retained = point_resample_detail::retainResident(
                impl_->resources.deviceState, impl_->resources.resident, impl_->owner);
            if (!retained) {
                [[maybe_unused]] const auto* const leaked = impl_.release();
                return;
            }
            impl_->resources.residentSlotHeld = false;
            impl_->resources.resident.reset();
            impl_.reset();
            return;
        }
        // A truly idle instance holds no native resources: safe to destroy from any thread.
        impl_.reset();
        return;
    }
    if (impl_->queueSubmitted) {
        impl_->discardRequested.store(true);
        const auto deadline =
            impl_->submittedAt + std::chrono::nanoseconds(kPointResampleDeadlineNanoseconds);
        while (impl_ != nullptr && impl_->queueSubmitted &&
               std::chrono::steady_clock::now() < deadline) {
            static_cast<void>(poll());
            if (impl_ != nullptr && impl_->queueSubmitted) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        if (impl_ != nullptr && impl_->queueSubmitted) {
            // Could not prove retirement: quarantine the exact resource set; never an unbounded
            // leak.
            if (!impl_->quarantine()) {
                [[maybe_unused]] const auto* const retained = impl_.release();
                return;
            }
        }
    }
    if (impl_ != nullptr) {
        impl_->releaseClaim();
    }
    impl_.reset();
}

GpuPointResampleJobState GpuPointResample::state() const noexcept {
    return impl_ != nullptr ? impl_->jobState : GpuPointResampleJobState::Idle;
}
const GpuPointResampleDiagnostic& GpuPointResample::diagnostic() const noexcept {
    static const GpuPointResampleDiagnostic none{};
    if (impl_ == nullptr) {
        return none;
    }
    return impl_->jobDiagnostic;
}
bool GpuPointResample::isBoundTo(GpuDevice& device) const noexcept {
    if (impl_ == nullptr || !impl_->onOwnerThread()) {
        return false;
    }
    const auto deviceState = GpuRendererAccess::state(device);
    return deviceState != nullptr && deviceState == impl_->control;
}

bool GpuPointResample::hasUnretiredSubmission() const noexcept {
    return impl_ != nullptr && impl_->queueSubmitted;
}

std::uint64_t GpuPointResample::lastJobAllocationBytes() const noexcept {
    return impl_ != nullptr ? impl_->lastJobBytes : 0;
}

GpuPointResampleCreateResult GpuPointResample::create(GpuDevice& device,
                                                      const GpuPointResampleBudgets& budgets) {
    if (budgets.maxImageBytes == 0 || budgets.maxMetadataBytes == 0) {
        return {nullptr, makeDiagnostic(GpuPointResampleDiagnosticCode::InvalidArgument,
                                        "the point-resample budget is zero")};
    }
    if (device.state() != GpuDeviceState::Ready) {
        return {nullptr, makeDiagnostic(GpuPointResampleDiagnosticCode::DeviceUnavailable,
                                        "the GPU device is not Ready")};
    }
    if (GpuRendererAccess::owner(device) != std::this_thread::get_id()) {
        return {nullptr, makeDiagnostic(GpuPointResampleDiagnosticCode::WrongThread,
                                        "the point-resample instance must be created on the device "
                                        "owner thread")};
    }
    auto control = GpuRendererAccess::state(device);
    if (control == nullptr) {
        return {nullptr, makeDiagnostic(GpuPointResampleDiagnosticCode::DeviceUnavailable,
                                        "the GPU device exposes no renderer state")};
    }
    // Owner recovery of any residents retained after foreign-thread destruction.
    static_cast<void>(point_resample_detail::retireRetainedForOwner());
    // No native work here: the bounded reservation and every native resource are created in
    // begin(), so many pre-created instances retain nothing.
    auto impl = std::make_unique<Impl>();
    impl->owner = std::this_thread::get_id();
    impl->expectedGeneration = control->generation;
    impl->control = std::move(control);
    impl->budgets = budgets;
    return {std::unique_ptr<GpuPointResample>(new GpuPointResample(std::move(impl))),
            GpuPointResampleDiagnostic{}};
}

GpuPointResampleDiagnostic GpuPointResample::begin(const GpuPointResampleRequest& parameters,
                                                   const std::uint64_t byteBudget) {
    if (impl_ == nullptr) {
        return makeDiagnostic(GpuPointResampleDiagnosticCode::DeviceUnavailable,
                              "the point-resample instance is not initialized");
    }
    Impl& impl = *impl_;
    const std::thread::id self = std::this_thread::get_id();
    if (!impl.onOwnerThread()) {
        return makeDiagnostic(GpuPointResampleDiagnosticCode::WrongThread,
                              "begin must run on the device owner thread");
    }
    if (impl.deviceLost) {
        return makeDiagnostic(GpuPointResampleDiagnosticCode::DeviceLost,
                              "the device was lost; this instance must not be reused");
    }
    // Owner recovery: reclaim any resident results retained after foreign-thread destruction before
    // starting new work, so ordinary preview/export is never stalled by the bounded store.
    static_cast<void>(point_resample_detail::retireRetainedForOwner());
    if (impl.queueSubmitted || impl.jobState == GpuPointResampleJobState::Pending ||
        impl.jobState == GpuPointResampleJobState::Ready) {
        return makeDiagnostic(GpuPointResampleDiagnosticCode::Busy,
                              "a job is already in flight or awaiting take()");
    }
    if (impl.control->generation != impl.expectedGeneration) {
        impl.deviceLost = true;
        return makeDiagnostic(GpuPointResampleDiagnosticCode::DeviceLost,
                              "the device generation changed; this instance must not be reused");
    }
    if (parameters.source == nullptr || !parameters.source->isValid()) {
        return makeDiagnostic(GpuPointResampleDiagnosticCode::InvalidArgument,
                              "the point-resample source image is missing");
    }
    const auto sourceWindow = parameters.source->dataWindow();
    if (!sourceWindow.has_value()) {
        return makeDiagnostic(GpuPointResampleDiagnosticCode::InvalidArgument,
                              "the point-resample source image has no data window");
    }
    const std::uint32_t sourceWidth = sourceWindow->extent().width();
    const std::uint32_t sourceHeight = sourceWindow->extent().height();
    std::uint32_t outputWidth = 0;
    std::uint32_t outputHeight = 0;
    if (!derivedProxyExtent(sourceWidth, sourceHeight, parameters.horizontalScale,
                            parameters.verticalScale, outputWidth, outputHeight)) {
        return makeDiagnostic(GpuPointResampleDiagnosticCode::InvalidArgument,
                              "the point-resample scale or derived extent is invalid");
    }
    const auto outputWindow = parameters.output.dataWindow();
    if (outputWindow.originX() != 0 || outputWindow.originY() != 0 ||
        outputWindow.extent().width() != outputWidth ||
        outputWindow.extent().height() != outputHeight) {
        return makeDiagnostic(GpuPointResampleDiagnosticCode::InvalidArgument,
                              "the output descriptor does not match the derived proxy window");
    }
    if (outputWidth == 0 || outputHeight == 0) {
        return makeDiagnostic(GpuPointResampleDiagnosticCode::InvalidArgument,
                              "an extent is empty");
    }
    std::uint64_t metadataBytes = 0;
    std::uint64_t imageBytes = 0;
    std::uint64_t pixels = 0;
    if (!axisMetadataBytes(outputWidth, outputHeight, metadataBytes) ||
        compositeMultiplyOverflows(outputWidth, outputHeight, pixels) ||
        compositeMultiplyOverflows(pixels, sizeof(Rgba32f), imageBytes)) {
        return makeDiagnostic(GpuPointResampleDiagnosticCode::OverBudget,
                              "the point-resample request overflows the byte arithmetic");
    }
    if (imageBytes > impl.budgets.maxImageBytes) {
        return makeDiagnostic(GpuPointResampleDiagnosticCode::OverBudget,
                              "the requested output image exceeds maxImageBytes");
    }
    if (metadataBytes > impl.budgets.maxMetadataBytes) {
        return makeDiagnostic(GpuPointResampleDiagnosticCode::OverBudget,
                              "the requested axis metadata exceeds maxMetadataBytes");
    }
    std::uint64_t requestedCombined = 0;
    if (compositeAddOverflows(imageBytes, metadataBytes, requestedCombined) ||
        requestedCombined > byteBudget) {
        return makeDiagnostic(GpuPointResampleDiagnosticCode::OverBudget,
                              "the requested image and axis metadata exceed the byte budget");
    }
    const SolidImageSupport support =
        querySolidImageSupport(*impl.control, outputWidth, outputHeight);
    if (!support.supported) {
        return makeDiagnostic(GpuPointResampleDiagnosticCode::Unsupported, support.reason);
    }
    if (imageBytes > support.maxImageBytes) {
        return makeDiagnostic(GpuPointResampleDiagnosticCode::OverBudget,
                              "the output image exceeds the device limit");
    }
    if (pixels > std::numeric_limits<std::uint32_t>::max()) {
        return makeDiagnostic(GpuPointResampleDiagnosticCode::OverBudget,
                              "the point-resample pixel count exceeds the flattened index range");
    }
    std::uint64_t effectiveMaxX = impl.control->maxComputeWorkGroupCountX;
    const std::uint32_t forcedMaxX =
        point_resample_detail::pointResampleForcedMaxWorkGroupCountX().load();
    if (forcedMaxX != 0) {
        effectiveMaxX = std::min<std::uint64_t>(effectiveMaxX, forcedMaxX);
    }
    PointResampleDispatch dispatch{};
    if (!planPointResampleDispatch(pixels, effectiveMaxX, 65535, dispatch)) {
        return makeDiagnostic(GpuPointResampleDiagnosticCode::OverBudget,
                              "the point-resample dispatch exceeds the device workgroup grid "
                              "capacity");
    }
    if (!compositeImageBelongsTo(gpuImageImpl(*parameters.source), impl.control)) {
        return makeDiagnostic(GpuPointResampleDiagnosticCode::InvalidArgument,
                              "the point-resample source image does not belong to this device");
    }

    const auto maps =
        preparePointResampleAxisMaps(sourceWidth, sourceHeight, outputWidth, outputHeight,
                                     parameters.horizontalScale, parameters.verticalScale);
    std::vector<std::int32_t> axisIndices;
    axisIndices.reserve(static_cast<std::size_t>(outputWidth) + outputHeight);
    axisIndices.insert(axisIndices.end(), maps.sourceX.begin(), maps.sourceX.end());
    axisIndices.insert(axisIndices.end(), maps.sourceY.begin(), maps.sourceY.end());

    // Claim the bounded reservation BEFORE any native allocation or submit, so a failure never
    // accumulates more than one occupied submission process-wide.
    {
        std::lock_guard lock(point_resample_detail::reservationMutex());
        point_resample_detail::Reservation& slot = point_resample_detail::reservation();
        if (slot.state == point_resample_detail::ReservationState::Reserved) {
            return makeDiagnostic(GpuPointResampleDiagnosticCode::Busy,
                                  "the point-resample reservation is held by a live submission");
        }
        if (slot.state == point_resample_detail::ReservationState::Quarantined) {
            if (slot.ownerThread != self) {
                return makeDiagnostic(GpuPointResampleDiagnosticCode::DeviceUnavailable,
                                      "a foreign owner thread holds the point-resample quarantine");
            }
            if (!point_resample_detail::retireQuarantinedLocked(slot)) {
                return makeDiagnostic(GpuPointResampleDiagnosticCode::DeviceUnavailable,
                                      "a prior point-resample submission is not retired");
            }
        }
        slot.state = point_resample_detail::ReservationState::Reserved;
        slot.ownerThread = self;
        ++slot.token;
        if (slot.token == 0) {
            ++slot.token;
        }
        impl.slotToken = slot.token;
    }

    impl.clearJob();
    if (!impl.createPipelines()) {
        const auto diagnostic = impl.createDiagnostic;
        impl.freeJobResources();
        impl.releaseClaim();
        return diagnostic;
    }
    if (!impl.launchJob(parameters.source, outputWidth, outputHeight,
                        parameters.output.displayWindow(), parameters.output.pixelAspect(),
                        axisIndices, byteBudget)) {
        const auto diagnostic = impl.jobDiagnostic;
        impl.freeJobResources();
        impl.releaseClaim();
        impl.jobState = GpuPointResampleJobState::Failure;
        return diagnostic;
    }
    impl.jobState = GpuPointResampleJobState::Pending;
    return GpuPointResampleDiagnostic{};
}

GpuPointResamplePollResult GpuPointResample::poll() {
    if (impl_ == nullptr) {
        return GpuPointResamplePollResult::Failure;
    }
    Impl& impl = *impl_;
    if (!impl.onOwnerThread()) {
        return GpuPointResamplePollResult::WrongThread;
    }
    if (impl.jobState == GpuPointResampleJobState::Ready) {
        return GpuPointResamplePollResult::Ready;
    }
    if (!impl.queueSubmitted) {
        return GpuPointResamplePollResult::Failure;
    }
    const auto fault = static_cast<point_resample_detail::PointResampleFault>(
        point_resample_detail::pointResampleFault().load());
    if (fault == point_resample_detail::PointResampleFault::ForceDeviceLost) {
        impl.deviceLost = true;
        impl.queueSubmitted = false;
        impl.freeJobResources();
        impl.releaseClaim();
        impl.fail(GpuPointResampleDiagnosticCode::DeviceLost, "the device was lost while polling");
        return GpuPointResamplePollResult::Failure;
    }
    const VkFence rawFence = static_cast<VkFence>(*impl.resources.fence);
    const VkResult status = fault == point_resample_detail::PointResampleFault::ForceFenceTimeout
                                ? VK_NOT_READY
                                : impl.control->device.getDispatcher()->vkGetFenceStatus(
                                      static_cast<VkDevice>(*impl.control->device), rawFence);
    if (status == VK_NOT_READY) {
        const bool deadlineExceeded =
            fault == point_resample_detail::PointResampleFault::ForceFenceTimeout ||
            std::chrono::steady_clock::now() >=
                impl.submittedAt + std::chrono::nanoseconds(kPointResampleDeadlineNanoseconds);
        if (!deadlineExceeded) {
            return GpuPointResamplePollResult::Pending;
        }
        const bool cancelled = impl.discardRequested.load();
        if (!impl.quarantine()) {
            [[maybe_unused]] const auto* const retained = impl_.release();
            return GpuPointResamplePollResult::Failure;
        }
        impl.fail(cancelled ? GpuPointResampleDiagnosticCode::Cancelled
                            : GpuPointResampleDiagnosticCode::NativeTimeout,
                  cancelled ? "the point-resample job was cancelled; the submission is retained"
                            : "the point-resample job did not retire within the deadline; the "
                              "submission is retained");
        return GpuPointResamplePollResult::Failure;
    }
    if (status == VK_ERROR_DEVICE_LOST) {
        impl.deviceLost = true;
        impl.queueSubmitted = false;
        impl.freeJobResources();
        impl.releaseClaim();
        impl.fail(GpuPointResampleDiagnosticCode::DeviceLost,
                  "the device was lost during the point-resample wait");
        return GpuPointResamplePollResult::Failure;
    }
    if (status != VK_SUCCESS) {
        if (!impl.quarantine()) {
            [[maybe_unused]] const auto* const retained = impl_.release();
            return GpuPointResamplePollResult::Failure;
        }
        impl.fail(GpuPointResampleDiagnosticCode::DeviceUnavailable,
                  "the point-resample completion is unknown; the submission was retained");
        return GpuPointResamplePollResult::Failure;
    }
    impl.queueSubmitted = false;
    if (impl.discardRequested.load()) {
        impl.releaseRetired();
        impl.fail(GpuPointResampleDiagnosticCode::Cancelled,
                  "the point-resample job was cancelled; no image was published");
        return GpuPointResamplePollResult::Failure;
    }
    if (impl.resources.resident == nullptr) {
        impl.releaseRetired();
        impl.fail(GpuPointResampleDiagnosticCode::DeviceUnavailable,
                  "the resident image is missing");
        return GpuPointResamplePollResult::Failure;
    }
    impl.releaseRetired();
    impl.jobState = GpuPointResampleJobState::Ready;
    impl.jobDiagnostic = GpuPointResampleDiagnostic{};
    return GpuPointResamplePollResult::Ready;
}

const GpuImage* GpuPointResample::image() const noexcept {
    if (impl_ == nullptr || impl_->jobState != GpuPointResampleJobState::Ready) {
        return nullptr;
    }
    return impl_->resources.resident.get();
}

GpuImage GpuPointResample::take() noexcept {
    if (impl_ == nullptr || impl_->resources.resident == nullptr ||
        impl_->jobState != GpuPointResampleJobState::Ready) {
        return GpuImage{};
    }
    GpuImage taken = std::move(*impl_->resources.resident);
    impl_->resources.resident.reset();
    if (impl_->resources.residentSlotHeld) {
        impl_->resources.residentSlotHeld = false;
        point_resample_detail::releaseResidentSlot();
    }
    impl_->clearJob();
    return taken;
}

void GpuPointResample::cancel() noexcept {
    if (impl_ != nullptr &&
        (impl_->queueSubmitted || impl_->jobState == GpuPointResampleJobState::Pending)) {
        impl_->discardRequested.store(true);
    }
}

bool GpuPointResample::teardownDrainIncomplete() noexcept {
    return pointResampleTeardownIncomplete();
}

} // namespace bloom::render
