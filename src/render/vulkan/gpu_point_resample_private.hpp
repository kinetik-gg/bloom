#ifndef BLOOM_RENDER_VULKAN_GPU_POINT_RESAMPLE_PRIVATE_HPP
#define BLOOM_RENDER_VULKAN_GPU_POINT_RESAMPLE_PRIVATE_HPP

// Private to src/render/vulkan. The bounded reservation and native resource set behind the
// PointResampleV1 producer. This mirrors the accepted GpuOutputColorReadback slot contract: exactly
// one submission may be Reserved or Quarantined process-wide. A reservation is acquired BEFORE any
// native allocation or submit; a proven retirement returns it to Free; an unproven retirement moves
// the exact submission (device generation, source, axis buffer, output, pipeline, command
// pool/buffer, fence) into the retained quarantine so only the genuine owner thread can later prove
// retirement or observe device loss. No per-object unbounded growth: a foreign-thread destruction
// retains the one occupied reservation instead of allocating a new one, and admission is refused
// while it is occupied.

#include <bloom/render/gpu_point_resample.hpp>

#include "gpu_composite_private.hpp"
#include "gpu_device_private.hpp"
#include "gpu_image_private.hpp"
#include "gpu_point_resample_dispatch.hpp"
#include "gpu_point_resample_fault.hpp"
#include "shaders/point_resample_spirv.inc"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>

namespace bloom::render {

// 3 bindings: source image, output image, per-axis source-index buffer.
inline constexpr std::uint32_t kPointResampleBindingCount = 3;
// Two tightly packed uint32 push constants: output data-window width/height.
inline constexpr std::uint32_t kPointResamplePushBytes = 8;

struct PointResamplePush final {
    std::uint32_t outputWidth;
    std::uint32_t outputHeight;
};
static_assert(sizeof(PointResamplePush) == kPointResamplePushBytes);

[[nodiscard]] inline GpuPointResampleDiagnostic
pointResampleDiagnostic(const GpuPointResampleDiagnosticCode code, std::string message) {
    return GpuPointResampleDiagnostic{code, std::move(message)};
}

// Live-native-resource accounting (defined in gpu_point_resample_resources.cpp).
void notePointResampleResourceSetCreated() noexcept;
void notePointResampleResourceSetFreed() noexcept;
void notePointResampleQuarantine() noexcept;
[[nodiscard]] bool pointResampleTeardownIncomplete() noexcept;

namespace point_resample_detail {

using vulkan_detail::DeviceAllocatorState;

// Bounded live-resident accounting and the owner-drainable retained-resident store, defined in
// gpu_point_resample_resources.cpp.
[[nodiscard]] bool acquireResidentSlot() noexcept;
void releaseResidentSlot() noexcept;
// Allocation-free: finds a free fixed-capacity slot first and only then moves `resident` into it,
// so a full store leaves the caller's unique_ptr ownership untouched (never destroying the image on
// this thread). Never throws.
[[nodiscard]] bool retainResident(const std::shared_ptr<DeviceAllocatorState>& deviceState,
                                  std::unique_ptr<GpuImage>& resident,
                                  std::thread::id ownerThread) noexcept;
// Allocation-free owner drain. Never throws.
[[nodiscard]] std::uint32_t retireRetainedForOwner() noexcept;
[[nodiscard]] std::uint32_t liveResidents() noexcept;
[[nodiscard]] std::uint32_t retainedResidents() noexcept;

enum class ReservationState : std::uint8_t {
    // Reusable by any owner thread.
    Free,
    // A live Impl owns every native resource. Never reclaimed by anyone until that Impl retires or
    // quarantines.
    Reserved,
    // A possibly in-flight submission was transferred here; only the owner thread whose fence is
    // proven retired (or the device is lost) may free/reclaim it.
    Quarantined,
};

// The exact native resource set of one PointResampleV1 submission. Owned by the live Impl while
// Reserved and by the slot while Quarantined.
struct ResourceSet final {
    std::shared_ptr<DeviceAllocatorState> deviceState;
    std::shared_ptr<const GpuImage> source;
    CompositeBuffer axis;
    std::unique_ptr<GpuImage> resident;
    CompositePipeline pipeline;
    vk::raii::DescriptorPool descriptorPool{nullptr};
    vk::raii::DescriptorSet descriptorSet{nullptr};
    vk::raii::CommandPool commandPool{nullptr};
    vk::raii::CommandBuffer commandBuffer{nullptr};
    vk::raii::Fence fence{nullptr};
    // True while this set owns one bounded live-resident slot for `resident`.
    bool residentSlotHeld = false;

    // Frees the per-job submission resources but keeps `resident` and its resident slot (used when
    // a proven-retired result is published).
    void releaseJobOnly() noexcept {
        fence = vk::raii::Fence{nullptr};
        commandBuffer = vk::raii::CommandBuffer{nullptr};
        commandPool = vk::raii::CommandPool{nullptr};
        descriptorSet = vk::raii::DescriptorSet{nullptr};
        descriptorPool = vk::raii::DescriptorPool{nullptr};
        pipeline = CompositePipeline{};
        axis.release();
        source.reset();
    }

