#ifndef BLOOM_RENDER_VULKAN_GPU_NEUTRAL_DISPLAY_PRIVATE_HPP
#define BLOOM_RENDER_VULKAN_GPU_NEUTRAL_DISPLAY_PRIVATE_HPP

// Private implementation surface for the fixed Bloom Neutral v1 display compute operation. Split
// out so the public dispatch translation unit and the resource/retirement translation unit share
// one Impl definition without either sprawling past the repository's source-size budget. Never
// included by a public header, the CPU stub, or a consumer.

#include <bloom/render/gpu_neutral_display.hpp>

#include "gpu_device_private.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace bloom::render::neutral_display_detail {

inline constexpr std::uint32_t kWorkgroupSizeX = 256;
inline constexpr std::uint64_t kInputBytesPerPixel = 16;
inline constexpr std::uint64_t kOutputBytesPerPixel = 4;
inline constexpr std::uint64_t kStatusBytes = 4;
inline constexpr std::uint64_t kDrainTimeoutNanoseconds = 2ULL * 1000ULL * 1000ULL * 1000ULL;
// A budget larger than this is refused as InvalidArgument rather than trusted; no first-slice
// device or frame needs it, and it keeps the retention arithmetic far from overflow.
inline constexpr std::uint64_t kMaxBudgetBytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
// Bounded process-wide quarantine fuse: a timed-out teardown leaks its generation rather than
// destroying a queue-busy device, and after this many leaks no further pipeline is created, so a
// repeatedly timing-out driver cannot grow the process without bound.
inline constexpr std::uint32_t kMaxQuarantines = 4;

[[nodiscard]] std::uint64_t gpuBytesForPixels(std::uint32_t pixelCount) noexcept;

struct StorageBuffer final {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VmaAllocationInfo info{};
    std::uint64_t capacityBytes = 0;
};

void destroyStorageBuffer(vulkan_detail::DeviceAllocatorState& state,
                          StorageBuffer& storage) noexcept;
[[nodiscard]] bool createStorageBuffer(vulkan_detail::DeviceAllocatorState& state,
                                       std::uint64_t bytes, VkBufferUsageFlags usage,
                                       VmaAllocationCreateFlags flags, StorageBuffer& storage);

[[nodiscard]] std::string errorFlagMessage(std::uint32_t flags);

[[nodiscard]] bool quarantineAllowed() noexcept;
void noteQuarantine() noexcept;
[[nodiscard]] bool teardownIncomplete() noexcept;

// Test-only fence-status substitution, so the retirement contract (an unknown VkGetFenceStatus
// result must NOT be treated as retired) is exercised without a driver that misbehaves on demand.
// Production code never sets it.
enum class FenceOverride : std::int32_t {
    None = 0,
    NotReady = 1,
    Success = 2,
    Unknown = 3,
};
void setFenceOverride(FenceOverride override) noexcept;
[[nodiscard]] VkResult effectiveFenceStatus(VkResult real) noexcept;

} // namespace bloom::render::neutral_display_detail

namespace bloom::render {

struct GpuNeutralDisplay::Impl final {
    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    ~Impl();

    [[nodiscard]] bool onOwnerThread() const noexcept {
        return std::this_thread::get_id() == owner;
    }
    void fail(GpuNeutralDisplayDiagnosticCode code, std::string message) {
        jobState = GpuNeutralDisplayJobState::Failure;
        jobDiagnostic = GpuNeutralDisplayDiagnostic{code, std::move(message)};
    }
    void clearJob() {
        jobState = GpuNeutralDisplayJobState::Idle;
        jobPixelCount = 0;
        frameByteBudget = 0;
        discardRequested.store(false);
        jobDiagnostic = GpuNeutralDisplayDiagnostic{};
        frame.clear();
    }

    // Builds every cached Vulkan object once. Returns false and leaves `createDiagnostic` set on
    // failure; the caller drops the partially built Impl, whose RAII members release safely because
    // nothing has been submitted.
    [[nodiscard]] bool createPipeline() noexcept;

    // Grows or shrinks the retained input/output buffers so that the retained GPU bytes are within
    // both the hard maxOwnedBytes ceiling and the per-request budget, freeing the idle old buffers
    // before allocating the new ones so the grow peak never exceeds the ceiling.
    [[nodiscard]] bool ensureCapacity(std::uint32_t pixelCount, std::uint64_t perRequestBudget);

    void updateDescriptors() const;
    void destroyBuffers() noexcept;

    // Bounded owner-thread drain. Returns true when the queue submission is proved retired (fence
    // signalled or the device is lost); false when retirement could not be confirmed, in which case
    // the owning GpuNeutralDisplay must quarantine rather than destroy.
    [[nodiscard]] bool drainAndRetire() noexcept;

    std::thread::id owner;
    std::shared_ptr<vulkan_detail::DeviceAllocatorState> state;
    GpuNeutralDisplayBudgets budgets;
    std::uint32_t expectedGeneration = 0;

    vk::raii::ShaderModule shaderModule{nullptr};
    vk::raii::DescriptorSetLayout descriptorSetLayout{nullptr};
    vk::raii::PipelineLayout pipelineLayout{nullptr};
    vk::raii::Pipeline pipeline{nullptr};
    vk::raii::DescriptorPool descriptorPool{nullptr};
    vk::raii::DescriptorSet descriptorSet{nullptr};
    vk::raii::CommandPool commandPool{nullptr};
    vk::raii::CommandBuffer commandBuffer{nullptr};
    vk::raii::Fence fence{nullptr};

    neutral_display_detail::StorageBuffer input;
    neutral_display_detail::StorageBuffer output;
    neutral_display_detail::StorageBuffer status;

    std::uint64_t capacityPixels = 0;
    std::uint64_t ownedBytes = 0;

    GpuNeutralDisplayJobState jobState = GpuNeutralDisplayJobState::Idle;
    // Retirement state, deliberately independent of the API-facing jobState: true from a successful
    // vkQueueSubmit until the fence is observed signalled or the device is lost. An unknown fence
    // query leaves it true so the destructor still drains and never frees a live submission.
    bool queueSubmitted = false;
    bool deviceLost = false;
    std::uint32_t jobPixelCount = 0;
    std::uint64_t frameByteBudget = 0;
    std::atomic<bool> discardRequested{false};
    GpuNeutralDisplayDiagnostic jobDiagnostic;
    std::vector<Rgba8> frame;
    GpuNeutralDisplayDiagnostic createDiagnostic;
};

} // namespace bloom::render

#endif // BLOOM_RENDER_VULKAN_GPU_NEUTRAL_DISPLAY_PRIVATE_HPP
