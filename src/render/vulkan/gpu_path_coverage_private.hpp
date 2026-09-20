#ifndef BLOOM_RENDER_VULKAN_GPU_PATH_COVERAGE_PRIVATE_HPP
#define BLOOM_RENDER_VULKAN_GPU_PATH_COVERAGE_PRIVATE_HPP

// Private to src/render/vulkan. Holds the Vulkan-backed GpuPathCoverage impl, the shared owner of
// its device-resident packed R8 coverage buffer, and the bounded process-wide reservation that
// serializes in-flight submissions. The public header exposes no native type; GpuSolid reaches the
// resident buffer only through the narrow gpuPathCoverageMask() accessor so it can bind it directly
// with no host roundtrip.

#include <bloom/render/gpu_path_coverage.hpp>

#include "gpu_device_private.hpp"
#include "gpu_path_coverage_fault.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace bloom::render {

// Shared by the pipeline-creation and dispatch translation units.
struct GpuPathCoveragePushConstants final {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t groupsX; // workgroups per row for the flattened 2D dispatch grid
};
static_assert(sizeof(GpuPathCoveragePushConstants) == 12);

// Shared ownership of one VMA host-visible storage buffer. Both the producing GpuPathCoverage and
// any consuming GpuSolid job retain this, so the buffer cannot be freed while a submission still
// references it.
struct GpuPathCoverageMaskOwner final {
    GpuPathCoverageMaskOwner() = default;
    GpuPathCoverageMaskOwner(const GpuPathCoverageMaskOwner&) = delete;
    GpuPathCoverageMaskOwner& operator=(const GpuPathCoverageMaskOwner&) = delete;
    ~GpuPathCoverageMaskOwner();

    void release() noexcept;

    std::shared_ptr<vulkan_detail::DeviceAllocatorState> state;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    std::uint64_t bytes = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t generation = 0;
};

// Move-only host-visible storage buffer for geometry upload or the resident mask.
struct GpuPathCoverageHostBuffer final {
    GpuPathCoverageHostBuffer() = default;
    GpuPathCoverageHostBuffer(const GpuPathCoverageHostBuffer&) = delete;
    GpuPathCoverageHostBuffer& operator=(const GpuPathCoverageHostBuffer&) = delete;
    GpuPathCoverageHostBuffer(GpuPathCoverageHostBuffer&& other) noexcept { *this = std::move(other); }
    GpuPathCoverageHostBuffer& operator=(GpuPathCoverageHostBuffer&& other) noexcept {
        if (this != &other) {
            release();
            state = other.state;
            buffer = other.buffer;
            allocation = other.allocation;
            bytes = other.bytes;
            armed = other.armed;
            other.state = nullptr;
            other.buffer = VK_NULL_HANDLE;
            other.allocation = VK_NULL_HANDLE;
            other.bytes = 0;
            other.armed = false;
        }
        return *this;
    }
    ~GpuPathCoverageHostBuffer() { release(); }

    void release() noexcept {
        if (armed && state != nullptr) {
            vmaDestroyBuffer(state->allocator, buffer, allocation);
        }
        state = nullptr;
        buffer = VK_NULL_HANDLE;
        allocation = VK_NULL_HANDLE;
        bytes = 0;
        armed = false;
    }

    vulkan_detail::DeviceAllocatorState* state = nullptr;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    std::uint64_t bytes = 0;
    bool armed = false;
};

// The bounded reservation behind the producer. Exactly one submission may be Reserved or
// Quarantined process-wide. A reservation is claimed BEFORE any native allocation or submit; a
// proven retirement returns it to Free; an unproven retirement moves the exact submission
// (geometry, mask, command pool/buffer, fence, device generation) into the retained quarantine, so
// only the genuine owner thread can later prove retirement or observe device loss. Admission is
// refused while the slot is occupied, so at most one in-flight submission exists and a foreign-thread
// destruction can retain at most that one Impl rather than leaking per object.
namespace path_coverage_detail {

enum class ReservationState : std::uint8_t { Free, Reserved, Quarantined };

struct Reservation final {
    ReservationState state = ReservationState::Free;
    std::thread::id ownerThread{};
    std::uint64_t token = 0;

