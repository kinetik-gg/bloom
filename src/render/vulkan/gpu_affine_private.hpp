#ifndef BLOOM_RENDER_VULKAN_GPU_AFFINE_PRIVATE_HPP
#define BLOOM_RENDER_VULKAN_GPU_AFFINE_PRIVATE_HPP

// Private to src/render/vulkan. Shared by gpu_affine.cpp (public API) and gpu_affine_resources.cpp
// (pipeline/buffer helpers). Vulkan-Hpp typed RAII stays here; the public header exposes no native
// type. It reuses the generic CompositeBuffer/CompositePipeline helpers declared by
// gpu_composite_private.hpp (no edit to gpu_composite.* is made) and the
// GpuImageImpl/GpuRendererAccess seams from gpu_image_private.hpp/gpu_device_private.hpp.

#include <bloom/render/gpu_affine.hpp>

#include "gpu_composite_private.hpp"
#include "gpu_device_private.hpp"
#include "gpu_image_private.hpp"
#include "shaders/affine_bilinear_spirv.inc"

#ifdef BLOOM_GPU_SCENE_EXECUTOR_TEST_FAULT_INJECTION
#include "gpu_scene_executor_fault_injection.hpp"
#endif

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>

namespace bloom::render {

// 4 bindings: source image, output image, per-pixel sample buffer, error-flag buffer.
inline constexpr std::uint32_t kAffineBindingCount = 4;
// Reuses the exact translation push-constant layout: output width/height, source width/height,
// opacity. Both are 20 bytes of tightly packed uint32/float32.
inline constexpr std::uint32_t kAffinePushBytes = 20;

static_assert(sizeof(GpuAffineSample) == 16);

// Process-global bounded quarantine accounting and teardown fuse for the affine operation. Kept
// separate from the composite fuse so an unproved affine teardown is reported through
// GpuAffine::teardownDrainIncomplete() rather than the composite one.
[[nodiscard]] bool affineQuarantineAllowed() noexcept;
void noteAffineQuarantine() noexcept;
[[nodiscard]] bool affineTeardownIncomplete() noexcept;

[[nodiscard]] inline GpuAffineDiagnostic affineDiagnostic(const GpuAffineDiagnosticCode code,
                                                          std::string message) {
    return GpuAffineDiagnostic{code, std::move(message)};
}

// The single job state machine behind GpuAffine, defined here so both translation units can
// implement its member operations.
struct GpuAffine::Impl final {
    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    ~Impl();

    [[nodiscard]] bool onOwnerThread() const noexcept {
        return control != nullptr && control->owner == std::this_thread::get_id();
    }
    void fail(GpuAffineDiagnosticCode code, std::string message);
    void clearJob();
    [[nodiscard]] bool createPipelines();
    [[nodiscard]] bool drainAndRetire() noexcept;
    [[nodiscard]] bool checkStatusFlag();
    // Cheap owner/busy/device-generation gate run BEFORE any host metadata allocation so a foreign
    // thread or a stale generation is rejected without O(width*height) work.
    [[nodiscard]] GpuAffineDiagnostic preflightCheap();
    // Shared validation/upload/submit half used by both beginAffine() and beginAffineMatrix().
    [[nodiscard]] GpuAffineDiagnostic
    beginPrepared(const std::shared_ptr<const GpuImage>& source, ImageWindow outputWindow,
                  std::span<const GpuAffineSample> preparedSamples, float opacity,
                  std::uint64_t byteBudget);

    std::thread::id owner;
    std::shared_ptr<vulkan_detail::DeviceAllocatorState> control;
    GpuAffineBudgets budgets;
    std::uint32_t expectedGeneration = 0;

    CompositePipeline affine;
    vk::raii::DescriptorPool descriptorPool{nullptr};
    vk::raii::DescriptorSet affineSet{nullptr};
    vk::raii::CommandPool commandPool{nullptr};
    vk::raii::CommandBuffer commandBuffer{nullptr};
    vk::raii::Fence fence{nullptr};

    std::unique_ptr<GpuImage> residentImage;
    std::shared_ptr<const GpuImage> retainedSource;
    CompositeBuffer samples;
    CompositeBuffer status;
    void* statusMapped = nullptr;

    GpuAffineJobState jobState = GpuAffineJobState::Idle;
    bool queueSubmitted = false;
    std::uint64_t lastJobBytes = 0;
    bool deviceLost = false;
    std::atomic<bool> discardRequested{false};
    GpuAffineDiagnostic jobDiagnostic;
    GpuAffineDiagnostic createDiagnostic;
#ifdef BLOOM_GPU_SCENE_EXECUTOR_TEST_FAULT_INJECTION
    // TEST-ONLY. Compiles only in the fault closure; production builds never see the hook. It lives
    // here so the production translation unit stays within the line budget with identical semantics
    // to the composite poll hook.
    [[nodiscard]] std::optional<GpuAffinePollResult> injectedPollFault() {
        const auto injected = gpu_scene_executor_fault::take();
        if (injected == gpu_scene_executor_fault::PollFault::None) {
            return std::nullopt;
        }
        if (injected == gpu_scene_executor_fault::PollFault::StallPending) {
            return GpuAffinePollResult::Pending;
        }
        if (injected == gpu_scene_executor_fault::PollFault::DeviceLost) {
            const VkFence faultFence = static_cast<VkFence>(*fence);
            const VkResult faultWait = control->device.getDispatcher()->vkWaitForFences(
                static_cast<VkDevice>(*control->device), 1, &faultFence, VK_TRUE, 1'000'000'000ULL);
            if (faultWait == VK_SUCCESS) {
                deviceLost = true;
                queueSubmitted = false;
                fail(GpuAffineDiagnosticCode::DeviceLost,
                     "injected device loss after proven retirement");
            } else {
                fail(GpuAffineDiagnosticCode::DeviceUnavailable,
                     "injected device loss could not prove fence retirement; the submission is "
                     "retained");
            }
            return GpuAffinePollResult::Failure;
        }
        fail(GpuAffineDiagnosticCode::DeviceUnavailable,
             "injected unknown fence status; the submission is not retired");
        return GpuAffinePollResult::Failure;
    }
#endif

};

} // namespace bloom::render

#endif // BLOOM_RENDER_VULKAN_GPU_AFFINE_PRIVATE_HPP
