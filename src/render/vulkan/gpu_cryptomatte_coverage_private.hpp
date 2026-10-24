#ifndef BLOOM_RENDER_VULKAN_GPU_CRYPTOMATTE_COVERAGE_PRIVATE_HPP
#define BLOOM_RENDER_VULKAN_GPU_CRYPTOMATTE_COVERAGE_PRIVATE_HPP

// Private to src/render/vulkan. The bounded reservation and native resource set behind the
// CryptomatteCoverageV1 producer. This mirrors the accepted GpuOutputColorReadback slot contract:
// exactly one submission may be Reserved or Quarantined process-wide. A reservation is acquired
// BEFORE any native allocation or submit; a proven retirement returns it to Free; an unproven
// retirement moves the exact submission (device generation, source, ID buffer, output, pipeline,
// command pool/buffer, fence) into the retained quarantine so only the genuine owner thread can
// later prove retirement or observe device loss. No per-object unbounded growth: a foreign-thread
// destruction retains the one occupied reservation instead of allocating a new one, and admission
// is refused while it is occupied.

#include <bloom/render/gpu_cryptomatte_coverage.hpp>

#include "gpu_composite_private.hpp"
#include "gpu_cryptomatte_coverage_dispatch.hpp"
#include "gpu_cryptomatte_coverage_fault.hpp"
#include "gpu_device_private.hpp"
#include "gpu_image_private.hpp"
#include "shaders/cryptomatte_coverage_spirv.inc"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>

namespace bloom::render {

// 3 bindings: stacked rank source image, output image, selected-ID buffer.
inline constexpr std::uint32_t kCryptomatteCoverageBindingCount = 3;
// Four tightly packed uint32 push constants: output width/height, rank count, ID count.
inline constexpr std::uint32_t kCryptomatteCoveragePushBytes = 16;

struct CryptomatteCoveragePush final {
    std::uint32_t outputWidth;
    std::uint32_t outputHeight;
    std::uint32_t rankCount;
    std::uint32_t idCount;
};
static_assert(sizeof(CryptomatteCoveragePush) == kCryptomatteCoveragePushBytes);

[[nodiscard]] inline GpuCryptomatteCoverageDiagnostic
cryptomatteCoverageDiagnostic(const GpuCryptomatteCoverageDiagnosticCode code,
                              std::string message) {
    return GpuCryptomatteCoverageDiagnostic{code, std::move(message)};
}

// Live-native-resource accounting (defined in gpu_cryptomatte_coverage_resources.cpp).
void noteCryptomatteCoverageResourceSetCreated() noexcept;
void noteCryptomatteCoverageResourceSetFreed() noexcept;
void noteCryptomatteCoverageQuarantine() noexcept;
[[nodiscard]] bool cryptomatteCoverageTeardownIncomplete() noexcept;

namespace cryptomatte_coverage_detail {

using vulkan_detail::DeviceAllocatorState;

// Bounded live-resident accounting and the owner-drainable retained-resident store, defined in
// gpu_cryptomatte_coverage_resources.cpp.
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

// The exact native resource set of one CryptomatteCoverageV1 submission. Owned by the live Impl
// while Reserved and by the slot while Quarantined.
struct ResourceSet final {
    std::shared_ptr<DeviceAllocatorState> deviceState;
    std::shared_ptr<const GpuImage> source;
    CompositeBuffer ids;
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
        ids.release();
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
    noteCryptomatteCoverageResourceSetFreed();
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
    if (static_cast<CryptomatteCoverageFault>(cryptomatteCoverageFault().load()) ==
        CryptomatteCoverageFault::ForceFenceTimeout) {
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

} // namespace cryptomatte_coverage_detail

// The single job state machine behind GpuCryptomatteCoverage. Native per-job resources are created
// only after the bounded reservation is acquired, so many pre-created instances hold no native
// resources.
struct GpuCryptomatteCoverage::Impl final {
    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    ~Impl();

    [[nodiscard]] bool onOwnerThread() const noexcept {
        return control != nullptr && control->owner == std::this_thread::get_id();
    }
    void fail(GpuCryptomatteCoverageDiagnosticCode code, std::string message);
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
                                 ImageWindow outputWindow, ImageWindow displayWindow,
                                 core::PixelAspectRatio pixelAspect, std::uint32_t rankCount,
                                 std::span<const std::uint32_t> ids, std::uint64_t byteBudget);

    std::thread::id owner;
    std::shared_ptr<cryptomatte_coverage_detail::DeviceAllocatorState> control;
    GpuCryptomatteCoverageBudgets budgets;
    std::uint32_t expectedGeneration = 0;

    // Per-job native resources, live only while a reservation is held (pending or quarantine).
    cryptomatte_coverage_detail::ResourceSet resources;
    bool resourcesLive = false;

    GpuCryptomatteCoverageJobState jobState = GpuCryptomatteCoverageJobState::Idle;
    bool queueSubmitted = false;
    bool deviceLost = false;
    std::uint64_t lastJobBytes = 0;
    std::uint64_t slotToken = 0;
    std::uint64_t quarantineToken = 0;
    std::chrono::steady_clock::time_point submittedAt{};
    std::atomic<bool> discardRequested{false};
    GpuCryptomatteCoverageDiagnostic jobDiagnostic;
    GpuCryptomatteCoverageDiagnostic createDiagnostic;
};

} // namespace bloom::render

#endif // BLOOM_RENDER_VULKAN_GPU_CRYPTOMATTE_COVERAGE_PRIVATE_HPP
