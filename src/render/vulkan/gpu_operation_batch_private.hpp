#pragma once

#include "gpu_affine_private.hpp"
#include "gpu_solid_private.hpp"
#include "shaders/solid_spirv.inc"

#include <bloom/render/gpu_operation_batch.hpp>

#include <array>
#include <optional>

namespace bloom::render::operation_batch_detail {
enum class PollFault : std::uint8_t { None, Pending, Unknown, LostAfterProof, StatusAfterProof };
inline std::atomic<std::size_t> orphanCount{0};
struct Job final {
    std::unique_ptr<GpuImageImpl> image;
    std::array<CompositeBuffer, 3> metadata;
    std::shared_ptr<const GpuImage> sourcePin;
    std::shared_ptr<const GpuImage> destinationPin;
    const GpuImageImpl* source = nullptr;
    const GpuImageImpl* destination = nullptr;
    VkDescriptorSet descriptors = VK_NULL_HANDLE;
    std::array<std::uint32_t, 6> push{};
    std::uint32_t pushBytes = 0;
    std::size_t kind = 0;
};
struct State final {
    std::shared_ptr<vulkan_detail::DeviceAllocatorState> control;
    std::array<CompositePipeline, 4> pipelines;
    VkDescriptorPool descriptors = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    std::array<Job, kGpuOperationBatchCapacity> jobs;
    std::uint32_t count = 0;
    std::uint32_t expectedGeneration = 0;
    GpuOperationBatchState state = GpuOperationBatchState::Idle;
    GpuOperationBatchDiagnostic diagnostic;
    GpuOperationBatchStatistics statistics;
    bool ownsSlot = false;
    bool submitted = false;
    bool cancelled = false;
    bool lost = false;
    bool initialized = false;
    bool initializationAttempted = false;
    bool orphaned = false;
    PollFault fault = PollFault::None;

    ~State();
    [[nodiscard]] bool owner() const noexcept;
    [[nodiscard]] bool initialize();
    void clear(bool restorePrefix) noexcept;
    void fail(GpuOperationBatchCode, const char*) noexcept;
    [[nodiscard]] bool prepare(std::span<const GpuBatchOperation>, const GpuOperationBatchBudgets&,
                               std::uint64_t byteBudget);
    [[nodiscard]] bool record();
    [[nodiscard]] bool submit();
    void poll() noexcept;
};
void retire(State*) noexcept;
void drainRetired(vulkan_detail::DeviceAllocatorState&) noexcept;
} // namespace bloom::render::operation_batch_detail
