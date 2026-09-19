#include "gpu_image_private.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace bloom::render {
namespace {

using vulkan_detail::DeviceAllocatorState;

constexpr VkFormat kSolidFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
constexpr std::uint64_t kReadbackDeadlineNanoseconds = 5ULL * 1000ULL * 1000ULL * 1000ULL;

[[nodiscard]] GpuImageReadback readbackFailure(const GpuImageReadbackCode code,
                                               const std::string_view message) noexcept {
    GpuImageReadback result;
    result.code = code;
    try {
        result.message.assign(message);
    } catch (...) {
        result.message.clear();
    }
    return result;
}

struct StagingBuffer final {
    StagingBuffer() = default;
    StagingBuffer(const StagingBuffer&) = delete;
    StagingBuffer& operator=(const StagingBuffer&) = delete;
    StagingBuffer(StagingBuffer&& other) noexcept { *this = std::move(other); }
    StagingBuffer& operator=(StagingBuffer&& other) noexcept {
        if (this != &other) {
            release();
            state = other.state;
            buffer = other.buffer;
            allocation = other.allocation;
            armed = other.armed;
            other.state = nullptr;
            other.buffer = VK_NULL_HANDLE;
            other.allocation = VK_NULL_HANDLE;
            other.armed = false;
        }
        return *this;
    }
    ~StagingBuffer() { release(); }

    void release() noexcept {
        if (armed && state != nullptr) {
            vmaDestroyBuffer(state->allocator, buffer, allocation);
        }
        state = nullptr;
        buffer = VK_NULL_HANDLE;
        allocation = VK_NULL_HANDLE;
        armed = false;
    }

    DeviceAllocatorState* state = nullptr;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    bool armed = false;
};

// One bounded quarantine slot for a readback whose submission could not be
// proved retired. It retains the entire submitted resource set, the source image
// (via GpuImageImpl::submissionUnretired), and the device generation. Allocated
// once on first use and never destroyed, so a possibly in-flight submission is
// never torn down at process exit. No allocation happens on the failure path.
struct ReadbackQuarantine final {
    std::shared_ptr<DeviceAllocatorState> state;
    StagingBuffer staging;
    vk::raii::CommandPool pool{nullptr};
    vk::raii::CommandBuffer buffer{nullptr};
    vk::raii::Fence fence{nullptr};
    bool occupied = false;
};

ReadbackQuarantine& readbackQuarantine() {
    static auto* const slot = new ReadbackQuarantine();
    return *slot;
}

std::atomic<bool>& readbackFuse() {
    static std::atomic<bool> fuse{false};
    return fuse;
}

// Retires the quarantined readback only when its fence is known signalled (or
// the device is lost). Returns true when the slot is free afterwards.
bool tryRetireQuarantine(ReadbackQuarantine& slot) noexcept {
    if (!slot.occupied) {
        return true;
    }
    if (slot.state == nullptr) {
        slot.occupied = false;
        return true;
    }
    const VkFence rawFence = static_cast<VkFence>(*slot.fence);
    const VkResult status = slot.state->device.getDispatcher()->vkGetFenceStatus(
        static_cast<VkDevice>(*slot.state->device), rawFence);
    if (status != VK_SUCCESS && status != VK_ERROR_DEVICE_LOST) {
        return false;
    }
    slot.staging.release();
    slot.fence = vk::raii::Fence{nullptr};
    slot.buffer = vk::raii::CommandBuffer{nullptr};
    slot.pool = vk::raii::CommandPool{nullptr};
    slot.state.reset();
    slot.occupied = false;
    return true;
}

} // namespace

GpuImage::GpuImage(std::unique_ptr<GpuImageImpl> impl) noexcept : impl_(std::move(impl)) {}

GpuImage::GpuImage(GpuImage&& other) noexcept : impl_(std::move(other.impl_)) {}

GpuImage& GpuImage::operator=(GpuImage&& other) noexcept {
    if (this != &other) {
        releaseOwnedImpl();
        impl_ = std::move(other.impl_);
    }
    return *this;
}

GpuImage::~GpuImage() { releaseOwnedImpl(); }

void GpuImage::releaseOwnedImpl() noexcept {
    if (impl_ == nullptr) {
        return;
    }
    // Never make Vulkan calls from a foreign thread, and never destroy an image
    // referenced by an unretired submission. Retain the whole impl instead
    // (bounded by the process fuse) so native ownership stays safe.
    if (impl_->submissionUnretired || !impl_->onOwnerThread()) {
        [[maybe_unused]] const auto* const retained = impl_.release();
        return;
    }
    impl_.reset();
}

