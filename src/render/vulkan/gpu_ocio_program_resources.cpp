#include "gpu_ocio_program_private.hpp"

#include "ocio_gpu_program_fault.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace bloom::render::ocio_program_detail {

[[nodiscard]] bool reserveUploadQuarantineIfFree() noexcept;
void releaseReservationIfReserved() noexcept;

[[nodiscard]] bool cancellationRequested(const GpuOcioProgramCancellation& cancellation) noexcept {
    if (!cancellation) {
        return false;
    }
    try {
        return cancellation();
    } catch (...) {
        return true;
    }
}

namespace {

std::atomic<std::uint32_t> g_quarantineCount{0};
std::atomic<bool> g_teardownIncomplete{false};
std::atomic<bool> g_faultNotReady{false};
std::atomic<bool> g_faultCancelAfterSubmit{false};

// Explicit single-slot ownership. `Reserved` is held from before the first allocation until either
// the upload retires (released) or the resources move into the slot (`Quarantined`). A foreign
// thread may only observe the state; it never touches the driver or destroys resources.
enum class SlotState : std::uint8_t { Free, Reserved, Quarantined };

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

std::mutex g_slotMutex;
SlotState g_slotState = SlotState::Free;
std::thread::id g_slotOwner{};

// The bounded retention slot is intentionally immortal: non-allocating aligned byte storage plus a
// placement-constructed instance whose pointer has static storage. Its C++ destructor never runs at
// process exit, so an unretired fence/pool/device can never be destroyed from a foreign exit
// thread. Construction happens on first use, which is always before any submission.
alignas(UploadQuarantine) unsigned char g_slotStorage[sizeof(UploadQuarantine)];

[[nodiscard]] UploadQuarantine* quarantineStorage() noexcept {
    static UploadQuarantine* const slot =
        ::new (static_cast<void*>(g_slotStorage)) UploadQuarantine();
    return slot;
}

void setReasonNoThrow(std::string& reason, const char* text) noexcept {
    try {
        reason = text;
    } catch (...) {
        // A diagnostic must never unwind through an ownership transition; drop any partial text
        // rather than leave the reason in an indeterminate state.
        reason.clear();
    }
}

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

// Caller holds g_slotMutex. Only the slot owner may query the driver.
[[nodiscard]] bool tryRetireQuarantinedLocked() noexcept {
    if (g_slotState != SlotState::Quarantined || g_slotOwner != std::this_thread::get_id() ||
        quarantineStorage()->state == nullptr) {
        return g_slotState == SlotState::Free;
    }
    const VkFence rawFence = static_cast<VkFence>(*quarantineStorage()->fence);
    const VkResult status = quarantineStorage()->state->device.getDispatcher()->vkGetFenceStatus(
        static_cast<VkDevice>(*quarantineStorage()->state->device), rawFence);
    if (status != VK_SUCCESS && status != VK_ERROR_DEVICE_LOST) {
        return false;
    }
    releaseQuarantineResources(*quarantineStorage());
    g_slotState = SlotState::Free;
    g_slotOwner = std::thread::id{};
    return true;
}

// No throwing operations after submission: this only moves already-owned resources and flips the
// reservation state. Diagnostics are assigned separately by a noexcept helper.
void quarantineUpload(ImageOwner& image, Staging& staging, vk::raii::CommandPool& pool,
                      vk::raii::CommandBuffer& command, vk::raii::Fence& fence,
                      std::shared_ptr<vulkan_detail::DeviceAllocatorState> state,
                      const bool latchFuse) noexcept {
    quarantineStorage()->state = std::move(state);
    quarantineStorage()->image = image.image;
    quarantineStorage()->imageAllocation = image.allocation;
    image.release();
    quarantineStorage()->staging = staging.buffer;
    quarantineStorage()->stagingAllocation = staging.allocation;
    staging.release();
    quarantineStorage()->pool = std::move(pool);
    quarantineStorage()->command = std::move(command);
    quarantineStorage()->fence = std::move(fence);
    {
        const std::lock_guard<std::mutex> lock(g_slotMutex);
        if (g_slotState == SlotState::Reserved && g_slotOwner == std::this_thread::get_id()) {
            g_slotState = SlotState::Quarantined;
        }
    }
    if (latchFuse) {
        noteQuarantine();
    }
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

// Checked sum of the actual image allocation, staging allocation, and padded host scratch against
// the caller's remaining owned budget. Never wraps.
[[nodiscard]] bool fitsRemainingBudget(const std::uint64_t imageBytes,
                                       const std::uint64_t stagingBytes,
                                       const std::uint64_t scratchBytes,
                                       const std::uint64_t remaining) noexcept {
    std::uint64_t total = imageBytes;
    if (total > std::numeric_limits<std::uint64_t>::max() - stagingBytes) {
        return false;
    }
    total += stagingBytes;
    if (total > std::numeric_limits<std::uint64_t>::max() - scratchBytes) {
        return false;
    }
    total += scratchBytes;
    return total <= remaining;
}

[[nodiscard]] UploadOutcome
uploadImage(vulkan_detail::DeviceAllocatorState& state,
            const std::shared_ptr<vulkan_detail::DeviceAllocatorState>& owner, ImageOwner& image,
            const VkExtent3D extent, const void* data, const std::uint64_t bytes,
            const std::uint64_t imageAllocationBytes, const std::uint64_t remainingOwnedBytes,
            const std::uint64_t hostScratchBytes, const GpuOcioProgramCancellation& cancellation,
            std::string& reason) {
    const VkDevice device = static_cast<VkDevice>(*state.device);
    const auto* dispatcher = state.device.getDispatcher();
    if (cancellationRequested(cancellation)) {
        setReasonNoThrow(reason, "the LUT upload was cancelled before submission");
        return UploadOutcome::Cancelled;
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
        setReasonNoThrow(reason, "the LUT staging buffer could not be created");
        return UploadOutcome::Failed;
    }
    std::memcpy(mappedInfo.pMappedData, data, static_cast<std::size_t>(bytes));
    // Non-coherent host memory: the CPU writes must be flushed before the transfer can read them.
    if (vmaFlushAllocation(state.allocator, staging.allocation, 0,
                           static_cast<VkDeviceSize>(bytes)) != VK_SUCCESS) {
        setReasonNoThrow(reason, "the LUT staging buffer could not be flushed");
        return UploadOutcome::Failed;
    }

    const std::uint64_t stagingBytes = static_cast<std::uint64_t>(mappedInfo.size);
    if (!fitsRemainingBudget(imageAllocationBytes, stagingBytes, hostScratchBytes,
                             remainingOwnedBytes)) {
        setReasonNoThrow(reason, "the LUT upload exceeds the remaining owned-byte budget");
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
            setReasonNoThrow(reason, "the LUT upload command pool could not be created");
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
            setReasonNoThrow(reason, "the LUT upload command buffer could not be allocated");
            return UploadOutcome::Failed;
        }
        command = vk::raii::CommandBuffer(state.device, rawCommand, rawPool);
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence rawFence = VK_NULL_HANDLE;
        if (dispatcher->vkCreateFence(device, &fenceInfo, nullptr, &rawFence) != VK_SUCCESS) {
            setReasonNoThrow(reason, "the LUT upload fence could not be created");
            return UploadOutcome::Failed;
        }
        fence = vk::raii::Fence(state.device, rawFence);
    }
    const VkCommandBuffer raw = static_cast<VkCommandBuffer>(*command);
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (dispatcher->vkBeginCommandBuffer(raw, &beginInfo) != VK_SUCCESS) {
        setReasonNoThrow(reason, "the LUT upload command buffer could not begin");
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
        setReasonNoThrow(reason, "the LUT upload command buffer could not end");
        return UploadOutcome::Failed;
    }
    if (cancellationRequested(cancellation)) {
        setReasonNoThrow(reason, "the LUT upload was cancelled before submission");
        return UploadOutcome::Cancelled;
    }
    const VkFence rawFence = static_cast<VkFence>(*fence);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &raw;
    if (dispatcher->vkQueueSubmit(static_cast<VkQueue>(*state.computeQueue), 1, &submit,
                                  rawFence) != VK_SUCCESS) {
        setReasonNoThrow(reason, "the LUT upload submission failed");
        return UploadOutcome::Failed;
    }
    // Submission is outstanding. No owned resource may be destroyed on an unproven outcome, and no
    // throwing operation runs until ownership state is authoritative.
    if (g_faultNotReady.load()) {
        quarantineUpload(image, staging, pool, command, fence, owner, true);
        setReasonNoThrow(reason, "the LUT upload completion is unproven (fault-injected)");
        return UploadOutcome::Quarantined;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    VkResult waited = VK_NOT_READY;
    while (true) {
        if (g_faultCancelAfterSubmit.load() || cancellationRequested(cancellation)) {
            quarantineUpload(image, staging, pool, command, fence, owner, false);
            setReasonNoThrow(reason, "the LUT upload was cancelled after submission");
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
    quarantineUpload(image, staging, pool, command, fence, owner, true);
    setReasonNoThrow(reason, waited == VK_ERROR_DEVICE_LOST
                                 ? "the device was lost during LUT upload"
                                 : "the LUT upload completion is unproven");
    return UploadOutcome::Quarantined;
}

} // namespace

bool reserveUploadQuarantineIfFree() noexcept {
    try {
        const std::lock_guard<std::mutex> lock(g_slotMutex);
        if (g_slotState == SlotState::Quarantined && g_slotOwner == std::this_thread::get_id()) {
            static_cast<void>(tryRetireQuarantinedLocked());
        }
        if (g_slotState != SlotState::Free) {
            return false;
        }
        g_slotState = SlotState::Reserved;
        g_slotOwner = std::this_thread::get_id();
        return true;
    } catch (...) {
        return false;
    }
}

void releaseReservationIfReserved() noexcept {
    try {
        const std::lock_guard<std::mutex> lock(g_slotMutex);
        if (g_slotState == SlotState::Reserved && g_slotOwner == std::this_thread::get_id()) {
            g_slotState = SlotState::Free;
            g_slotOwner = std::thread::id{};
        }
    } catch (...) { // NOLINT(bugprone-empty-catch)
        // The mutex acquisition failed; leave the reservation fail-closed rather than terminating a
        // noexcept ownership transition. There is no reporting channel here.
    }
}

UploadOutcome
createSampledResource(const std::shared_ptr<vulkan_detail::DeviceAllocatorState>& owner,
                      const OcioGpuTextureDesc& texture, const std::uint64_t remainingOwnedBytes,
                      const GpuOcioProgramCancellation& cancellation,
                      SampledResource& out) noexcept {
    if (cancellationRequested(cancellation)) {
        return UploadOutcome::Cancelled;
    }
    if (!reserveUploadQuarantineIfFree()) {
        // A retention slot must exist before we allocate or submit anything; refuse typed.
        return UploadOutcome::Failed;
    }
    struct ReservationGuard final {
        ~ReservationGuard() { releaseReservationIfReserved(); }
    } reservationGuard;
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
            format = VK_FORMAT_R32G32B32A32_SFLOAT;
            components = 4;
            if (!formatSupports(state, format, linear)) {
                return UploadOutcome::Failed;
            }
        }
        const VkExtent3D extent = textureExtent(texture);
        VkImageCreateInfo imageInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .imageType = imageType(texture.dimensions),
            .format = format,
            .extent = extent,
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
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
        const std::uint64_t hostScratchBytes = padded.empty() ? 0U : padded.size() * sizeof(float);
        std::string reason;
        const UploadOutcome outcome = uploadImage(
            state, owner, image, extent, source, bytes, static_cast<std::uint64_t>(mapped.size),
            remainingOwnedBytes, hostScratchBytes, cancellation, reason);
        if (outcome != UploadOutcome::Retired) {
            // Quarantined/Cancelled keep the reservation (resources retained); OverBudget/Failed
            // release it via the guard. Typed outcomes are preserved for the caller.
            return outcome;
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

bool uploadQuarantineOccupied() noexcept {
    try {
        const std::lock_guard<std::mutex> lock(g_slotMutex);
        return g_slotState != SlotState::Free;
    } catch (...) {
        return true;
    }
}

void retireUploadQuarantine() noexcept {
    try {
        const std::lock_guard<std::mutex> lock(g_slotMutex);
        if (g_slotState != SlotState::Quarantined || g_slotOwner != std::this_thread::get_id()) {
            return;
        }
        static_cast<void>(tryRetireQuarantinedLocked());
    } catch (...) { // NOLINT(bugprone-empty-catch)
        // The mutex acquisition failed; the quarantine stays latched rather than terminating this
        // noexcept owner-thread retirement. There is no reporting channel here.
    }
}

std::atomic<bool>& ocioFaultNotReady() noexcept { return g_faultNotReady; }
std::atomic<bool>& ocioFaultCancelAfterSubmit() noexcept { return g_faultCancelAfterSubmit; }

} // namespace bloom::render::ocio_program_detail
