#include <bloom/render/gpu_cryptomatte_coverage.hpp>

#include "gpu_cryptomatte_coverage_private.hpp"

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

constexpr std::uint64_t kCryptomatteCoverageDeadlineNanoseconds =
    5ULL * 1000ULL * 1000ULL * 1000ULL;

[[nodiscard]] GpuCryptomatteCoverageDiagnostic
makeDiagnostic(const GpuCryptomatteCoverageDiagnosticCode code, std::string message) {
    return cryptomatteCoverageDiagnostic(code, std::move(message));
}

} // namespace

void GpuCryptomatteCoverage::Impl::fail(const GpuCryptomatteCoverageDiagnosticCode code,
                                        std::string message) {
    jobState = GpuCryptomatteCoverageJobState::Failure;
    jobDiagnostic = cryptomatteCoverageDiagnostic(code, std::move(message));
}

void GpuCryptomatteCoverage::Impl::clearJob() {
    jobState = GpuCryptomatteCoverageJobState::Idle;
    jobDiagnostic = GpuCryptomatteCoverageDiagnostic{};
    discardRequested.store(false);
}

GpuCryptomatteCoverage::GpuCryptomatteCoverage(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
GpuCryptomatteCoverage::GpuCryptomatteCoverage(GpuCryptomatteCoverage&& other) noexcept = default;
GpuCryptomatteCoverage& GpuCryptomatteCoverage::operator=(GpuCryptomatteCoverage&& other) noexcept {
    if (this != &other) {
        releaseImpl();
        impl_ = std::move(other.impl_);
    }
    return *this;
}
GpuCryptomatteCoverage::~GpuCryptomatteCoverage() { releaseImpl(); }

void GpuCryptomatteCoverage::releaseImpl() noexcept {
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
            const bool retained = cryptomatte_coverage_detail::retainResident(
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
            impl_->submittedAt + std::chrono::nanoseconds(kCryptomatteCoverageDeadlineNanoseconds);
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

GpuCryptomatteCoverageJobState GpuCryptomatteCoverage::state() const noexcept {
    return impl_ != nullptr ? impl_->jobState : GpuCryptomatteCoverageJobState::Idle;
}
const GpuCryptomatteCoverageDiagnostic& GpuCryptomatteCoverage::diagnostic() const noexcept {
    static const GpuCryptomatteCoverageDiagnostic none{};
    if (impl_ == nullptr) {
        return none;
    }
    return impl_->jobDiagnostic;
}
bool GpuCryptomatteCoverage::isBoundTo(GpuDevice& device) const noexcept {
    if (impl_ == nullptr || !impl_->onOwnerThread()) {
        return false;
    }
    const auto deviceState = GpuRendererAccess::state(device);
    return deviceState != nullptr && deviceState == impl_->control;
}

bool GpuCryptomatteCoverage::hasUnretiredSubmission() const noexcept {
    if (impl_ == nullptr) {
        return false;
    }
    if (impl_->queueSubmitted) {
        return true;
    }
    if (impl_->quarantineToken == 0) {
        return false;
    }
    std::lock_guard lock(cryptomatte_coverage_detail::reservationMutex());
    const auto& slot = cryptomatte_coverage_detail::reservation();
    return slot.state == cryptomatte_coverage_detail::ReservationState::Quarantined &&
           slot.token == impl_->quarantineToken && slot.ownerThread == impl_->owner &&
           slot.resources.deviceState == impl_->control;
}

std::uint64_t GpuCryptomatteCoverage::lastJobAllocationBytes() const noexcept {
    return impl_ != nullptr ? impl_->lastJobBytes : 0;
}

GpuCryptomatteCoverageCreateResult
GpuCryptomatteCoverage::create(GpuDevice& device, const GpuCryptomatteCoverageBudgets& budgets) {
    if (budgets.maxImageBytes == 0 || budgets.maxMetadataBytes == 0) {
        return {nullptr, makeDiagnostic(GpuCryptomatteCoverageDiagnosticCode::InvalidArgument,
                                        "the cryptomatte-coverage budget is zero")};
    }
    if (device.state() != GpuDeviceState::Ready) {
        return {nullptr, makeDiagnostic(GpuCryptomatteCoverageDiagnosticCode::DeviceUnavailable,
                                        "the GPU device is not Ready")};
    }
    if (GpuRendererAccess::owner(device) != std::this_thread::get_id()) {
        return {nullptr,
                makeDiagnostic(GpuCryptomatteCoverageDiagnosticCode::WrongThread,
                               "the cryptomatte-coverage instance must be created on the device "
                               "owner thread")};
    }
    auto control = GpuRendererAccess::state(device);
    if (control == nullptr) {
        return {nullptr, makeDiagnostic(GpuCryptomatteCoverageDiagnosticCode::DeviceUnavailable,
                                        "the GPU device exposes no renderer state")};
    }
    // Owner recovery of any residents retained after foreign-thread destruction.
    static_cast<void>(cryptomatte_coverage_detail::retireRetainedForOwner());
    // No native work here: the bounded reservation and every native resource are created in
    // begin(), so many pre-created instances retain nothing.
    auto impl = std::make_unique<Impl>();
    impl->owner = std::this_thread::get_id();
    impl->expectedGeneration = control->generation;
    impl->control = std::move(control);
    impl->budgets = budgets;
    return {std::unique_ptr<GpuCryptomatteCoverage>(new GpuCryptomatteCoverage(std::move(impl))),
            GpuCryptomatteCoverageDiagnostic{}};
}

GpuCryptomatteCoverageDiagnostic
GpuCryptomatteCoverage::begin(const GpuCryptomatteCoverageRequest& parameters,
                              const std::uint64_t byteBudget) {
    if (impl_ == nullptr) {
        return makeDiagnostic(GpuCryptomatteCoverageDiagnosticCode::DeviceUnavailable,
                              "the cryptomatte-coverage instance is not initialized");
    }
    Impl& impl = *impl_;
    const std::thread::id self = std::this_thread::get_id();
    if (!impl.onOwnerThread()) {
        return makeDiagnostic(GpuCryptomatteCoverageDiagnosticCode::WrongThread,
                              "begin must run on the device owner thread");
    }
    if (impl.deviceLost) {
        return makeDiagnostic(GpuCryptomatteCoverageDiagnosticCode::DeviceLost,
                              "the device was lost; this instance must not be reused");
    }
    // Owner recovery: reclaim any resident results retained after foreign-thread destruction before
    // starting new work, so ordinary preview/export is never stalled by the bounded store.
    static_cast<void>(cryptomatte_coverage_detail::retireRetainedForOwner());
    if (impl.queueSubmitted || impl.jobState == GpuCryptomatteCoverageJobState::Pending ||
        impl.jobState == GpuCryptomatteCoverageJobState::Ready) {
        return makeDiagnostic(GpuCryptomatteCoverageDiagnosticCode::Busy,
                              "a job is already in flight or awaiting take()");
    }
    if (impl.control->generation != impl.expectedGeneration) {
        impl.deviceLost = true;
        return makeDiagnostic(GpuCryptomatteCoverageDiagnosticCode::DeviceLost,
                              "the device generation changed; this instance must not be reused");
    }
    if (parameters.source == nullptr || !parameters.source->isValid()) {
        return makeDiagnostic(GpuCryptomatteCoverageDiagnosticCode::InvalidArgument,
                              "the cryptomatte-coverage source image is missing");
    }
    const auto sourceWindow = parameters.source->dataWindow();
    if (!sourceWindow.has_value()) {
        return makeDiagnostic(GpuCryptomatteCoverageDiagnosticCode::InvalidArgument,
                              "the Cryptomatte rank source has no data window");
    }
    const auto outputWindow = parameters.output.dataWindow();
    const std::uint32_t outputWidth = outputWindow.extent().width();
    const std::uint32_t outputHeight = outputWindow.extent().height();
    std::uint64_t stackedHeight = 0;
    if (parameters.rankCount == 0 || outputWidth == 0 || outputHeight == 0 ||
        compositeMultiplyOverflows(outputHeight, parameters.rankCount, stackedHeight) ||
        sourceWindow->extent().width() != outputWidth ||
        sourceWindow->extent().height() != stackedHeight) {
        return makeDiagnostic(GpuCryptomatteCoverageDiagnosticCode::InvalidArgument,
                              "the Cryptomatte rank source does not stack rankCount output frames");
    }
    std::uint64_t metadataBytes = 0;
    std::uint64_t imageBytes = 0;
    std::uint64_t pixels = 0;
    const std::uint64_t idCount = parameters.ids.empty() ? 1U : parameters.ids.size();
    if (compositeMultiplyOverflows(idCount, sizeof(std::uint32_t), metadataBytes) ||
        compositeMultiplyOverflows(outputWidth, outputHeight, pixels) ||
        compositeMultiplyOverflows(pixels, sizeof(Rgba32f), imageBytes) ||
        parameters.ids.size() > std::numeric_limits<std::uint32_t>::max()) {
        return makeDiagnostic(GpuCryptomatteCoverageDiagnosticCode::OverBudget,
                              "the Cryptomatte request overflows the byte arithmetic");
    }
    if (imageBytes > impl.budgets.maxImageBytes) {
        return makeDiagnostic(GpuCryptomatteCoverageDiagnosticCode::OverBudget,
                              "the requested output image exceeds maxImageBytes");
    }
    if (metadataBytes > impl.budgets.maxMetadataBytes) {
        return makeDiagnostic(GpuCryptomatteCoverageDiagnosticCode::OverBudget,
                              "the Cryptomatte selection exceeds maxMetadataBytes");
    }
    std::uint64_t requestedCombined = 0;
    if (compositeAddOverflows(imageBytes, metadataBytes, requestedCombined) ||
        compositeAddOverflows(requestedCombined, metadataBytes, requestedCombined) ||
        requestedCombined > byteBudget) {
        return makeDiagnostic(GpuCryptomatteCoverageDiagnosticCode::OverBudget,
                              "the requested image and selection exceed the byte budget");
    }
    const SolidImageSupport support =
        querySolidImageSupport(*impl.control, outputWidth, outputHeight);
    if (!support.supported) {
        return makeDiagnostic(GpuCryptomatteCoverageDiagnosticCode::Unsupported, support.reason);
    }
    if (imageBytes > support.maxImageBytes) {
        return makeDiagnostic(GpuCryptomatteCoverageDiagnosticCode::OverBudget,
                              "the output image exceeds the device limit");
    }
    if (pixels > std::numeric_limits<std::uint32_t>::max()) {
        return makeDiagnostic(GpuCryptomatteCoverageDiagnosticCode::OverBudget,
                              "the Cryptomatte pixel count exceeds the flattened index range");
    }
    std::uint64_t effectiveMaxX = impl.control->maxComputeWorkGroupCountX;
    const std::uint32_t forcedMaxX =
        cryptomatte_coverage_detail::cryptomatteCoverageForcedMaxWorkGroupCountX().load();
    if (forcedMaxX != 0) {
        effectiveMaxX = std::min<std::uint64_t>(effectiveMaxX, forcedMaxX);
    }
    CryptomatteCoverageDispatch dispatch{};
    if (!planCryptomatteCoverageDispatch(pixels, effectiveMaxX, 65535, dispatch)) {
        return makeDiagnostic(
            GpuCryptomatteCoverageDiagnosticCode::OverBudget,
            "the Cryptomatte dispatch exceeds the device workgroup grid capacity");
    }
    if (!compositeImageBelongsTo(gpuImageImpl(*parameters.source), impl.control)) {
        return makeDiagnostic(GpuCryptomatteCoverageDiagnosticCode::InvalidArgument,
                              "the Cryptomatte rank source does not belong to this device");
    }

    // Claim the bounded reservation BEFORE any native allocation or submit, so a failure never
    // accumulates more than one occupied submission process-wide.
    {
        std::lock_guard lock(cryptomatte_coverage_detail::reservationMutex());
        cryptomatte_coverage_detail::Reservation& slot = cryptomatte_coverage_detail::reservation();
        if (slot.state == cryptomatte_coverage_detail::ReservationState::Reserved) {
            return makeDiagnostic(
                GpuCryptomatteCoverageDiagnosticCode::Busy,
                "the cryptomatte-coverage reservation is held by a live submission");
        }
        if (slot.state == cryptomatte_coverage_detail::ReservationState::Quarantined) {
            if (slot.ownerThread != self) {
                return makeDiagnostic(
                    GpuCryptomatteCoverageDiagnosticCode::DeviceUnavailable,
                    "a foreign owner thread holds the cryptomatte-coverage quarantine");
            }
            if (!cryptomatte_coverage_detail::retireQuarantinedLocked(slot)) {
                return makeDiagnostic(GpuCryptomatteCoverageDiagnosticCode::DeviceUnavailable,
                                      "a prior cryptomatte-coverage submission is not retired");
            }
        }
        slot.state = cryptomatte_coverage_detail::ReservationState::Reserved;
        slot.ownerThread = self;
        ++slot.token;
        if (slot.token == 0) {
            ++slot.token;
        }
        impl.slotToken = slot.token;
    }

    impl.quarantineToken = 0;
    impl.clearJob();
    if (!impl.createPipelines()) {
        const auto diagnostic = impl.createDiagnostic;
        impl.freeJobResources();
        impl.releaseClaim();
        return diagnostic;
    }
    if (!impl.launchJob(parameters.source, outputWindow, parameters.output.displayWindow(),
                        parameters.output.pixelAspect(), parameters.rankCount, parameters.ids,
                        parameters.preview, byteBudget)) {
        const auto diagnostic = impl.jobDiagnostic;
        impl.freeJobResources();
        impl.releaseClaim();
        impl.jobState = GpuCryptomatteCoverageJobState::Failure;
        return diagnostic;
    }
    impl.jobState = GpuCryptomatteCoverageJobState::Pending;
    return GpuCryptomatteCoverageDiagnostic{};
}

GpuCryptomatteCoveragePollResult GpuCryptomatteCoverage::poll() {
    if (impl_ == nullptr) {
        return GpuCryptomatteCoveragePollResult::Failure;
    }
    Impl& impl = *impl_;
    if (!impl.onOwnerThread()) {
        return GpuCryptomatteCoveragePollResult::WrongThread;
    }
    if (impl.quarantineToken != 0) {
        std::lock_guard lock(cryptomatte_coverage_detail::reservationMutex());
        auto& slot = cryptomatte_coverage_detail::reservation();
        if (slot.state == cryptomatte_coverage_detail::ReservationState::Quarantined &&
            slot.token == impl.quarantineToken && slot.ownerThread == impl.owner &&
            slot.resources.deviceState == impl.control) {
            if (!cryptomatte_coverage_detail::retireQuarantinedLocked(slot)) {
                return GpuCryptomatteCoveragePollResult::Failure;
            }
        }
        impl.quarantineToken = 0;
        return GpuCryptomatteCoveragePollResult::Failure;
    }
    if (impl.jobState == GpuCryptomatteCoverageJobState::Ready) {
        return GpuCryptomatteCoveragePollResult::Ready;
    }
    if (!impl.queueSubmitted) {
        return GpuCryptomatteCoveragePollResult::Failure;
    }
    const auto fault = static_cast<cryptomatte_coverage_detail::CryptomatteCoverageFault>(
        cryptomatte_coverage_detail::cryptomatteCoverageFault().load());
    const VkFence rawFence = static_cast<VkFence>(*impl.resources.fence);
    const VkResult status =
        fault == cryptomatte_coverage_detail::CryptomatteCoverageFault::ForceFenceTimeout
            ? VK_NOT_READY
            : impl.control->device.getDispatcher()->vkGetFenceStatus(
                  static_cast<VkDevice>(*impl.control->device), rawFence);
    // An injected diagnostic cannot justify freeing real pending driver resources. First prove
    // retirement using the genuine fence, then reject publication as device loss.
    if (fault == cryptomatte_coverage_detail::CryptomatteCoverageFault::ForceDeviceLost &&
        status == VK_SUCCESS) {
        impl.deviceLost = true;
        impl.queueSubmitted = false;
        impl.freeJobResources();
        impl.releaseClaim();
        impl.fail(GpuCryptomatteCoverageDiagnosticCode::DeviceLost,
                  "the device was lost while polling");
        return GpuCryptomatteCoveragePollResult::Failure;
    }
    if (status == VK_NOT_READY) {
        const bool deadlineExceeded =
            fault == cryptomatte_coverage_detail::CryptomatteCoverageFault::ForceFenceTimeout ||
            std::chrono::steady_clock::now() >=
                impl.submittedAt +
                    std::chrono::nanoseconds(kCryptomatteCoverageDeadlineNanoseconds);
        if (!deadlineExceeded) {
            return GpuCryptomatteCoveragePollResult::Pending;
        }
        const bool cancelled = impl.discardRequested.load();
        if (!impl.quarantine()) {
            [[maybe_unused]] const auto* const retained = impl_.release();
            return GpuCryptomatteCoveragePollResult::Failure;
        }
        impl.fail(cancelled ? GpuCryptomatteCoverageDiagnosticCode::Cancelled
                            : GpuCryptomatteCoverageDiagnosticCode::NativeTimeout,
                  cancelled
                      ? "the cryptomatte-coverage job was cancelled; the submission is retained"
                      : "the cryptomatte-coverage job did not retire within the deadline; the "
                        "submission is retained");
        return GpuCryptomatteCoveragePollResult::Failure;
    }
    if (status == VK_ERROR_DEVICE_LOST) {
        impl.deviceLost = true;
        impl.queueSubmitted = false;
        impl.freeJobResources();
        impl.releaseClaim();
        impl.fail(GpuCryptomatteCoverageDiagnosticCode::DeviceLost,
                  "the device was lost during the cryptomatte-coverage wait");
        return GpuCryptomatteCoveragePollResult::Failure;
    }
    if (status != VK_SUCCESS) {
        if (!impl.quarantine()) {
            [[maybe_unused]] const auto* const retained = impl_.release();
            return GpuCryptomatteCoveragePollResult::Failure;
        }
        impl.fail(GpuCryptomatteCoverageDiagnosticCode::DeviceUnavailable,
                  "the cryptomatte-coverage completion is unknown; the submission was retained");
        return GpuCryptomatteCoveragePollResult::Failure;
    }
    impl.queueSubmitted = false;
    if (impl.discardRequested.load()) {
        impl.releaseRetired();
        impl.fail(GpuCryptomatteCoverageDiagnosticCode::Cancelled,
                  "the cryptomatte-coverage job was cancelled; no image was published");
        return GpuCryptomatteCoveragePollResult::Failure;
    }
    if (impl.resources.resident == nullptr) {
        impl.releaseRetired();
        impl.fail(GpuCryptomatteCoverageDiagnosticCode::DeviceUnavailable,
                  "the resident image is missing");
        return GpuCryptomatteCoveragePollResult::Failure;
    }
    impl.releaseRetired();
    impl.jobState = GpuCryptomatteCoverageJobState::Ready;
    impl.jobDiagnostic = GpuCryptomatteCoverageDiagnostic{};
    return GpuCryptomatteCoveragePollResult::Ready;
}

const GpuImage* GpuCryptomatteCoverage::image() const noexcept {
    if (impl_ == nullptr || impl_->jobState != GpuCryptomatteCoverageJobState::Ready) {
        return nullptr;
    }
    return impl_->resources.resident.get();
}

GpuImage GpuCryptomatteCoverage::take() noexcept {
    if (impl_ == nullptr || impl_->resources.resident == nullptr ||
        impl_->jobState != GpuCryptomatteCoverageJobState::Ready) {
        return GpuImage{};
    }
    GpuImage taken = std::move(*impl_->resources.resident);
    impl_->resources.resident.reset();
    if (impl_->resources.residentSlotHeld) {
        impl_->resources.residentSlotHeld = false;
        cryptomatte_coverage_detail::releaseResidentSlot();
    }
    impl_->clearJob();
    return taken;
}

void GpuCryptomatteCoverage::cancel() noexcept {
    if (impl_ != nullptr &&
        (impl_->queueSubmitted || impl_->jobState == GpuCryptomatteCoverageJobState::Pending)) {
        impl_->discardRequested.store(true);
    }
}

bool GpuCryptomatteCoverage::teardownDrainIncomplete() noexcept {
    return cryptomatteCoverageTeardownIncomplete();
}

} // namespace bloom::render
