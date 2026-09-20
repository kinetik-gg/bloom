#include "gpu_path_coverage_private.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>

// GpuPathCoverage lifecycle, split out of gpu_path_coverage.cpp to keep each translation unit under
// the file budget: the test-only fault seam, the bounded reservation bookkeeping, the object
// constructor/destructor, and poll/readback/cancel/observability. begin() and create() stay in
// gpu_path_coverage.cpp.

namespace bloom::render {
namespace {
constexpr std::uint64_t kDrainTimeoutNanoseconds = 2ULL * 1000ULL * 1000ULL * 1000ULL;
constexpr std::uint64_t kCoverageDeadlineNanoseconds = 2ULL * 1000ULL * 1000ULL * 1000ULL;
} // namespace

// --- Test-only fault seam (production-owned atomics; setters are private and never called by
// production code) ------------------------------------------------------------------------------
namespace path_coverage_detail {

std::atomic<std::uint8_t>& pathCoverageFault() noexcept {
    static std::atomic<std::uint8_t> fault{0};
    return fault;
}
std::atomic<std::uint32_t>& pathCoverageMaxWorkGroupCountXOverride() noexcept {
    static std::atomic<std::uint32_t> value{0};
    return value;
}
std::atomic<std::uint32_t>& pathCoverageMaxWorkGroupCountYOverride() noexcept {
    static std::atomic<std::uint32_t> value{0};
    return value;
}
bool pathCoverageQuarantineOccupied() noexcept {
    std::lock_guard lock(reservationMutex());
    return reservation().state == ReservationState::Quarantined;
}
bool retirePathCoverageQuarantineForOwner() noexcept {
    std::lock_guard lock(reservationMutex());
    Reservation& slot = reservation();
    if (slot.state != ReservationState::Quarantined ||
        slot.ownerThread != std::this_thread::get_id()) {
        return false;
    }
    return retireQuarantinedLocked(slot);
}

} // namespace path_coverage_detail

GpuPathCoverageMaskOwner::~GpuPathCoverageMaskOwner() { release(); }

void GpuPathCoverageMaskOwner::release() noexcept {
    if (state != nullptr && buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(state->allocator, buffer, allocation);
    }
    state.reset();
    buffer = VK_NULL_HANDLE;
    allocation = VK_NULL_HANDLE;
    bytes = 0;
}

void GpuPathCoverageImpl::releaseRetired(const bool keepMask) noexcept {
    ranges.release();
    spans.release();
    if (!keepMask) {
        mask.reset();
    }
    jobBuffer = vk::raii::CommandBuffer{nullptr};
    jobPool = vk::raii::CommandPool{nullptr};
    jobFence = vk::raii::Fence{nullptr};
    if (slotToken != 0) {
        std::lock_guard lock(path_coverage_detail::reservationMutex());
        path_coverage_detail::Reservation& slot = path_coverage_detail::reservation();
        if (path_coverage_detail::isReservationOwnerLocked(slot) && slot.token == slotToken) {
            slot.state = path_coverage_detail::ReservationState::Free;
            slot.ownerThread = std::thread::id{};
        }
        slotToken = 0;
    }
    submitted = false;
}

void GpuPathCoverageImpl::releaseClaim() noexcept {
    if (slotToken == 0) {
        return;
    }
    std::lock_guard lock(path_coverage_detail::reservationMutex());
    path_coverage_detail::Reservation& slot = path_coverage_detail::reservation();
    if (path_coverage_detail::isReservationOwnerLocked(slot) && slot.token == slotToken) {
        slot.state = path_coverage_detail::ReservationState::Free;
        slot.ownerThread = std::thread::id{};
    }
    slotToken = 0;
}

bool GpuPathCoverageImpl::quarantine() noexcept {
    if (!submitted) {
        releaseRetired(true);
        return true;
    }
    path_coverage_detail::quarantineFuse().store(true);
    std::lock_guard lock(path_coverage_detail::reservationMutex());
    path_coverage_detail::Reservation& slot = path_coverage_detail::reservation();
    if (!path_coverage_detail::isReservationOwnerLocked(slot) || slot.token != slotToken) {
        return false;
    }
    slot.deviceState = control;
    slot.ranges = std::move(ranges);
    slot.spans = std::move(spans);
    slot.mask = std::move(mask);
    slot.pool = std::move(jobPool);
    slot.buffer = std::move(jobBuffer);
    slot.fence = std::move(jobFence);
    slot.state = path_coverage_detail::ReservationState::Quarantined;
    ranges = {};
    spans = {};
    mask.reset();
    jobBuffer = vk::raii::CommandBuffer{nullptr};
    jobPool = vk::raii::CommandPool{nullptr};
    jobFence = vk::raii::Fence{nullptr};
    slotToken = 0;
    submitted = false;
    return true;
}

GpuPathCoverageImpl::~GpuPathCoverageImpl() {
    assert(owner == std::this_thread::get_id());
    ranges.release();
    spans.release();
    mask.reset();
}

GpuPathCoverage::GpuPathCoverage(std::unique_ptr<GpuPathCoverageImpl> impl) noexcept
    : impl_(std::move(impl)) {}
GpuPathCoverage::GpuPathCoverage(GpuPathCoverage&& other) noexcept = default;
GpuPathCoverage& GpuPathCoverage::operator=(GpuPathCoverage&& other) noexcept {
    if (this != &other) {
        releaseImpl();
        impl_ = std::move(other.impl_);
    }
    return *this;
}
GpuPathCoverage::~GpuPathCoverage() { releaseImpl(); }

void GpuPathCoverage::releaseImpl() noexcept {
    if (impl_ == nullptr) {
        return;
    }
    // Native pipeline and resident resources are owner-thread-only. A destruction on a foreign
    // thread retains the Impl (mirroring GpuSolid) rather than tearing down a live device resource
    // from the wrong thread; an in-flight submission is additionally bounded by the reservation.
    if (!impl_->onOwnerThread()) {
        [[maybe_unused]] const auto* const retained = impl_.release();
        return;
    }
    if (impl_->submitted) {
        cancel();
        const auto deadline =
            impl_->submittedAt + std::chrono::nanoseconds(kDrainTimeoutNanoseconds);
        while (impl_->submitted && std::chrono::steady_clock::now() < deadline) {
            static_cast<void>(poll());
            if (impl_->submitted) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        if (impl_->submitted && !impl_->quarantine()) {
            [[maybe_unused]] const auto* const retained = impl_.release();
            return;
        }
    }
    impl_.reset();
}


GpuPathCoveragePollResult GpuPathCoverage::poll() {
    if (impl_ == nullptr) {
        return GpuPathCoveragePollResult::Failure;
    }
    GpuPathCoverageImpl& impl = *impl_;
    if (!impl.onOwnerThread()) {
        return GpuPathCoveragePollResult::WrongThread;
    }
    if (impl.jobState == GpuPathCoverageJobState::Ready) {
        return GpuPathCoveragePollResult::Ready;
    }
    const bool pending = impl.jobState == GpuPathCoverageJobState::Pending;
    if (!pending && !impl.submitted) {
        return GpuPathCoveragePollResult::Failure;
    }
    if (!impl.submitted) {
        return GpuPathCoveragePollResult::Failure;
    }
    const auto fault =
        static_cast<path_coverage_detail::PathCoverageFault>(
            path_coverage_detail::pathCoverageFault().load());
    VkResult status = VK_NOT_READY;
    if (fault == path_coverage_detail::PathCoverageFault::ForceDeviceLost) {
        status = VK_ERROR_DEVICE_LOST;
    } else if (fault != path_coverage_detail::PathCoverageFault::ForceFenceTimeout) {
        const VkFence rawFence = static_cast<VkFence>(*impl.jobFence);
        status = impl.control->device.getDispatcher()->vkGetFenceStatus(
            static_cast<VkDevice>(*impl.control->device), rawFence);
    }
    if (status == VK_NOT_READY) {
        const bool deadlineExceeded =
            fault == path_coverage_detail::PathCoverageFault::ForceFenceTimeout ||
            std::chrono::steady_clock::now() >=
                impl.submittedAt + std::chrono::nanoseconds(kCoverageDeadlineNanoseconds);
        if (!deadlineExceeded) {
            return pending ? GpuPathCoveragePollResult::Pending : GpuPathCoveragePollResult::Failure;
        }
        const bool cancelled = impl.discardRequested.load();
        if (!impl.quarantine()) {
            return GpuPathCoveragePollResult::Failure;
        }
        impl.fail(cancelled ? GpuPathCoverageDiagnosticCode::Cancelled
                            : GpuPathCoverageDiagnosticCode::NativeTimeout,
                  cancelled ? "the coverage job was cancelled; the submission is retained until "
                              "retired"
                            : "the coverage did not retire within the deadline; the submission "
                              "is retained");
        return GpuPathCoveragePollResult::Failure;
    }
    if (status == VK_ERROR_DEVICE_LOST) {
        impl.deviceLost = true;
        impl.releaseRetired(false);
        impl.fail(GpuPathCoverageDiagnosticCode::DeviceLost, "the device was lost while polling");
        return GpuPathCoveragePollResult::Failure;
    }
    if (status != VK_SUCCESS) {
        if (!impl.quarantine()) {
            return GpuPathCoveragePollResult::Failure;
        }
        impl.fail(GpuPathCoverageDiagnosticCode::DeviceUnavailable,
                  "the coverage completion is unknown; the submission was retained");
        return GpuPathCoveragePollResult::Failure;
    }
    // The dispatch is proved complete: free the in-flight geometry and command resources, keep the
    // resident mask, and return the reservation to Free.
    impl.releaseRetired(true);
    if (impl.discardRequested.load()) {
        impl.clearJob();
        impl.fail(GpuPathCoverageDiagnosticCode::Cancelled, "the coverage job was cancelled");
        return GpuPathCoveragePollResult::Failure;
    }
    if (impl.mask == nullptr) {
        impl.fail(GpuPathCoverageDiagnosticCode::DeviceUnavailable,
                  "the resident coverage mask is missing");
        return GpuPathCoveragePollResult::Failure;
    }
    impl.jobState = GpuPathCoverageJobState::Ready;
    impl.jobDiagnostic = {};
    return GpuPathCoveragePollResult::Ready;
}

GpuPathCoverageReadback GpuPathCoverage::readback(const std::uint64_t byteBudget) noexcept {
    GpuPathCoverageReadback result;
    if (impl_ == nullptr || impl_->jobState != GpuPathCoverageJobState::Ready ||
        impl_->mask == nullptr) {
        result.code = GpuPathCoverageReadbackCode::DeviceUnavailable;
        result.message = "no resident coverage mask to read back";
        return result;
    }
    GpuPathCoverageImpl& impl = *impl_;
    if (!impl.onOwnerThread()) {
        result.code = GpuPathCoverageReadbackCode::WrongThread;
        result.message = "readback must run on the device owner thread";
        return result;
    }
    const std::uint64_t pixels =
        static_cast<std::uint64_t>(impl.mask->width) * impl.mask->height;
    if (pixels > byteBudget) {
        result.code = GpuPathCoverageReadbackCode::OverBudget;
        result.message = "the coverage readback exceeds the requested byte budget";
        return result;
    }
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(impl.control->allocator, impl.mask->allocation, &info);
    if (info.pMappedData == nullptr) {
        result.code = GpuPathCoverageReadbackCode::ReadbackFailed;
        result.message = "the resident coverage mask is not host-mapped";
        return result;
    }
    if (vmaInvalidateAllocation(impl.control->allocator, impl.mask->allocation, 0, VK_WHOLE_SIZE) !=
        VK_SUCCESS) {
        result.code = GpuPathCoverageReadbackCode::ReadbackFailed;
        result.message = "the resident coverage mask could not be invalidated";
        return result;
    }
    const auto* packed = static_cast<const std::uint8_t*>(info.pMappedData);
    result.coverage.resize(static_cast<std::size_t>(pixels));
    for (std::uint64_t index = 0; index < pixels; ++index) {
        const auto base = static_cast<std::size_t>(index >> 2) * 4ULL;
        const std::uint32_t word = static_cast<std::uint32_t>(packed[base]) |
                                   (static_cast<std::uint32_t>(packed[base + 1]) << 8U) |
                                   (static_cast<std::uint32_t>(packed[base + 2]) << 16U) |
                                   (static_cast<std::uint32_t>(packed[base + 3]) << 24U);
        result.coverage[static_cast<std::size_t>(index)] =
            static_cast<std::uint8_t>((word >> ((index & 3ULL) * 8ULL)) & 0xFFU);
    }
    result.code = GpuPathCoverageReadbackCode::None;
    return result;
}

void GpuPathCoverage::cancel() noexcept {
    if (impl_ != nullptr) {
        impl_->discardRequested.store(true);
    }
}

bool GpuPathCoverage::teardownDrainIncomplete() noexcept {
    return path_coverage_detail::quarantineFuse().load();
}

std::shared_ptr<GpuPathCoverageMaskOwner>
gpuPathCoverageMask(const GpuPathCoverage& coverage) noexcept {
    if (coverage.impl_ == nullptr) {
        return nullptr;
    }
    return coverage.impl_->mask;
}

} // namespace bloom::render