bool GpuImage::isValid() const noexcept {
    return impl_ != nullptr && impl_->image != VK_NULL_HANDLE && !impl_->deviceLost;
}
std::uint32_t GpuImage::width() const noexcept { return impl_ != nullptr ? impl_->width : 0; }
std::uint32_t GpuImage::height() const noexcept { return impl_ != nullptr ? impl_->height : 0; }
std::optional<ImageWindow> GpuImage::dataWindow() const noexcept {
    if (impl_ == nullptr) {
        return std::nullopt;
    }
    return impl_->dataWindow;
}
std::optional<ImageWindow> GpuImage::displayWindow() const noexcept {
    if (impl_ == nullptr) {
        return std::nullopt;
    }
    return impl_->displayWindow;
}
core::PixelAspectRatio GpuImage::pixelAspect() const noexcept {
    return impl_ != nullptr ? impl_->pixelAspect : core::PixelAspectRatio::square();
}
std::uint32_t GpuImage::generation() const noexcept {
    return impl_ != nullptr ? impl_->generation : 0;
}

bool GpuImage::isBoundTo(GpuDevice& device) const noexcept {
    if (impl_ == nullptr || !impl_->onOwnerThread()) {
        return false;
    }
    const auto deviceState = GpuRendererAccess::state(device);
    return deviceState != nullptr && deviceState == impl_->state;
}

GpuImage makeGpuImage(std::unique_ptr<GpuImageImpl> impl) noexcept {
    return GpuImage(std::move(impl));
}

const GpuImageImpl* gpuImageImpl(const GpuImage& image) noexcept { return image.impl_.get(); }

bool GpuImageImpl::onOwnerThread() const noexcept {
    return state != nullptr && state->owner == std::this_thread::get_id();
}

void GpuImageImpl::destroy() noexcept {
    if (state == nullptr) {
        return;
    }
    if (view != VK_NULL_HANDLE) {
        state->device.getDispatcher()->vkDestroyImageView(static_cast<VkDevice>(*state->device),
                                                          view, nullptr);
        view = VK_NULL_HANDLE;
    }
    if (image != VK_NULL_HANDLE) {
        vmaDestroyImage(state->allocator, image, allocation);
        image = VK_NULL_HANDLE;
        allocation = VK_NULL_HANDLE;
    }
}

GpuImageImpl::~GpuImageImpl() { destroy(); }

SolidImageSupport querySolidImageSupport(vulkan_detail::DeviceAllocatorState& state,
                                         const std::uint32_t width,
                                         const std::uint32_t height) noexcept {
    SolidImageSupport support;
    if (state.physicalDevice == nullptr) {
        support.reason = "no physical device";
        return support;
    }
    const auto* dispatcher = state.physicalDevice.getDispatcher();
    const auto physical = static_cast<VkPhysicalDevice>(*state.physicalDevice);

    VkPhysicalDeviceProperties properties{};
    dispatcher->vkGetPhysicalDeviceProperties(physical, &properties);
    if (width == 0 || height == 0 || width > properties.limits.maxImageDimension2D ||
        height > properties.limits.maxImageDimension2D) {
        support.reason = "extent exceeds maxImageDimension2D";
        return support;
    }

    VkFormatProperties formatProperties{};
    dispatcher->vkGetPhysicalDeviceFormatProperties(physical, kSolidFormat, &formatProperties);
    const VkFormatFeatureFlags required =
        VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if ((formatProperties.optimalTilingFeatures & required) != required) {
        support.reason = "RGBA32F storage/sampled image is not supported";
        return support;
    }

    const VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImageFormatProperties imageProperties{};
    if (dispatcher->vkGetPhysicalDeviceImageFormatProperties(
            physical, kSolidFormat, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, 0,
            &imageProperties) != VK_SUCCESS) {
        support.reason = "image format/usage combination is unsupported";
        return support;
    }
    if (width > imageProperties.maxExtent.width || height > imageProperties.maxExtent.height) {
        support.reason = "extent exceeds image format maxExtent";
        return support;
    }
    const std::uint64_t requiredBytes =
        static_cast<std::uint64_t>(width) * height * sizeof(Rgba32f);
    if (requiredBytes > imageProperties.maxResourceSize) {
        support.reason = "image exceeds maxResourceSize";
        return support;
    }
    support.supported = true;
    support.maxImageBytes = imageProperties.maxResourceSize;
    return support;
}

