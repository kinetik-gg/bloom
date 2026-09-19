#ifndef BLOOM_RENDER_VULKAN_GPU_RESIDENT_DISPLAY_PRIVATE_HPP
#define BLOOM_RENDER_VULKAN_GPU_RESIDENT_DISPLAY_PRIVATE_HPP

// Private to src/render/vulkan. Defines the resident RGBA8 display image impl, the job resource
// set, and the one-job pipeline state behind bloom::render::GpuResidentDisplay. Vulkan-Hpp + VMA
// stay here; the public header exposes no native type.

#include <bloom/render/gpu_resident_display.hpp>

#include "gpu_device_private.hpp"
#include "gpu_image_private.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace bloom::render {

// Proposed minimal private seam against the gpu-solid-prep base (see README):
//   gpu_image.hpp : friend const GpuImageImpl *gpuImageImpl(const GpuImage &image) noexcept;
//   gpu_image.cpp : const GpuImageImpl *gpuImageImpl(const GpuImage &image) noexcept {
//                     return image.impl_.get(); }
// It exposes only the read-only impl for the same render module; no native handle reaches a public
// header.
[[nodiscard]] const GpuImageImpl* gpuImageImpl(const GpuImage& image) noexcept;

struct GpuDisplayImageImpl final {
    GpuDisplayImageImpl() = default;
    GpuDisplayImageImpl(const GpuDisplayImageImpl&) = delete;
    GpuDisplayImageImpl& operator=(const GpuDisplayImageImpl&) = delete;
    ~GpuDisplayImageImpl();

    [[nodiscard]] bool onOwnerThread() const noexcept;
    void destroy() noexcept;

    std::shared_ptr<vulkan_detail::DeviceAllocatorState> state;
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    std::uint64_t allocationBytes = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::optional<ImageWindow> dataWindow;
    std::optional<ImageWindow> displayWindow;
    core::PixelAspectRatio pixelAspect = core::PixelAspectRatio::square();
    std::uint32_t generation = 0;
    bool deviceLost = false;
    // Set while a readback submission references this image and its completion is unproved.
    bool submissionUnretired = false;
};

struct ResidentBuffer final {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VmaAllocationInfo info{};
    std::uint64_t bytes = 0;
    bool hostVisible = false;
};

[[nodiscard]] bool createResidentBuffer(vulkan_detail::DeviceAllocatorState& state,
                                        std::uint64_t bytes, VkBufferUsageFlags usage,
                                        VmaAllocationCreateFlags flags, bool hostVisible,
                                        ResidentBuffer& out);
void destroyResidentBuffer(vulkan_detail::DeviceAllocatorState& state,
                           ResidentBuffer& buffer) noexcept;

// Device-limit/format facts for the resident route: RGBA32F storage buffer reads, packed-output
// buffer, RGBA8_UNORM output image, and the aggregate job bytes.
struct ResidentDisplaySupport final {
    bool supported = false;
    std::uint64_t maxOwnedBytes = 0;
    std::string reason;
};
[[nodiscard]] ResidentDisplaySupport
queryResidentDisplaySupport(vulkan_detail::DeviceAllocatorState& state, std::uint32_t width,
                            std::uint32_t height) noexcept;

[[nodiscard]] bool createDisplayImage(vulkan_detail::DeviceAllocatorState& state,
                                      std::uint32_t width, std::uint32_t height,
                                      GpuDisplayImageImpl& out);

struct GpuResidentDisplay::Impl final {
    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    ~Impl();

    [[nodiscard]] bool onOwnerThread() const noexcept {
        return control != nullptr && control->owner == std::this_thread::get_id();
    }
    void fail(GpuResidentDisplayDiagnosticCode code, std::string message) {
        jobState = GpuResidentDisplayJobState::Failure;
        jobDiagnostic = GpuResidentDisplayDiagnostic{code, std::move(message)};
    }
    void clearJob() {
        jobState = GpuResidentDisplayJobState::Idle;
        jobDiagnostic = GpuResidentDisplayDiagnostic{};
        discardRequested.store(false);
        residentImage.reset();
        input.reset();
        destroyResidentBuffer(*control, jobInput);
        destroyResidentBuffer(*control, jobOutput);
        destroyResidentBuffer(*control, jobStatus);
    }
    [[nodiscard]] bool createPipeline();
    [[nodiscard]] bool drainAndRetire() noexcept;

    std::thread::id owner;
    std::shared_ptr<vulkan_detail::DeviceAllocatorState> control;
    GpuResidentDisplayBudgets budgets;
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

    ResidentBuffer jobInput;
    ResidentBuffer jobOutput;
    ResidentBuffer jobStatus;
    std::unique_ptr<GpuDisplayImage> residentImage;
    std::shared_ptr<const GpuImage> input;

    GpuResidentDisplayJobState jobState = GpuResidentDisplayJobState::Idle;
    bool queueSubmitted = false;
    bool deviceLost = false;
    std::atomic<bool> discardRequested{false};
    GpuResidentDisplayDiagnostic jobDiagnostic;
    GpuResidentDisplayDiagnostic createDiagnostic;
};

} // namespace bloom::render

#endif // BLOOM_RENDER_VULKAN_GPU_RESIDENT_DISPLAY_PRIVATE_HPP