    void reset() noexcept {
        if (residentSlotHeld) {
            releaseResidentSlot();
            residentSlotHeld = false;
        }
        releaseJobOnly();
        resident.reset();
        deviceState.reset();
    }
};

struct Reservation final {
    ReservationState state = ReservationState::Free;
    std::thread::id ownerThread{};
    std::uint64_t token = 0;
    ResourceSet resources;
};

inline Reservation& reservation() {
    static auto* const value = new Reservation();
    return *value;
}

inline std::mutex& reservationMutex() {
    static auto* const value = new std::mutex();
    return *value;
}

inline bool isReservationOwnerLocked(const Reservation& slot) noexcept {
    return slot.state != ReservationState::Free && slot.ownerThread == std::this_thread::get_id();
}

inline void freeQuarantinedLocked(Reservation& slot) noexcept {
    slot.resources.reset();
    slot.state = ReservationState::Free;
    slot.ownerThread = std::thread::id{};
    notePointResampleResourceSetFreed();
}

// Owner-only, non-blocking retirement of a quarantined submission. Returns true when the fence is
// proven retired (or the device is lost) and the native resource set is freed.
inline bool retireQuarantinedLocked(Reservation& slot) noexcept {
    if (slot.state != ReservationState::Quarantined || slot.resources.deviceState == nullptr ||
        slot.resources.fence == vk::raii::Fence{nullptr}) {
        return false;
    }
    // Test seam: ForceFenceTimeout keeps a quarantined submission un-retirable so admission refusal
    // and recovery can be proven deterministically.
    if (static_cast<PointResampleFault>(pointResampleFault().load()) ==
        PointResampleFault::ForceFenceTimeout) {
        return false;
    }
    const VkFence rawFence = static_cast<VkFence>(*slot.resources.fence);
    const VkResult status = slot.resources.deviceState->device.getDispatcher()->vkGetFenceStatus(
        static_cast<VkDevice>(*slot.resources.deviceState->device), rawFence);
    if (status != VK_SUCCESS && status != VK_ERROR_DEVICE_LOST) {
        return false;
    }
    freeQuarantinedLocked(slot);
    return true;
}

} // namespace point_resample_detail

// The single job state machine behind GpuPointResample. Native per-job resources are created only
// after the bounded reservation is acquired, so many pre-created instances hold no native
// resources.
struct GpuPointResample::Impl final {
    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    ~Impl();

    [[nodiscard]] bool onOwnerThread() const noexcept {
        return control != nullptr && control->owner == std::this_thread::get_id();
    }
    void fail(GpuPointResampleDiagnosticCode code, std::string message);
    void clearJob();
    // Frees every native per-job resource and returns a held Reserved claim to Free. Owner-thread
    // only. Never frees another owner's Quarantined resources.
    void freeJobResources() noexcept;
    // Frees the native resources of a proven-retired job and returns the claim to Free, keeping a
    // published resident image. Owner-thread only.
    void releaseRetired() noexcept;
    // Returns a Reserved claim taken before native work. Owner-thread only.
    void releaseClaim() noexcept;
    // Transfers a possibly in-flight submission into the process-wide quarantine. Owner-thread
    // only. On success the Impl becomes empty; the quarantine retains the exact resource set.
    [[nodiscard]] bool quarantine() noexcept;
    [[nodiscard]] bool createPipelines();
    // Allocates and records the per-job resources, enforces the actual-allocation budgets, and
    // submits one dispatch. Returns false and sets jobDiagnostic on any failure (resources remain
    // owned by Impl for the caller to free).
    [[nodiscard]] bool launchJob(const std::shared_ptr<const GpuImage>& source,
                                 std::uint32_t outputWidth, std::uint32_t outputHeight,
                                 ImageWindow displayWindow, core::PixelAspectRatio pixelAspect,
                                 std::span<const std::int32_t> axisIndices,
                                 std::uint64_t byteBudget);

    std::thread::id owner;
    std::shared_ptr<point_resample_detail::DeviceAllocatorState> control;
    GpuPointResampleBudgets budgets;
    std::uint32_t expectedGeneration = 0;

    // Per-job native resources, live only while a reservation is held (pending or quarantine).
    point_resample_detail::ResourceSet resources;
    bool resourcesLive = false;

    GpuPointResampleJobState jobState = GpuPointResampleJobState::Idle;
    bool queueSubmitted = false;
    bool deviceLost = false;
    std::uint64_t lastJobBytes = 0;
    std::uint64_t slotToken = 0;
    std::chrono::steady_clock::time_point submittedAt{};
    std::atomic<bool> discardRequested{false};
    GpuPointResampleDiagnostic jobDiagnostic;
    GpuPointResampleDiagnostic createDiagnostic;
};

} // namespace bloom::render

#endif // BLOOM_RENDER_VULKAN_GPU_POINT_RESAMPLE_PRIVATE_HPP
