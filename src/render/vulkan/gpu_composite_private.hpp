#ifndef BLOOM_RENDER_VULKAN_GPU_COMPOSITE_PRIVATE_HPP
#define BLOOM_RENDER_VULKAN_GPU_COMPOSITE_PRIVATE_HPP

// Private to src/render/vulkan. Shared by gpu_composite.cpp (public API) and
// gpu_composite_resources.cpp (pipeline/buffer helpers). Vulkan-Hpp typed RAII stays here; the
// public header exposes no native type. This file does NOT mutate the solid mirror: it only
// consumes the existing GpuImageImpl/GpuRendererAccess seams declared in gpu_image_private.hpp and
// gpu_device_private.hpp.

#include <bloom/render/gpu_composite.hpp>

#include "gpu_device_private.hpp"
#include "gpu_image_private.hpp"
#include "shaders/source_over_spirv.inc"
#include "shaders/translation_opacity_spirv.inc"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace bloom::render {

// One operation's cached pipeline state. Built once per GpuComposite on the owner thread.
struct CompositePipeline final {
    vk::raii::ShaderModule shaderModule{nullptr};
    vk::raii::DescriptorSetLayout descriptorSetLayout{nullptr};
    vk::raii::PipelineLayout pipelineLayout{nullptr};
    vk::raii::Pipeline pipeline{nullptr};
};

// A bounded device-local buffer holding host-prepared metadata (axis samples) or the status flag
// word. Co-owns the device allocator generation. Owner-thread destruction only.
struct CompositeBuffer final {
    CompositeBuffer() = default;
    CompositeBuffer(const CompositeBuffer&) = delete;
    CompositeBuffer& operator=(const CompositeBuffer&) = delete;
    CompositeBuffer(CompositeBuffer&& other) noexcept { *this = std::move(other); }
    CompositeBuffer& operator=(CompositeBuffer&& other) noexcept;
    ~CompositeBuffer();
    void release() noexcept;

    std::shared_ptr<vulkan_detail::DeviceAllocatorState> state;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    std::uint64_t bytes = 0;
    bool hostVisible = false;
    bool armed = false;
};

// Creates a device-local (or host-visible) buffer and, when `initialData` is non-null, uploads it
// through a transient staging buffer synchronously on the owner thread. Returns false and leaves
// `out` untouched on any failure; every transient resource is RAII. `hostVisible` buffers are used
// for the status word so poll() can map it.
[[nodiscard]] bool createCompositeBuffer(vulkan_detail::DeviceAllocatorState& state,
                                         std::uint64_t bytes, bool hostVisible,
                                         const void* initialData, CompositeBuffer& out) noexcept;

// Builds one compute pipeline from the given SPIR-V with `bindingCount` storage bindings (the first
// N are storage images, the rest storage buffers) and a push-constant range of `pushBytes`.
// `bindingIsImage` has one entry per binding; true = storage image, false = storage buffer. The
// order is explicit because SourceOverV1's third binding is an image while TranslationOpacity's
// third is a buffer.
[[nodiscard]] bool createCompositePipeline(vulkan_detail::DeviceAllocatorState& state,
                                           const std::uint32_t* spirvCode, std::uint32_t spirvBytes,
                                           const bool* bindingIsImage, std::uint32_t bindingCount,
                                           std::uint32_t pushBytes, std::string& reason,
                                           CompositePipeline& out) noexcept;

// Process-global bounded quarantine accounting and teardown fuse, shared by both translation units.
[[nodiscard]] bool compositeQuarantineAllowed() noexcept;
void noteCompositeQuarantine() noexcept;
[[nodiscard]] bool compositeTeardownIncomplete() noexcept;

// Shared constants and layouts used by both translation units.
inline constexpr std::uint32_t kCompositeWorkgroupSizeX = 256;
inline constexpr std::uint32_t kTranslationBindingCount = 5; // 2 images + x axis + y axis + status
inline constexpr std::uint32_t kSourceOverBindingCount = 4;  // source + backdrop + output + status
inline constexpr std::uint32_t kTranslationPushBytes = 20;
inline constexpr std::uint32_t kSourceOverPushBytes = 24;

struct CompositeTranslationPush final {
    std::uint32_t outputWidth;
    std::uint32_t outputHeight;
    std::uint32_t sourceWidth;
    std::uint32_t sourceHeight;
    float opacity;
};
static_assert(sizeof(CompositeTranslationPush) == kTranslationPushBytes);

struct CompositeSourceOverPush final {
    std::uint32_t destWidth;
    std::uint32_t destHeight;
    std::uint32_t sourceWidth;
    std::uint32_t sourceHeight;
    std::int32_t sourceOffsetX;
    std::int32_t sourceOffsetY;
};
static_assert(sizeof(CompositeSourceOverPush) == kSourceOverPushBytes);

