#ifndef BLOOM_RENDER_VULKAN_GPU_OCIO_PROGRAM_PRIVATE_HPP
#define BLOOM_RENDER_VULKAN_GPU_OCIO_PROGRAM_PRIVATE_HPP

// Private to src/render/vulkan. Defines the LUT/UBO/job resource set and the one-job pipeline state
// behind bloom::render::GpuOcioProgram. Vulkan-Hpp + VMA stay here; the public header exposes no
// native type.

#include <bloom/render/gpu_ocio_program.hpp>

#include "gpu_device_private.hpp"
#include "gpu_image_private.hpp"
#include "gpu_resident_display_private.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace bloom::render::ocio_program_detail {

inline constexpr std::uint32_t kWorkgroupSizeX = 64;
inline constexpr std::uint64_t kStatusBytes = 4;
inline constexpr std::uint64_t kDrainTimeoutNanoseconds = 2ULL * 1000ULL * 1000ULL * 1000ULL;
inline constexpr std::uint32_t kMaxQuarantines = 4;

// One uploaded OCIO sampled texture plus its sampler. Filtering follows the declared interpolation:
// nearest for INTERP_NEAREST and linear otherwise (tetrahedral weighting is performed by OCIO's own
// emitted shader, so no colour approximation is applied here).
enum class UploadOutcome : std::uint8_t { Retired, Quarantined, Failed, OverBudget, Cancelled };

struct SampledResource final {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    std::uint64_t allocationBytes = 0;
    std::uint32_t binding = 0;
    std::uint64_t sampleBytes = 0;
};

[[nodiscard]] UploadOutcome
createSampledResource(const std::shared_ptr<vulkan_detail::DeviceAllocatorState>& owner,
                      const OcioGpuTextureDesc& texture, std::uint64_t remainingOwnedBytes,
                      const GpuOcioProgramCancellation& cancellation,
                      SampledResource& out) noexcept;
void destroySampledResource(vulkan_detail::DeviceAllocatorState& state,
                            SampledResource& resource) noexcept;

[[nodiscard]] bool quarantineAllowed() noexcept;
void noteQuarantine() noexcept;
[[nodiscard]] bool teardownIncomplete() noexcept;
[[nodiscard]] bool uploadQuarantineOccupied() noexcept;
void retireUploadQuarantine() noexcept;

inline void writeImageDescriptor(const vk::raii::DescriptorSet& set,
                                 vulkan_detail::DeviceAllocatorState& state,
                                 const std::uint32_t binding, const VkImageView view) {
    const vk::DescriptorImageInfo info{nullptr, view, vk::ImageLayout::eGeneral};
    vk::WriteDescriptorSet write{};
    write.dstSet = *set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = vk::DescriptorType::eStorageImage;
    write.pImageInfo = &info;
    state.device.getDispatcher()->vkUpdateDescriptorSets(
        static_cast<VkDevice>(*state.device), 1,
        reinterpret_cast<const VkWriteDescriptorSet*>(&write), 0, nullptr);
}

inline void writeBufferDescriptor(const vk::raii::DescriptorSet& set,
                                  vulkan_detail::DeviceAllocatorState& state,
                                  const std::uint32_t binding, const VkBuffer buffer,
                                  const std::uint64_t bytes) {
    const vk::DescriptorBufferInfo info{buffer, 0, bytes};
    vk::WriteDescriptorSet write{};
    write.dstSet = *set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = vk::DescriptorType::eStorageBuffer;
    write.pBufferInfo = &info;
    state.device.getDispatcher()->vkUpdateDescriptorSets(
        static_cast<VkDevice>(*state.device), 1,
        reinterpret_cast<const VkWriteDescriptorSet*>(&write), 0, nullptr);
}

inline void writeUniformDescriptor(const vk::raii::DescriptorSet& set,
                                   vulkan_detail::DeviceAllocatorState& state,
                                   const std::uint32_t binding, const VkBuffer buffer,
                                   const std::uint64_t bytes) {
    const vk::DescriptorBufferInfo info{buffer, 0, bytes};
    vk::WriteDescriptorSet write{};
    write.dstSet = *set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = vk::DescriptorType::eUniformBuffer;
    write.pBufferInfo = &info;
    state.device.getDispatcher()->vkUpdateDescriptorSets(
        static_cast<VkDevice>(*state.device), 1,
        reinterpret_cast<const VkWriteDescriptorSet*>(&write), 0, nullptr);
}

