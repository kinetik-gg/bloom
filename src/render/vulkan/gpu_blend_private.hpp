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
#include "shaders/blend_portable_spirv.inc"
#include "shaders/blend_spirv.inc"

#ifdef BLOOM_GPU_SCENE_EXECUTOR_TEST_FAULT_INJECTION
#include "gpu_scene_executor_fault_injection.hpp"
#endif

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
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

// Sentinel for an Impl that owns no bounded resident-pool slot.
inline constexpr std::size_t kBlendNoResidentSlot = static_cast<std::size_t>(-1);

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
    // Bounded resident-slot management, defined in gpu_blend_retirement.cpp. Acquire is called
    // before the first native allocation; release is owner-thread retirement; orphan is the
    // foreign-thread or unproven owner path that preserves the already-owned slot for the owner
    // drain. The drain is a static member because it names the private Impl type.
    [[nodiscard]] bool acquireResidentSlot() noexcept;
    void releaseResidentSlot() noexcept;
    void orphanResidentSlot() noexcept;
    static void drainResidentOrphansOnOwnerThread() noexcept;
    // Acquires the bounded slot and lazily builds the pipelines/layouts/descriptor set/command
    // resources under it on the first begin. A full pool refuses before any native allocation; a
    // creation failure resets the partial native state and returns the slot. Returns false with
    // createDiagnostic set. Defined in gpu_blend.cpp.
    [[nodiscard]] bool ensureResidentReady();
    // Frees every pipeline/descriptor/command/fence native resource. Called only on the owner
    // thread after a failed ensureResidentReady() so an Impl that then holds no resident slot owns
    // no Vulkan object and may be destroyed from any thread.
    void resetPipelineResources() noexcept;

    std::thread::id owner;
    std::shared_ptr<vulkan_detail::DeviceAllocatorState> control;
    GpuBlendBudgets budgets;
    GpuBlendKernelPolicy policy = GpuBlendKernelPolicy::Auto;
    std::uint32_t expectedGeneration = 0;
    // Bounded resident-pool slot owned by this Impl from before its first native allocation until
    // owner-thread release. kBlendNoResidentSlot means this Impl owns no native resources.
    std::size_t residentSlot = kBlendNoResidentSlot;
    bool pipelinesReady = false;

    // pipeline is the exact Float32 blend.comp kernel for Normal/Add. pipelineF64 is the exact
    // Float64 kernel for the six general modes, built only when the device advertised and enabled
    // shaderFloat64. pipelinePortable is the compensated-Float32 general-mode kernel, built
    // whenever the Float64 companion is not selected. `generalUsesF64` records which of the two the
    // six general modes dispatch, and therefore which identity the executor keys them under.
    CompositePipeline pipeline;
    CompositePipeline pipelineF64;
    CompositePipeline pipelinePortable;
    bool generalUsesF64 = false;
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