struct CompositeAxisSampleGpu final {
    std::int32_t base;
    float factor;
};
static_assert(sizeof(CompositeAxisSampleGpu) == sizeof(GpuAxisSample));

[[nodiscard]] inline GpuCompositeDiagnostic
compositeDiagnostic(const GpuCompositeDiagnosticCode code, std::string message) {
    return GpuCompositeDiagnostic{code, std::move(message)};
}

[[nodiscard]] inline std::uint64_t
compositeDispatchGroupCount(const std::uint64_t pixels) noexcept {
    return (pixels + kCompositeWorkgroupSizeX - 1ULL) / kCompositeWorkgroupSizeX;
}

// Overflow-safe multiply/add. The requested byte math multiplies two uint32 extents, which can
// overflow uint64 for a hostile request, so it must never wrap into a small "fits" value.
[[nodiscard]] inline bool compositeMultiplyOverflows(const std::uint64_t a, const std::uint64_t b,
                                                     std::uint64_t& out) noexcept {
    return __builtin_mul_overflow(a, b, &out);
}

[[nodiscard]] inline bool compositeAddOverflows(const std::uint64_t a, const std::uint64_t b,
                                                std::uint64_t& out) noexcept {
    return __builtin_add_overflow(a, b, &out);
}

// The ACTUAL bytes VMA allocated for a composite metadata/status buffer (allocator rounding
// included), not the requested size. Zero for an unarmed buffer.
[[nodiscard]] std::uint64_t compositeBufferAllocationBytes(const CompositeBuffer& buffer) noexcept;

// The ACTUAL bytes VMA allocated for a resident image's backing allocation.
[[nodiscard]] std::uint64_t compositeImageAllocationBytes(const GpuImageImpl& image) noexcept;

// True only when an input image belongs to exactly this composite's device generation. A null
// image, a moved-from image, or an image from a foreign GpuDevice fails closed. Called before any
// driver resource is created or bound.
[[nodiscard]] inline bool compositeImageBelongsTo(
    const GpuImageImpl* image,
    const std::shared_ptr<vulkan_detail::DeviceAllocatorState>& control) noexcept {
    return image != nullptr && control != nullptr && image->state.get() == control.get();
}

// The canonical read-only resident-image seam, provided by the solid/resident mirror's
// gpu_image.cpp and declared friend in gpu_image.hpp. The composite does not define or duplicate
// it.
[[nodiscard]] const GpuImageImpl* gpuImageImpl(const GpuImage& image) noexcept;

// The single job state machine behind GpuComposite. Defined here (not in the .cpp) so both
// gpu_composite.cpp and gpu_composite_resources.cpp can implement its member operations without a
// second public header or a second Impl definition.
struct GpuComposite::Impl final {
    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    ~Impl();

    [[nodiscard]] bool onOwnerThread() const noexcept {
        return control != nullptr && control->owner == std::this_thread::get_id();
    }
    void fail(GpuCompositeDiagnosticCode code, std::string message);
    void clearJob();
    [[nodiscard]] bool createPipelines();
    [[nodiscard]] bool drainAndRetire() noexcept;
    [[nodiscard]] bool checkStatusFlag();

    std::thread::id owner;
    std::shared_ptr<vulkan_detail::DeviceAllocatorState> control;
    GpuCompositeBudgets budgets;
    std::uint32_t expectedGeneration = 0;

    CompositePipeline translation;
    CompositePipeline sourceOver;
    vk::raii::DescriptorPool descriptorPool{nullptr};
    vk::raii::DescriptorSet translationSet{nullptr};
    vk::raii::DescriptorSet sourceOverSet{nullptr};
    vk::raii::CommandPool commandPool{nullptr};
    vk::raii::CommandBuffer commandBuffer{nullptr};
    vk::raii::Fence fence{nullptr};

    std::unique_ptr<GpuImage> residentImage;
    std::shared_ptr<const GpuImage> retainedSource;
    std::shared_ptr<const GpuImage> retainedDestination;
    CompositeBuffer axisX;
    CompositeBuffer axisY;
    CompositeBuffer status;
    void* statusMapped = nullptr;
    bool translationJob = false;

    GpuCompositeJobState jobState = GpuCompositeJobState::Idle;
    bool queueSubmitted = false;
    std::uint64_t lastJobBytes = 0;
    bool deviceLost = false;
    std::atomic<bool> discardRequested{false};
    GpuCompositeDiagnostic jobDiagnostic;
    GpuCompositeDiagnostic createDiagnostic;
};

} // namespace bloom::render

#endif // BLOOM_RENDER_VULKAN_GPU_COMPOSITE_PRIVATE_HPP
