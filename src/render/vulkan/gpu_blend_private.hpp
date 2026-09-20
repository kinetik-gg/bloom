#ifndef BLOOM_RENDER_VULKAN_GPU_BLEND_PRIVATE_HPP
#define BLOOM_RENDER_VULKAN_GPU_BLEND_PRIVATE_HPP

// Private to src/render/vulkan. The single-job state machine behind the additive GpuBlend class.
// Vulkan-Hpp typed RAII stays here; the public header exposes no native type.
//
// This header deliberately reuses the already-qualified private seams from
// gpu_composite_private.hpp and gpu_image_private.hpp (CompositePipeline/CompositeBuffer,
// createCompositePipeline/ createCompositeBuffer, createResidentImage/querySolidImageSupport,
// gpuImageImpl) instead of duplicating them or editing gpu_composite.*. It does not mutate the
// composite mirror.

#include <bloom/render/gpu_blend.hpp>

#include "gpu_composite_private.hpp"
#include "gpu_image_private.hpp"
#include "shaders/blend_f64_spirv.inc"
#include "shaders/blend_spirv.inc"

#ifdef BLOOM_GPU_SCENE_EXECUTOR_TEST_FAULT_INJECTION
#include "gpu_scene_executor_fault_injection.hpp"
#endif

#include <atomic>
#include <optional>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace bloom::render {

// BlendV1 bindings: source + backdrop + output storage images, then the error-flag storage buffer.
inline constexpr std::uint32_t kBlendBindingCount = 4;
inline constexpr std::uint32_t kBlendPushBytes = 28;

struct BlendPush final {
    std::uint32_t destWidth;
    std::uint32_t destHeight;
    std::uint32_t sourceWidth;
    std::uint32_t sourceHeight;
    std::int32_t sourceOffsetX;
    std::int32_t sourceOffsetY;
    std::uint32_t mode;
};
static_assert(sizeof(BlendPush) == kBlendPushBytes);

[[nodiscard]] inline GpuBlendDiagnostic blendDiagnostic(const GpuBlendDiagnosticCode code,
                                                        std::string message) {
    return GpuBlendDiagnostic{code, std::move(message)};
}

struct GpuBlend::Impl final {
    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    ~Impl();

    [[nodiscard]] bool onOwnerThread() const noexcept {
        return control != nullptr && control->owner == std::this_thread::get_id();
    }
    void fail(GpuBlendDiagnosticCode code, std::string message);
    void clearJob();
    [[nodiscard]] bool createPipeline();
    [[nodiscard]] bool drainAndRetire() noexcept;
    [[nodiscard]] bool checkStatusFlag();

    std::thread::id owner;
    std::shared_ptr<vulkan_detail::DeviceAllocatorState> control;
    GpuBlendBudgets budgets;
    std::uint32_t expectedGeneration = 0;

    // pipeline is the Float32 blend.comp kernel (Normal/Add, and every mode on a device without
    // shaderFloat64). pipelineF64 is the exact Float64 kernel, built only when the device
    // advertised and enabled shaderFloat64; f64Available records whether it exists.
    CompositePipeline pipeline;
    CompositePipeline pipelineF64;
    bool f64Available = false;
    vk::raii::DescriptorPool descriptorPool{nullptr};
    vk::raii::DescriptorSet descriptorSet{nullptr};
    vk::raii::CommandPool commandPool{nullptr};
    vk::raii::CommandBuffer commandBuffer{nullptr};
    vk::raii::Fence fence{nullptr};

    std::unique_ptr<GpuImage> residentImage;
    std::shared_ptr<const GpuImage> retainedSource;
    std::shared_ptr<const GpuImage> retainedDestination;
    CompositeBuffer status;
    void* statusMapped = nullptr;

    GpuBlendJobState jobState = GpuBlendJobState::Idle;
    bool queueSubmitted = false;
    std::uint64_t lastJobBytes = 0;
    bool deviceLost = false;
    std::atomic<bool> discardRequested{false};
    GpuBlendDiagnostic jobDiagnostic;
    GpuBlendDiagnostic createDiagnostic;
#ifdef BLOOM_GPU_SCENE_EXECUTOR_TEST_FAULT_INJECTION
    // TEST-ONLY. Compiles only in the fault closure; production builds never see the hook. It lives
    // here so the production translation unit stays within the line budget with identical semantics
    // to the composite poll hook.
    [[nodiscard]] std::optional<GpuBlendPollResult> injectedPollFault() {
        const auto injected = gpu_scene_executor_fault::take();
        if (injected == gpu_scene_executor_fault::PollFault::None) {
            return std::nullopt;
        }
        if (injected == gpu_scene_executor_fault::PollFault::StallPending) {
            return GpuBlendPollResult::Pending;
        }
        if (injected == gpu_scene_executor_fault::PollFault::DeviceLost) {
            const VkFence faultFence = static_cast<VkFence>(*fence);
            const VkResult faultWait = control->device.getDispatcher()->vkWaitForFences(
                static_cast<VkDevice>(*control->device), 1, &faultFence, VK_TRUE, 1'000'000'000ULL);
            if (faultWait == VK_SUCCESS) {
                deviceLost = true;
                queueSubmitted = false;
                fail(GpuBlendDiagnosticCode::DeviceLost,
                     "injected device loss after proven retirement");
            } else {
                fail(GpuBlendDiagnosticCode::DeviceUnavailable,
                     "injected device loss could not prove fence retirement; the submission is "
                     "retained");
            }
            return GpuBlendPollResult::Failure;
        }
        fail(GpuBlendDiagnosticCode::DeviceUnavailable,
             "injected unknown fence status; the submission is not retired");
        return GpuBlendPollResult::Failure;
    }
#endif

};

} // namespace bloom::render

#endif // BLOOM_RENDER_VULKAN_GPU_BLEND_PRIVATE_HPP
