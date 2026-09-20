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
#include <cstddef>
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

// Packed GPU layout of the compact O(1) inverse-affine map: six binary64 coefficients, each split
// into an error-free (hi, lo) Float32 pair, matching the shader's `vec2 map[6]` std430 buffer. The
// order is localX@origin, localY@origin, d(localX)/d(column), d(localX)/d(row),
// d(localY)/d(column), d(localY)/d(row).
struct GpuAffineMapGpu final {
    float coefficients[12] = {};

    friend bool operator==(const GpuAffineMapGpu&, const GpuAffineMapGpu&) noexcept = default;
};
static_assert(sizeof(GpuAffineMapGpu) == 48);

[[nodiscard]] inline GpuAffineMapGpu packAffineMap(const GpuAffineMap& map) noexcept {
    const auto split = [](const double value, float& high, float& low) noexcept {
        high = static_cast<float>(value);
        low = static_cast<float>(value - static_cast<double>(high));
    };
    GpuAffineMapGpu packed;
    split(map.localXAtOrigin, packed.coefficients[0], packed.coefficients[1]);
    split(map.localYAtOrigin, packed.coefficients[2], packed.coefficients[3]);
    split(map.stepXPerColumn, packed.coefficients[4], packed.coefficients[5]);
    split(map.stepXPerRow, packed.coefficients[6], packed.coefficients[7]);
    split(map.stepYPerColumn, packed.coefficients[8], packed.coefficients[9]);
    split(map.stepYPerRow, packed.coefficients[10], packed.coefficients[11]);
    return packed;
}

[[nodiscard]] inline GpuAffineDiagnostic affineDiagnostic(const GpuAffineDiagnosticCode code,
                                                          std::string message) {
    return GpuAffineDiagnostic{code, std::move(message)};
}

// The single job state machine behind GpuAffine, defined here so both translation units can
// implement its member operations.
//
// Sentinel for an Impl that owns no bounded resident-pool slot.
inline constexpr std::size_t kAffineNoResidentSlot = static_cast<std::size_t>(-1);

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
    [[nodiscard]] GpuAffineDiagnostic beginPrepared(const std::shared_ptr<const GpuImage>& source,
                                                    ImageWindow outputWindow,
                                                    const GpuAffineMap& affineMap, float opacity,
                                                    std::uint64_t byteBudget);
    // Bounded resident-slot management, defined in gpu_affine_retirement.cpp. Acquire is called
    // before the first native allocation; release is owner-thread retirement; orphan is the
    // foreign-thread or unproven owner path that preserves the already-owned slot for the owner
    // drain. The drain is a static member because it names the private Impl type.
    [[nodiscard]] bool acquireResidentSlot() noexcept;
    void releaseResidentSlot() noexcept;
    void orphanResidentSlot() noexcept;
    static void drainResidentOrphansOnOwnerThread() noexcept;
    // Acquires the bounded slot and lazily builds the pipeline/layout/descriptor set/command
    // resources under it on the first begin. A full pool refuses before any native allocation; a
    // creation failure resets the partial native state and returns the slot. Returns false with
    // createDiagnostic set. Defined in gpu_affine.cpp.
    [[nodiscard]] bool ensureResidentReady();
    // Frees every pipeline/descriptor/command/fence native resource. Called only on the owner
    // thread after a failed ensureResidentReady() so an Impl that then holds no resident slot owns
    // no Vulkan object and may be destroyed from any thread.
    void resetPipelineResources() noexcept;

    std::thread::id owner;
    std::shared_ptr<vulkan_detail::DeviceAllocatorState> control;
    GpuAffineBudgets budgets;
    std::uint32_t expectedGeneration = 0;
    // Bounded resident-pool slot owned by this Impl from before its first native allocation until
    // owner-thread release. kAffineNoResidentSlot means this Impl owns no native resources.
    std::size_t residentSlot = kAffineNoResidentSlot;
    bool pipelinesReady = false;

    CompositePipeline affine;
    vk::raii::DescriptorPool descriptorPool{nullptr};
    vk::raii::DescriptorSet affineSet{nullptr};
    vk::raii::CommandPool commandPool{nullptr};
    vk::raii::CommandBuffer commandBuffer{nullptr};
    vk::raii::Fence fence{nullptr};

    std::unique_ptr<GpuImage> residentImage;
    std::shared_ptr<const GpuImage> retainedSource;
    CompositeBuffer map;
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