    std::shared_ptr<vulkan_detail::DeviceAllocatorState> deviceState;
    GpuPathCoverageHostBuffer ranges;
    GpuPathCoverageHostBuffer spans;
    std::shared_ptr<GpuPathCoverageMaskOwner> mask;
    vk::raii::CommandPool pool{nullptr};
    vk::raii::CommandBuffer buffer{nullptr};
    vk::raii::Fence fence{nullptr};
};

inline Reservation& reservation() {
    static auto* const value = new Reservation();
    return *value;
}

inline std::mutex& reservationMutex() {
    static auto* const value = new std::mutex();
    return *value;
}

// Latched when a submission could not be proven retired. Never cleared; the reservation serializes
// admission so a stale fuse only denies new work until the owner proves retirement.
inline std::atomic<bool>& quarantineFuse() {
    static std::atomic<bool> fuse{false};
    return fuse;
}

inline void freeQuarantinedLocked(Reservation& slot) noexcept {
    slot.ranges.release();
    slot.spans.release();
    slot.mask.reset();
    slot.buffer = vk::raii::CommandBuffer{nullptr};
    slot.pool = vk::raii::CommandPool{nullptr};
    slot.fence = vk::raii::Fence{nullptr};
    slot.deviceState.reset();
    slot.state = ReservationState::Free;
    slot.ownerThread = std::thread::id{};
}

inline bool isReservationOwnerLocked(const Reservation& slot) noexcept {
    return slot.state != ReservationState::Free &&
           slot.ownerThread == std::this_thread::get_id();
}

// Owner-only, non-blocking.
inline bool retireQuarantinedLocked(Reservation& slot) noexcept {
    if (slot.state != ReservationState::Quarantined || slot.deviceState == nullptr ||
        slot.fence == vk::raii::Fence{nullptr}) {
        return false;
    }
    if (static_cast<PathCoverageFault>(pathCoverageFault().load()) ==
        PathCoverageFault::ForceFenceTimeout) {
        return false;
    }
    const VkFence rawFence = static_cast<VkFence>(*slot.fence);
    const VkResult status = slot.deviceState->device.getDispatcher()->vkGetFenceStatus(
        static_cast<VkDevice>(*slot.deviceState->device), rawFence);
    if (status != VK_SUCCESS && status != VK_ERROR_DEVICE_LOST) {
        return false;
    }
    freeQuarantinedLocked(slot);
    return true;
}

} // namespace path_coverage_detail

struct GpuPathCoverageImpl final {
    GpuPathCoverageImpl() = default;
    GpuPathCoverageImpl(const GpuPathCoverageImpl&) = delete;
    GpuPathCoverageImpl& operator=(const GpuPathCoverageImpl&) = delete;
    ~GpuPathCoverageImpl();

    [[nodiscard]] bool onOwnerThread() const noexcept {
        return control != nullptr && control->owner == std::this_thread::get_id();
    }
    void fail(const GpuPathCoverageDiagnosticCode code, std::string message) {
        jobState = GpuPathCoverageJobState::Failure;
        jobDiagnostic = GpuPathCoverageDiagnostic{code, std::move(message)};
    }
    void clearJob() {
        jobState = GpuPathCoverageJobState::Idle;
        jobDiagnostic = GpuPathCoverageDiagnostic{};
        discardRequested.store(false);
        mask.reset();
        ranges.release();
        spans.release();
        jobBuffer = vk::raii::CommandBuffer{nullptr};
        jobPool = vk::raii::CommandPool{nullptr};
        jobFence = vk::raii::Fence{nullptr};
        submitted = false;
        slotToken = 0;
    }
    // Frees the in-flight native resources and returns a proven-retired claim to Free.
    void releaseRetired(bool keepMask) noexcept;
    // Returns a Reserved claim taken before submit. Never frees another owner's quarantine.
    void releaseClaim() noexcept;
    // Transfers a possibly in-flight submission into the process-wide quarantine. On success the
    // Impl holds no job resources; the reservation retains the exact pins.
    [[nodiscard]] bool quarantine() noexcept;

    [[nodiscard]] bool createPipeline();

    std::thread::id owner;
    std::shared_ptr<vulkan_detail::DeviceAllocatorState> control;
    GpuPathCoverageBudgets budgets;
    std::uint32_t expectedGeneration = 0;
    // Private device fact read once at create(): the compute workgroup count Y limit, so a large
    // capacity-valid dispatch can be flattened into 2D instead of being refused by a 1D grid.
    std::uint32_t maxWorkGroupCountY = 0;

    vk::raii::ShaderModule shaderModule{nullptr};
    vk::raii::DescriptorSetLayout descriptorSetLayout{nullptr};
    vk::raii::PipelineLayout pipelineLayout{nullptr};
    vk::raii::Pipeline pipeline{nullptr};
    vk::raii::DescriptorPool descriptorPool{nullptr};
    vk::raii::DescriptorSet descriptorSet{nullptr};

    // Per-job native resources, retained until the producing fence is proved retired (or moved into
    // the quarantine on an unproven retirement).
    GpuPathCoverageHostBuffer ranges;
    GpuPathCoverageHostBuffer spans;
    std::shared_ptr<GpuPathCoverageMaskOwner> mask;
    vk::raii::CommandPool jobPool{nullptr};
    vk::raii::CommandBuffer jobBuffer{nullptr};
    vk::raii::Fence jobFence{nullptr};

    GpuPathCoverageJobState jobState = GpuPathCoverageJobState::Idle;
    bool submitted = false;
    std::uint64_t lastJobBytes = 0;
    bool deviceLost = false;
    std::uint64_t slotToken = 0;
    std::chrono::steady_clock::time_point submittedAt{};
    std::atomic<bool> discardRequested{false};
    GpuPathCoverageDiagnostic jobDiagnostic;
    GpuPathCoverageDiagnostic createDiagnostic;
};

} // namespace bloom::render

#endif // BLOOM_RENDER_VULKAN_GPU_PATH_COVERAGE_PRIVATE_HPP
