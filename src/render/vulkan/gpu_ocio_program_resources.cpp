#include "gpu_ocio_program_private.hpp"

#include "../ocio_gpu_program_fault.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace bloom::render::ocio_program_detail {

[[nodiscard]] bool reserveUploadQuarantineIfFree() noexcept;
void releaseUploadQuarantineFlag() noexcept;

namespace {
std::atomic<std::uint32_t> g_quarantineCount{0};
std::atomic<bool> g_teardownIncomplete{false};
std::atomic<bool> g_faultNotReady{false};
std::atomic<bool> g_faultCancelAfterSubmit{false};
std::atomic_flag g_uploadQuarantineFlag = ATOMIC_FLAG_INIT;

[[nodiscard]] VkFormat channelFormat(const OcioGpuTextureChannel channel,
                                     const std::uint32_t components) noexcept {
    if (components == 4) {
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
    return channel == OcioGpuTextureChannel::Red ? VK_FORMAT_R32_SFLOAT
                                                 : VK_FORMAT_R32G32B32_SFLOAT;
}

[[nodiscard]] std::uint32_t channelComponents(const OcioGpuTextureChannel channel) noexcept {
    return channel == OcioGpuTextureChannel::Red ? 1U : 3U;
}

[[nodiscard]] VkImageType imageType(const OcioGpuTextureDimensions dimensions) noexcept {
    switch (dimensions) {
    case OcioGpuTextureDimensions::OneD:
        return VK_IMAGE_TYPE_1D;
    case OcioGpuTextureDimensions::TwoD:
        return VK_IMAGE_TYPE_2D;
    case OcioGpuTextureDimensions::ThreeD:
        return VK_IMAGE_TYPE_3D;
    }
    return VK_IMAGE_TYPE_2D;
}

[[nodiscard]] VkImageViewType viewType(const OcioGpuTextureDimensions dimensions) noexcept {
    switch (dimensions) {
    case OcioGpuTextureDimensions::OneD:
        return VK_IMAGE_VIEW_TYPE_1D;
    case OcioGpuTextureDimensions::TwoD:
        return VK_IMAGE_VIEW_TYPE_2D;
    case OcioGpuTextureDimensions::ThreeD:
        return VK_IMAGE_VIEW_TYPE_3D;
    }
    return VK_IMAGE_VIEW_TYPE_2D;
}

[[nodiscard]] VkExtent3D textureExtent(const OcioGpuTextureDesc& texture) noexcept {
    switch (texture.dimensions) {
    case OcioGpuTextureDimensions::OneD:
        return {texture.width, 1, 1};
    case OcioGpuTextureDimensions::TwoD:
        return {texture.width, texture.height, 1};
    case OcioGpuTextureDimensions::ThreeD:
        return {texture.edgeLength, texture.edgeLength, texture.edgeLength};
    }
    return {1, 1, 1};
}

// RAII owner for the raw VMA image until its ownership is transferred (either to a SampledResource
// or to the upload quarantine). Prevents a leak if any later step throws.
struct ImageOwner final {
    vulkan_detail::DeviceAllocatorState* state = nullptr;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;

    ImageOwner() = default;
    ImageOwner(const ImageOwner&) = delete;
    ImageOwner& operator=(const ImageOwner&) = delete;
    ~ImageOwner() {
        if (image != VK_NULL_HANDLE && state != nullptr) {
            vmaDestroyImage(state->allocator, image, allocation);
        }
    }
    void release() noexcept {
        image = VK_NULL_HANDLE;
        allocation = VK_NULL_HANDLE;
    }
};

struct Staging final {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    vulkan_detail::DeviceAllocatorState* state = nullptr;
    ~Staging() {
        if (buffer != VK_NULL_HANDLE && state != nullptr) {
            vmaDestroyBuffer(state->allocator, buffer, allocation);
        }
    }
    void release() noexcept {
        buffer = VK_NULL_HANDLE;
        allocation = VK_NULL_HANDLE;
    }
};

struct UploadQuarantine final {
    std::shared_ptr<vulkan_detail::DeviceAllocatorState> state;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation imageAllocation = VK_NULL_HANDLE;
    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation stagingAllocation = VK_NULL_HANDLE;
    vk::raii::CommandPool pool{nullptr};
    vk::raii::CommandBuffer command{nullptr};
    vk::raii::Fence fence{nullptr};
};

UploadQuarantine* quarantineSlot() {
    static auto* const slot = new UploadQuarantine();
    return slot;
}

void releaseQuarantineResources(UploadQuarantine& slot) noexcept {
    if (slot.state == nullptr) {
        return;
    }
    if (slot.staging != VK_NULL_HANDLE) {
        vmaDestroyBuffer(slot.state->allocator, slot.staging, slot.stagingAllocation);
    }
    slot.staging = VK_NULL_HANDLE;
    if (slot.image != VK_NULL_HANDLE) {
        vmaDestroyImage(slot.state->allocator, slot.image, slot.imageAllocation);
    }
    slot.image = VK_NULL_HANDLE;
    slot.fence = vk::raii::Fence{nullptr};
    slot.command = vk::raii::CommandBuffer{nullptr};
    slot.pool = vk::raii::CommandPool{nullptr};
    slot.state.reset();
}

// Moves every resource the outstanding submission may still use into the bounded single-slot
// quarantine, so nothing is destroyed while the GPU can still read it.
void quarantineUpload(UploadQuarantine& slot, UploadOutcome& outcome, ImageOwner& image,
                      Staging& staging, vk::raii::CommandPool& pool,
                      vk::raii::CommandBuffer& command, vk::raii::Fence& fence,
                      std::shared_ptr<vulkan_detail::DeviceAllocatorState> state,
                      const bool latchFuse) noexcept {
    // The bounded retention slot was reserved before any allocation or submission, so this path
    // never allocates; it only moves the resources the GPU may still touch into that slot.
    slot.state = std::move(state);
    slot.image = image.image;
    slot.imageAllocation = image.allocation;
    image.release();
    slot.staging = staging.buffer;
    slot.stagingAllocation = staging.allocation;
    staging.release();
    slot.pool = std::move(pool);
    slot.command = std::move(command);
    slot.fence = std::move(fence);
    if (latchFuse) {
        noteQuarantine();
    }
    outcome = UploadOutcome::Quarantined;
}

[[nodiscard]] bool formatSupports(vulkan_detail::DeviceAllocatorState& state, const VkFormat format,
                                  const bool linear) noexcept {
    if (state.physicalDevice == nullptr) {
        return false;
    }
    VkFormatProperties properties{};
    state.physicalDevice.getDispatcher()->vkGetPhysicalDeviceFormatProperties(
        static_cast<VkPhysicalDevice>(*state.physicalDevice), format, &properties);
    VkFormatFeatureFlags required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if (linear) {
        required |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    }
    return (properties.optimalTilingFeatures & required) == required;
}

[[nodiscard]] bool dimensionsSupported(vulkan_detail::DeviceAllocatorState& state,
                                       const OcioGpuTextureDesc& texture) noexcept {
    if (state.physicalDevice == nullptr) {
        return false;
    }
    VkPhysicalDeviceProperties properties{};
    state.physicalDevice.getDispatcher()->vkGetPhysicalDeviceProperties(
        static_cast<VkPhysicalDevice>(*state.physicalDevice), &properties);
    switch (texture.dimensions) {
    case OcioGpuTextureDimensions::OneD:
        return texture.width <= properties.limits.maxImageDimension1D;
    case OcioGpuTextureDimensions::TwoD:
        return texture.width <= properties.limits.maxImageDimension2D &&
               texture.height <= properties.limits.maxImageDimension2D;
    case OcioGpuTextureDimensions::ThreeD:
        return texture.edgeLength <= properties.limits.maxImageDimension3D;
    }
    return false;
}

[[nodiscard]] UploadOutcome
uploadImage(vulkan_detail::DeviceAllocatorState& state,
            const std::shared_ptr<vulkan_detail::DeviceAllocatorState>& owner, ImageOwner& image,
            UploadQuarantine& slot, const VkExtent3D extent, const void* data,
            const std::uint64_t bytes, const std::uint64_t remainingOwnedBytes,
            const std::uint64_t hostScratchBytes, const GpuOcioProgramCancellation& cancellation,
            std::string& reason) {
    const VkDevice device = static_cast<VkDevice>(*state.device);
    const auto* dispatcher = state.device.getDispatcher();
    if (cancellation && cancellation()) {
        reason = "the LUT upload was cancelled before submission";
        return UploadOutcome::Failed;
    }
    Staging staging;
    staging.state = &state;
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = bytes;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocationInfo.flags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo mappedInfo{};
    if (vmaCreateBuffer(state.allocator, &bufferInfo, &allocationInfo, &staging.buffer,
                        &staging.allocation, &mappedInfo) != VK_SUCCESS ||
        mappedInfo.pMappedData == nullptr) {
        reason = "the LUT staging buffer could not be created";
        return UploadOutcome::Failed;
    }
    std::memcpy(mappedInfo.pMappedData, data, static_cast<std::size_t>(bytes));
    // The staging allocation is host-visible but not guaranteed host-coherent; make the CPU writes
    // visible to the transfer engine explicitly.
    static_cast<void>(vmaFlushAllocation(state.allocator, staging.allocation, 0,
                                         static_cast<VkDeviceSize>(bytes)));

    // Actual host-visible staging bytes + the (already-created) image allocation + any padded host
    // scratch must fit the caller's remaining owned budget before the submission is recorded.
    std::uint64_t actual = 0;
    if (__builtin_add_overflow(static_cast<std::uint64_t>(mappedInfo.size), hostScratchBytes,
                               &actual) ||
        actual > remainingOwnedBytes) {
        reason = "the LUT upload exceeds the remaining owned-byte budget";
        return UploadOutcome::OverBudget;
    }
    vk::raii::CommandPool pool{nullptr};
    vk::raii::CommandBuffer command{nullptr};
    vk::raii::Fence fence{nullptr};
    {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolInfo.queueFamilyIndex = state.computeQueueFamily;
        VkCommandPool rawPool = VK_NULL_HANDLE;
        if (dispatcher->vkCreateCommandPool(device, &poolInfo, nullptr, &rawPool) != VK_SUCCESS) {
            reason = "the LUT upload command pool could not be created";
            return UploadOutcome::Failed;
        }
        pool = vk::raii::CommandPool(state.device, rawPool);
        VkCommandBufferAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocateInfo.commandPool = rawPool;
        allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateInfo.commandBufferCount = 1;
        VkCommandBuffer rawCommand = VK_NULL_HANDLE;
        if (dispatcher->vkAllocateCommandBuffers(device, &allocateInfo, &rawCommand) !=
            VK_SUCCESS) {
            reason = "the LUT upload command buffer could not be allocated";
            return UploadOutcome::Failed;
        }
        command = vk::raii::CommandBuffer(state.device, rawCommand, rawPool);
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence rawFence = VK_NULL_HANDLE;
        if (dispatcher->vkCreateFence(device, &fenceInfo, nullptr, &rawFence) != VK_SUCCESS) {
            reason = "the LUT upload fence could not be created";
            return UploadOutcome::Failed;
        }
        fence = vk::raii::Fence(state.device, rawFence);
    }
    const VkCommandBuffer raw = static_cast<VkCommandBuffer>(*command);
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (dispatcher->vkBeginCommandBuffer(raw, &beginInfo) != VK_SUCCESS) {
        reason = "the LUT upload command buffer could not begin";
        return UploadOutcome::Failed;
    }
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    dispatcher->vkCmdPipelineBarrier(raw, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                     &barrier);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = extent;
    dispatcher->vkCmdCopyBufferToImage(raw, staging.buffer, image.image,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    dispatcher->vkCmdPipelineBarrier(raw, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                                     nullptr, 1, &barrier);
    if (dispatcher->vkEndCommandBuffer(raw) != VK_SUCCESS) {
        reason = "the LUT upload command buffer could not end";
        return UploadOutcome::Failed;
    }
    if (cancellation && cancellation()) {
        reason = "the LUT upload was cancelled before submission";
        return UploadOutcome::Failed;
    }
    const VkFence rawFence = static_cast<VkFence>(*fence);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &raw;
    if (dispatcher->vkQueueSubmit(static_cast<VkQueue>(*state.computeQueue), 1, &submit,
                                  rawFence) != VK_SUCCESS) {
        reason = "the LUT upload submission failed";
        return UploadOutcome::Failed;
    }
    // The submission is now outstanding. From here no owned resource may be destroyed on an
    // unproven outcome; a quarantine slot retains every resource the GPU can still touch.
    UploadOutcome outcome = UploadOutcome::Retired;
    if (g_faultNotReady.load()) {
        quarantineUpload(slot, outcome, image, staging, pool, command, fence, owner, true);
        reason = "the LUT upload completion is unproven (fault-injected)";
        return outcome;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    VkResult waited = VK_NOT_READY;
    while (true) {
        if (g_faultCancelAfterSubmit.load() || (cancellation && cancellation())) {
            quarantineUpload(slot, outcome, image, staging, pool, command, fence, owner, false);
            reason = "the LUT upload was cancelled after submission";
            return UploadOutcome::Cancelled;
        }
        waited = dispatcher->vkWaitForFences(device, 1, &rawFence, VK_TRUE, 1000000ULL);
        if (waited == VK_SUCCESS || waited == VK_ERROR_DEVICE_LOST) {
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            break;
        }
    }
    if (waited == VK_SUCCESS) {
        return UploadOutcome::Retired;
    }
    quarantineUpload(slot, outcome, image, staging, pool, command, fence, owner, true);
    reason = waited == VK_ERROR_DEVICE_LOST ? "the device was lost during LUT upload"
                                            : "the LUT upload completion is unproven";
    return outcome;
}

} // namespace

bool uploadQuarantineReserved() noexcept {
    return g_uploadQuarantineFlag.test(std::memory_order_acquire);
}

bool reserveUploadQuarantineIfFree() noexcept {
    // Touch the preallocated slot BEFORE any submission so no allocation can fail between submit
    // and a quarantine transfer.
    static_cast<void>(quarantineSlot());
    if (g_uploadQuarantineFlag.test_and_set(std::memory_order_acq_rel)) {
        return false;
    }
    return true;
}

void releaseUploadQuarantineFlag() noexcept {
    g_uploadQuarantineFlag.clear(std::memory_order_release);
}

UploadOutcome
createSampledResource(const std::shared_ptr<vulkan_detail::DeviceAllocatorState>& owner,
                      const OcioGpuTextureDesc& texture, const std::uint64_t remainingOwnedBytes,
                      const GpuOcioProgramCancellation& cancellation,
                      SampledResource& out) noexcept {
    if (cancellation && cancellation()) {
        return UploadOutcome::Failed;
    }
    if (!reserveUploadQuarantineIfFree()) {
        // A retention slot must exist before we allocate or submit anything; refuse typed.
        return UploadOutcome::Failed;
    }
    bool retained = false;
    struct SlotGuard final {
        bool& retained;
        ~SlotGuard() {
            if (!retained) {
                releaseUploadQuarantineFlag();
            }
        }
    } slotGuard{retained};
    try {
        vulkan_detail::DeviceAllocatorState& state = *owner;
        const VkDevice device = static_cast<VkDevice>(*state.device);
        const auto* dispatcher = state.device.getDispatcher();
        if (!dimensionsSupported(state, texture)) {
            return UploadOutcome::Failed;
        }
        const bool linear = texture.interpolation != OcioGpuInterpolation::Nearest;
        const std::uint32_t nativeComponents = channelComponents(texture.channel);
        std::uint32_t components = nativeComponents;
        VkFormat format = channelFormat(texture.channel, nativeComponents);
        if (!formatSupports(state, format, linear)) {
            // RGBA is almost universal; padding preserves exact texel meaning (the shader reads
            // only .rgb / .r).
            format = VK_FORMAT_R32G32B32A32_SFLOAT;
            components = 4;
            if (!formatSupports(state, format, linear)) {
                return UploadOutcome::Failed;
            }
        }
        const VkExtent3D extent = textureExtent(texture);
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = imageType(texture.dimensions);
        imageInfo.format = format;
        imageInfo.extent = extent;
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo allocationInfo{};
        allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        ImageOwner image;
        image.state = &state;
        VmaAllocationInfo mapped{};
        if (vmaCreateImage(state.allocator, &imageInfo, &allocationInfo, &image.image,
                           &image.allocation, &mapped) != VK_SUCCESS) {
            return UploadOutcome::Failed;
        }
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = image.image;
        viewInfo.viewType = viewType(texture.dimensions);
        viewInfo.format = format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView rawView = VK_NULL_HANDLE;
        if (dispatcher->vkCreateImageView(device, &viewInfo, nullptr, &rawView) != VK_SUCCESS) {
            return UploadOutcome::Failed;
        }
        vk::raii::ImageView view(state.device, rawView);
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        samplerInfo.minFilter = samplerInfo.magFilter;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.maxLod = 0.0F;
        VkSampler rawSampler = VK_NULL_HANDLE;
        if (dispatcher->vkCreateSampler(device, &samplerInfo, nullptr, &rawSampler) != VK_SUCCESS) {
            return UploadOutcome::Failed;
        }
        vk::raii::Sampler sampler(state.device, rawSampler);

        const std::size_t texels =
            static_cast<std::size_t>(extent.width) * extent.height * extent.depth;
        std::vector<float> padded;
        const void* source = texture.samples.data();
        if (components != nativeComponents) {
            padded.resize(texels * components, 1.0F);
            for (std::size_t t = 0; t < texels; ++t) {
                const std::size_t from = t * nativeComponents;
                const std::size_t to = t * components;
                for (std::uint32_t c = 0; c < nativeComponents; ++c) {
                    padded[to + c] = texture.samples[from + c];
                }
                padded[to + nativeComponents] = 1.0F;
            }
            source = padded.data();
        }
        const std::uint64_t bytes = static_cast<std::uint64_t>(texels) * components * sizeof(float);
        std::string reason;
        const std::uint64_t hostScratchBytes = padded.empty() ? 0U : padded.size() * sizeof(float);
        const UploadOutcome outcome =
            uploadImage(state, owner, image, *quarantineSlot(), extent, source, bytes,
                        remainingOwnedBytes, hostScratchBytes, cancellation, reason);
        if (outcome == UploadOutcome::Quarantined || outcome == UploadOutcome::Cancelled) {
            // The image is now owned by the quarantine; the sampler/view are not referenced by the
            // outstanding copy and are safely destroyed by RAII. Keep the reserved slot.
            retained = true;
            return outcome;
        }
        if (outcome != UploadOutcome::Retired) {
            return UploadOutcome::Failed;
        }
        out.image = image.image;
        out.view = *view;
        out.sampler = *sampler;
        out.allocation = image.allocation;
        out.allocationBytes = mapped.size;
        out.binding = texture.binding;
        out.sampleBytes = texture.samples.size() * sizeof(float);
        view.release();
        sampler.release();
        image.release();
        return UploadOutcome::Retired;
    } catch (...) {
        return UploadOutcome::Failed;
    }
}

void destroySampledResource(vulkan_detail::DeviceAllocatorState& state,
                            SampledResource& resource) noexcept {
    if (resource.image == VK_NULL_HANDLE) {
        return;
    }
    const VkDevice device = static_cast<VkDevice>(*state.device);
    const auto* dispatcher = state.device.getDispatcher();
    if (resource.sampler != VK_NULL_HANDLE) {
        dispatcher->vkDestroySampler(device, resource.sampler, nullptr);
    }
    if (resource.view != VK_NULL_HANDLE) {
        dispatcher->vkDestroyImageView(device, resource.view, nullptr);
    }
    vmaDestroyImage(state.allocator, resource.image, resource.allocation);
    resource = SampledResource{};
}

bool quarantineAllowed() noexcept { return !g_teardownIncomplete.load(); }
void noteQuarantine() noexcept {
    g_quarantineCount.fetch_add(1);
    g_teardownIncomplete.store(true);
}
bool teardownIncomplete() noexcept { return g_teardownIncomplete.load(); }

bool uploadQuarantineOccupied() noexcept { return g_uploadQuarantineFlag.test(); }

void retireUploadQuarantine() noexcept {
    UploadQuarantine& slot = *quarantineSlot();
    if (slot.state == nullptr) {
        releaseUploadQuarantineFlag();
        return;
    }
    if (slot.state->owner != std::this_thread::get_id()) {
        // Foreign-thread reclamation must never touch the driver or destroy resources.
        return;
    }
    const VkFence rawFence = static_cast<VkFence>(*slot.fence);
    const VkResult status = slot.state->device.getDispatcher()->vkGetFenceStatus(
        static_cast<VkDevice>(*slot.state->device), rawFence);
    if (status != VK_SUCCESS && status != VK_ERROR_DEVICE_LOST) {
        return; // still unproven; keep every resource and the flag set
    }
    releaseQuarantineResources(slot);
    releaseUploadQuarantineFlag();
}

void setUploadFenceOverrideForTest(const UploadFenceOverride override) noexcept {
    g_faultNotReady.store(override == UploadFenceOverride::NotReady);
    g_faultCancelAfterSubmit.store(override == UploadFenceOverride::CancelAfterSubmit);
}
bool uploadQuarantineOccupiedForTest() noexcept { return uploadQuarantineOccupied(); }
void retireUploadQuarantineForTest() noexcept { retireUploadQuarantine(); }

} // namespace bloom::render::ocio_program_detail