inline void writeCombinedDescriptor(const vk::raii::DescriptorSet& set,
                                    vulkan_detail::DeviceAllocatorState& state,
                                    const std::uint32_t binding, const VkSampler sampler,
                                    const VkImageView view) {
    const vk::DescriptorImageInfo info{sampler, view, vk::ImageLayout::eShaderReadOnlyOptimal};
    vk::WriteDescriptorSet write{};
    write.dstSet = *set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
    write.pImageInfo = &info;
    state.device.getDispatcher()->vkUpdateDescriptorSets(
        static_cast<VkDevice>(*state.device), 1,
        reinterpret_cast<const VkWriteDescriptorSet*>(&write), 0, nullptr);
}

} // namespace bloom::render::ocio_program_detail

namespace bloom::render {

struct GpuOcioProgram::Impl final {
    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    ~Impl();

    [[nodiscard]] bool onOwnerThread() const noexcept {
        return control != nullptr && control->owner == std::this_thread::get_id();
    }
    void fail(GpuOcioProgramDiagnosticCode code, std::string message);
    void beginImpl(bool display, const std::shared_ptr<const GpuImage>& input,
                   std::span<const std::byte> uniformBytes, std::uint64_t byteBudget);
    void releaseTransient();
    void clearJob();
    [[nodiscard]] bool createPipeline();
    [[nodiscard]] bool createOcioResources();
    [[nodiscard]] bool drainAndRetire() noexcept;
    [[nodiscard]] bool checkStatus();
    void writeStaticDescriptors();
    void writeInputDescriptor(const GpuImageImpl& image);
    void writeEffectOutputDescriptor(const GpuImageImpl& image);
    void writePackedDescriptor();

    std::thread::id owner;
    std::shared_ptr<vulkan_detail::DeviceAllocatorState> control;
    GpuOcioProgramBudgets budgets;
    std::uint32_t expectedGeneration = 0;
    OcioGpuProgramDesc desc;
    std::vector<std::uint32_t> spirv;
    GpuOcioProgramCancellation cancellation;
    bool displayArm = false;

    vk::raii::ShaderModule shaderModule{nullptr};
    vk::raii::DescriptorSetLayout ocioSetLayout{nullptr};
    vk::raii::DescriptorSetLayout ioSetLayout{nullptr};
    vk::raii::PipelineLayout pipelineLayout{nullptr};
    vk::raii::Pipeline pipeline{nullptr};
    vk::raii::DescriptorPool descriptorPool{nullptr};
    vk::raii::DescriptorSet ocioSet{nullptr};
    vk::raii::DescriptorSet ioSet{nullptr};
    vk::raii::CommandPool commandPool{nullptr};
    vk::raii::CommandBuffer commandBuffer{nullptr};
    vk::raii::Fence fence{nullptr};

    std::vector<ocio_program_detail::SampledResource> textures;
    ResidentBuffer uniformBuffer;
    std::uint8_t* uniformMapped = nullptr;
    ResidentBuffer status;
    std::uint8_t* statusMapped = nullptr;
    ResidentBuffer packed;
    std::unique_ptr<GpuImage> effectOutput;
    std::unique_ptr<GpuDisplayImage> displayOutput;
    std::uint64_t retainedResourceBytes = 0;

    GpuOcioProgramJobState jobState = GpuOcioProgramJobState::Idle;
    bool queueSubmitted = false;
    bool deviceLost = false;
    std::uint32_t jobPixelCount = 0;
    std::uint32_t jobWidth = 0;
    std::uint32_t jobHeight = 0;
    std::uint64_t lastJobBytes = 0;
    std::atomic<bool> discardRequested{false};
    GpuOcioProgramDiagnostic jobDiagnostic;
    GpuOcioProgramDiagnostic createDiagnostic;

    std::shared_ptr<const GpuImage> inputRetained;
};

} // namespace bloom::render

#endif // BLOOM_RENDER_VULKAN_GPU_OCIO_PROGRAM_PRIVATE_HPP
