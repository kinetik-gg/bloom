#include "gpu_path_coverage_private.hpp"

#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

// GpuPathCoverage lifecycle, split out of gpu_path_coverage.cpp to keep each translation unit under
// the file budget: the test-only fault seam, the bounded reservation bookkeeping, the bounded
// owner-retirement store for foreign-thread destruction, the object constructor/destructor, and
// poll/readback/cancel/observability. begin() and create() stay in gpu_path_coverage.cpp.

namespace bloom::render {
namespace {
constexpr std::uint64_t kDrainTimeoutNanoseconds = 2ULL * 1000ULL * 1000ULL * 1000ULL;
constexpr std::uint64_t kCoverageDeadlineNanoseconds = 2ULL * 1000ULL * 1000ULL * 1000ULL;

// Bounded resident pool. Every Impl native resource set (compute pipeline plus resident mask plus
// any in-flight job resources) occupies exactly one fixed slot, acquired before the Impl's first
// native allocation and held until owner-thread retirement. A foreign-thread destruction never
// destroys native state: it marks the already-owned slot orphaned and releases the wrapper. The
// owner drain retires orphaned residents (allocation-free, noexcept) and returns their slots, so
// admission recovers after recoverable pressure. There is no fuse and no per-object leak.
constexpr std::size_t kResidentCapacity = 8;

struct ResidentSlot final {
    GpuPathCoverageImpl* impl = nullptr;
    bool orphaned = false;
};

std::mutex& residentMutex() {
    static auto* const value = new std::mutex();
    return *value;
}
std::array<ResidentSlot, kResidentCapacity>& residentSlots() {
    static auto* const value = new std::array<ResidentSlot, kResidentCapacity>{};
    return *value;
}
std::atomic<std::uint64_t>& residentRefusals() {
    static std::atomic<std::uint64_t> value{0};
    return value;
}
std::atomic<std::uint64_t>& residentRetired() {
    static std::atomic<std::uint64_t> value{0};
    return value;
}

// Foreign-thread destruction: preserve the original ownership in the slot, never destroy native
// state off-thread.
void orphanResidentSlot(GpuPathCoverageImpl* const impl) noexcept {
    if (impl == nullptr || impl->residentSlot >= kResidentCapacity) {
        return;
    }
    std::lock_guard lock(residentMutex());
    ResidentSlot& slot = residentSlots()[impl->residentSlot];
    if (slot.impl == impl) {
        slot.orphaned = true;
    }
}

// Exact ownership gate: an Impl may only be touched by the device owner thread that created it,
// and only while it still belongs to the device generation it was created from. Every Vulkan access
// (fence query, resource teardown) and the final delete must be gated on this.
[[nodiscard]] bool implOwnedByCurrentThread(const GpuPathCoverageImpl* const impl) noexcept {
    return impl != nullptr && impl->owner != std::thread::id{} &&
           impl->owner == std::this_thread::get_id() && impl->control != nullptr &&
           impl->control->generation == impl->expectedGeneration;
}

// Non-blocking. Returns true when the Impl has no live submission left to prove. Caller must have
// checked implOwnedByCurrentThread() first.
[[nodiscard]] bool retireOrphanedImplIfProven(GpuPathCoverageImpl* const impl) noexcept {
    if (impl == nullptr) {
        return true;
    }
    if (impl->submitted) {
        if (impl->jobFence == vk::raii::Fence{nullptr}) {
            return false;
        }
        const VkResult status = impl->control->device.getDispatcher()->vkGetFenceStatus(
            static_cast<VkDevice>(*impl->control->device),
            static_cast<VkFence>(*impl->jobFence));
        if (status != VK_SUCCESS && status != VK_ERROR_DEVICE_LOST) {
            return false;
        }
        impl->deviceLost = impl->deviceLost || status == VK_ERROR_DEVICE_LOST;
        impl->releaseRetired(false);
    }
    return true;
}

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

std::size_t pathCoverageResidentCapacity() noexcept { return kResidentCapacity; }

// Acquires a resident slot before any native allocation. Returns false when the bounded pool is
// full; the caller refuses the begin cleanly without allocating.
bool acquireResidentSlot(GpuPathCoverageImpl* const impl) noexcept {
    std::lock_guard lock(residentMutex());
    for (std::size_t index = 0; index < kResidentCapacity; ++index) {
        if (residentSlots()[index].impl == nullptr) {
            residentSlots()[index] = {impl, false};
            impl->residentSlot = index;
            return true;
        }
    }
    residentRefusals().fetch_add(1);
    return false;
}

// Owner-thread retirement: returns the slot. Never touches another owner's resources.
void releaseResidentSlot(GpuPathCoverageImpl* const impl) noexcept {
    if (impl == nullptr || impl->residentSlot >= kResidentCapacity) {
        return;
    }
    std::lock_guard lock(residentMutex());
    ResidentSlot& slot = residentSlots()[impl->residentSlot];
    if (slot.impl == impl) {
        slot = {};
    }
    impl->residentSlot = kPathCoverageNoResidentSlot;
}

std::size_t pathCoverageResidentInUse() noexcept {
    std::lock_guard lock(residentMutex());
    std::size_t count = 0;
    for (const auto& slot : residentSlots()) {
        if (slot.impl != nullptr) {
            ++count;
        }
    }
    return count;
}

std::size_t pathCoverageResidentOrphaned() noexcept {
    std::lock_guard lock(residentMutex());
    std::size_t count = 0;
    for (const auto& slot : residentSlots()) {
        if (slot.orphaned) {
            ++count;
        }
    }
    return count;
}

std::uint64_t pathCoverageResidentRefusals() noexcept { return residentRefusals().load(); }
std::uint64_t pathCoverageResidentRetired() noexcept { return residentRetired().load(); }

void drainPathCoverageResidentOrphansOnOwnerThread() noexcept {
    std::array<GpuPathCoverageImpl*, kResidentCapacity> freed{};
    std::size_t freedCount = 0;
    {
        std::lock_guard lock(residentMutex());
        for (auto& slot : residentSlots()) {
            if (slot.impl == nullptr || !slot.orphaned) {
                continue;
            }
            // Never touch another device owner's resources: skip orphans whose exact owner thread
            // (and generation) is not this thread. The rightful owner drains them.
            if (!implOwnedByCurrentThread(slot.impl)) {
                continue;
            }
            if (!retireOrphanedImplIfProven(slot.impl)) {
                continue;
            }
            GpuPathCoverageImpl* const impl = slot.impl;
            slot = {};
            // Clear the back-pointer before the Impl is destroyed so a stale destructor can never
            // address a slot that may already have been re-admitted.
            impl->residentSlot = kPathCoverageNoResidentSlot;
            freed[freedCount++] = impl;
        }
    }
    for (std::size_t index = 0; index < freedCount; ++index) {
        delete freed[index];
        residentRetired().fetch_add(1);
    }
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
    // A resident slot must have been returned by owner retirement before destruction; an Impl with
    // no slot owns no Vulkan objects (the pipeline is created lazily under a slot), so it is safe to
    // destroy on any thread.
    assert(residentSlot == kPathCoverageNoResidentSlot);
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
    if (!impl_->onOwnerThread()) {
        // Foreign thread: never destroy native state. An Impl that owns a resident slot is preserved
        // in that same slot (orphaned) for owner retirement; an Impl with no slot owns no Vulkan
        // objects and can be destroyed here.
        if (impl_->residentSlot != kPathCoverageNoResidentSlot) {
            orphanResidentSlot(impl_.get());
            (void)impl_.release();
        } else {
            impl_.reset();
        }
        return;
    }
    // Owner thread: prove retirement if needed, then free native resources and return the slot.
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
            // Could not prove retirement: keep the Impl and its slot (bounded by the pool).
            [[maybe_unused]] const auto* const retained = impl_.release();
            return;
        }
    }
    path_coverage_detail::releaseResidentSlot(impl_.get());
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