bool createResidentImage(vulkan_detail::DeviceAllocatorState& state, const std::uint32_t width,
                         const std::uint32_t height, GpuImageImpl& out) {
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = kSolidFormat;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    if (vmaCreateImage(state.allocator, &imageInfo, &allocationInfo, &image, &allocation,
                       nullptr) != VK_SUCCESS) {
        return false;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = kSolidFormat;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    if (state.device.getDispatcher()->vkCreateImageView(static_cast<VkDevice>(*state.device),
                                                        &viewInfo, nullptr, &view) != VK_SUCCESS) {
        vmaDestroyImage(state.allocator, image, allocation);
        return false;
    }

    out.image = image;
    out.allocation = allocation;
    out.view = view;
    out.width = width;
    out.height = height;
    return true;
}

GpuImageReadback readbackResidentImage(const GpuImage& image,
                                       const std::uint64_t byteBudget) noexcept {
    if (readbackFuse().load()) {
        return readbackFailure(GpuImageReadbackCode::DeviceUnavailable,
                               "a prior readback could not be retired; the device is quarantined");
    }
    try {
        if (image.impl_ == nullptr || image.impl_->image == VK_NULL_HANDLE) {
            return readbackFailure(GpuImageReadbackCode::DeviceUnavailable, "no resident image");
        }
        GpuImageImpl& impl = *image.impl_;
        if (!impl.onOwnerThread()) {
            return readbackFailure(GpuImageReadbackCode::WrongThread,
                                   "readback must run on the device owner thread");
        }
        if (impl.deviceLost) {
            return readbackFailure(GpuImageReadbackCode::DeviceLost, "the device was lost");
        }
        if (impl.submissionUnretired) {
            return readbackFailure(GpuImageReadbackCode::DeviceUnavailable,
                                   "a prior readback submission is not retired");
        }
        const std::uint64_t bytes =
            static_cast<std::uint64_t>(impl.width) * impl.height * sizeof(Rgba32f);
        if (bytes == 0 || bytes > byteBudget) {
            return readbackFailure(GpuImageReadbackCode::OverBudget,
                                   "the readback exceeds the requested byte budget");
        }

        ReadbackQuarantine& slot = readbackQuarantine();
        if (!tryRetireQuarantine(slot)) {
            return readbackFailure(
                GpuImageReadbackCode::DeviceUnavailable,
                "a prior readback submission is still unretired and was retained");
        }

        auto& state = *impl.state;
        const VkDevice device = static_cast<VkDevice>(*state.device);
        const auto* dispatcher = state.device.getDispatcher();

        StagingBuffer staging;
        staging.state = &state;
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = bytes;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo stagingAllocation{};
        stagingAllocation.usage = VMA_MEMORY_USAGE_AUTO;
        stagingAllocation.flags =
            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo stagingInfo{};
        if (vmaCreateBuffer(state.allocator, &bufferInfo, &stagingAllocation, &staging.buffer,
                            &staging.allocation, &stagingInfo) != VK_SUCCESS) {
            return readbackFailure(GpuImageReadbackCode::ReadbackFailed,
                                   "the readback staging buffer could not be created");
        }
        staging.armed = true;

        vk::raii::CommandPool pool{nullptr};
        {
            VkCommandPoolCreateInfo poolInfo{};
            poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            poolInfo.queueFamilyIndex = state.computeQueueFamily;
            VkCommandPool raw = VK_NULL_HANDLE;
            if (dispatcher->vkCreateCommandPool(device, &poolInfo, nullptr, &raw) != VK_SUCCESS) {
                return readbackFailure(GpuImageReadbackCode::ReadbackFailed,
                                       "the readback command pool could not be created");
            }
            pool = vk::raii::CommandPool(state.device, raw);
        }
        vk::raii::CommandBuffer commandBuffer{nullptr};
        {
            VkCommandBufferAllocateInfo allocateInfo{};
            allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocateInfo.commandPool = *pool;
            allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocateInfo.commandBufferCount = 1;
            VkCommandBuffer raw = VK_NULL_HANDLE;
            if (dispatcher->vkAllocateCommandBuffers(device, &allocateInfo, &raw) != VK_SUCCESS) {
                return readbackFailure(GpuImageReadbackCode::ReadbackFailed,
                                       "the readback command buffer could not be allocated");
            }
            commandBuffer = vk::raii::CommandBuffer(state.device, raw, *pool);
        }
        vk::raii::Fence fence{nullptr};
        {
            VkFenceCreateInfo fenceInfo{};
            fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            VkFence raw = VK_NULL_HANDLE;
            if (dispatcher->vkCreateFence(device, &fenceInfo, nullptr, &raw) != VK_SUCCESS) {
                return readbackFailure(GpuImageReadbackCode::ReadbackFailed,
                                       "the readback fence could not be created");
            }
            fence = vk::raii::Fence(state.device, raw);
        }

        const VkCommandBuffer rawCommandBuffer = static_cast<VkCommandBuffer>(*commandBuffer);
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (dispatcher->vkBeginCommandBuffer(rawCommandBuffer, &beginInfo) != VK_SUCCESS) {
            return readbackFailure(GpuImageReadbackCode::ReadbackFailed,
                                   "the readback command buffer could not begin");
        }

        VkImageMemoryBarrier toTransfer{};
        toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toTransfer.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toTransfer.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = impl.image;
        toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        dispatcher->vkCmdPipelineBarrier(rawCommandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                                         1, &toTransfer);

        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {impl.width, impl.height, 1};
        dispatcher->vkCmdCopyImageToBuffer(rawCommandBuffer, impl.image,
                                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.buffer, 1,
                                           &copy);

        VkImageMemoryBarrier toGeneral = toTransfer;
        toGeneral.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toGeneral.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        toGeneral.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        dispatcher->vkCmdPipelineBarrier(rawCommandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                                         nullptr, 1, &toGeneral);

        if (dispatcher->vkEndCommandBuffer(rawCommandBuffer) != VK_SUCCESS) {
            return readbackFailure(GpuImageReadbackCode::ReadbackFailed,
                                   "the readback command buffer could not end");
        }

        const VkFence rawFence = static_cast<VkFence>(*fence);
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &rawCommandBuffer;
        const VkResult submitted = dispatcher->vkQueueSubmit(
            static_cast<VkQueue>(*state.computeQueue), 1, &submit, rawFence);
        if (submitted == VK_ERROR_DEVICE_LOST) {
            impl.deviceLost = true;
            return readbackFailure(GpuImageReadbackCode::DeviceLost,
                                   "the device was lost during the readback submit");
        }
        if (submitted != VK_SUCCESS) {
            return readbackFailure(GpuImageReadbackCode::ReadbackFailed,
                                   "the readback submission failed");
        }

        // From here the queue owns staging, command buffer, and fence. Retire only on
        // a known completion; on an unknown result retain the whole set and the image.
        const VkResult waited = dispatcher->vkWaitForFences(device, 1, &rawFence, VK_TRUE,
                                                            kReadbackDeadlineNanoseconds);
        if (waited != VK_SUCCESS && waited != VK_ERROR_DEVICE_LOST) {
            slot.state = impl.state;
            slot.staging = std::move(staging);
            slot.pool = std::move(pool);
            slot.buffer = std::move(commandBuffer);
            slot.fence = std::move(fence);
            slot.occupied = true;
            impl.submissionUnretired = true;
            readbackFuse().store(true);
            return readbackFailure(
                GpuImageReadbackCode::ReadbackFailed,
                "the readback completion is unknown; the submission was retained");
        }
        if (waited == VK_ERROR_DEVICE_LOST) {
            impl.deviceLost = true;
            return readbackFailure(GpuImageReadbackCode::DeviceLost,
                                   "the device was lost during the readback wait");
        }
        impl.submissionUnretired = false;
        if (vmaInvalidateAllocation(state.allocator, staging.allocation, 0, bytes) != VK_SUCCESS) {
            return readbackFailure(GpuImageReadbackCode::ReadbackFailed,
                                   "the readback buffer could not be invalidated");
        }
        std::vector<Rgba32f> pixels;
        pixels.resize(static_cast<std::size_t>(impl.width) * impl.height, Rgba32f::transparent());
        std::memcpy(pixels.data(), stagingInfo.pMappedData, static_cast<std::size_t>(bytes));
        return GpuImageReadback{GpuImageReadbackCode::None, {}, std::move(pixels)};
    } catch (const std::bad_alloc&) {
        return readbackFailure(GpuImageReadbackCode::ReadbackFailed,
                               "the readback host buffer could not be allocated");
    } catch (...) {
        return readbackFailure(GpuImageReadbackCode::ReadbackFailed,
                               "the readback failed unexpectedly");
    }
}

} // namespace bloom::render
