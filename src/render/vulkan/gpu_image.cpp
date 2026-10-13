#include "gpu_image_private.hpp"
#include "gpu_image_reclamation_private.hpp"
#include "gpu_native_image_allocation_private.hpp"
#include "gpu_oracle_readback_private.hpp"

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
    // referenced by an unretired submission. Its pre-reserved owner slot retains the entire
    // impl and allocator generation, with its native allocation bytes still charged.
    if (!impl_->onOwnerThread() || impl_->submissionUnretired.load(std::memory_order_acquire)) {
        image_detail::orphanImage(*impl_);
        static_cast<void>(impl_.release()); // NOLINT(bugprone-unused-return-value)
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

std::uint64_t GpuImage::allocationBytes() const noexcept {
    if (impl_ == nullptr || impl_->allocation == VK_NULL_HANDLE || impl_->state == nullptr) {
        return 0;
    }
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(impl_->state->allocator, impl_->allocation, &info);
    return static_cast<std::uint64_t>(info.size);
}

bool GpuImage::isBoundTo(GpuDevice& device) const noexcept {
    if (impl_ == nullptr || !impl_->onOwnerThread()) {
        return false;
    }
    const auto deviceState = GpuRendererAccess::state(device);
    return deviceState != nullptr && deviceState == impl_->state;
}

bool GpuImage::promoteStorageOwnership() const noexcept {
    return impl_ && impl_->onOwnerThread() &&
           !impl_->submissionUnretired.load(std::memory_order_acquire) &&
           !impl_->state->processingPhaseUnproven &&
           (impl_->storageToken.bytes() == 0 || impl_->storageToken.promote());
}

bool retireFailedAttemptGpuImage(std::shared_ptr<const GpuImage>& image,
                                 GpuDevice& device) noexcept {
    if (!image || image.use_count() != 1 || !image->isBoundTo(device))
        return false;
    auto& impl = *image->impl_;
    if (impl.deviceLost || device.state() != GpuDeviceState::Ready ||
        impl.submissionUnretired.load(std::memory_order_acquire) ||
        impl.state->processingPhaseUnproven || !impl.state->processingPhase)
        return false;
    // The unique holder is this failed attempt. Cache or external aliases would have prevented
    // entry above. Keep the token until actual native destruction, then fold only the exact origin.
    auto token = std::move(impl.storageToken);
    impl.destroy();
    static_cast<void>(token.releaseFailedAttemptAfterBackingDestroyed(impl.state->processingPhase));
    image.reset();
    return true;
}

GpuImage makeGpuImage(std::unique_ptr<GpuImageImpl> impl) noexcept {
    return GpuImage(std::move(impl));
}

const GpuImageImpl* gpuImageImpl(const GpuImage& image) noexcept { return image.impl_.get(); }

bool GpuImageImpl::onOwnerThread() const noexcept {
    return state != nullptr && state->owner == std::this_thread::get_id() &&
           state->ownerLifetimeToken != 0 &&
           state->ownerLifetimeToken == vulkan_detail::deviceOwnerLifetimeToken();
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
        if (state->nativeImageBackingsDestroyed != UINT64_MAX)
            ++state->nativeImageBackingsDestroyed;
        image = VK_NULL_HANDLE;
        allocation = VK_NULL_HANDLE;
    }
    storageToken = {};
}

GpuImageImpl::~GpuImageImpl() {
    destroy();
    image_detail::releaseImageSlot(*this);
}

SolidImageSupport querySolidImageSupport(vulkan_detail::DeviceAllocatorState& state,
                                         const std::uint32_t width,
                                         const std::uint32_t height) noexcept {
    SolidImageSupport support;
    if (state.physicalDevice == nullptr) {
        support.reason = "no physical device";
        return support;
    }
    const auto* dispatcher = state.physicalDevice.getDispatcher();
    auto* const physical = static_cast<VkPhysicalDevice>(*state.physicalDevice);

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
                         const std::uint32_t height, GpuImageImpl& out,
                         const bool restoreFailedPrefix) {
    if (!image_detail::reserveImage(out)) {
        image_detail::releaseImageSlot(out);
        return false;
    }
    VkImageCreateInfo imageInfo;
    std::memset(&imageInfo, 0, sizeof(imageInfo));
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

    if (!vulkan_detail::allocateCoveredImage(state, imageInfo, out.image, out.allocation,
                                             out.storageToken, false, restoreFailedPrefix)) {
        image_detail::releaseImageSlot(out);
        return false;
    }
    VmaAllocationInfo nativeInfo{};
    vmaGetAllocationInfo(state.allocator, out.allocation, &nativeInfo);
    image_detail::registerImageBytes(out, static_cast<std::uint64_t>(nativeInfo.size));
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = out.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = kSolidFormat;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    const bool viewFault =
        state.nativeImageAllocationFault == vulkan_detail::NativeImageAllocationFault::View;
    if (viewFault)
        state.nativeImageAllocationFault = vulkan_detail::NativeImageAllocationFault::None;
    if (viewFault ||
        state.device.getDispatcher()->vkCreateImageView(static_cast<VkDevice>(*state.device),
                                                        &viewInfo, nullptr, &view) != VK_SUCCESS) {
        auto token = restoreFailedPrefix ? std::move(out.storageToken) : NativeImageStorageToken{};
        out.destroy();
        if (restoreFailedPrefix)
            static_cast<void>(
                token.releaseFailedAttemptAfterBackingDestroyed(state.processingPhase));
        image_detail::releaseImageSlot(out);
        return false;
    }

    out.view = view;
    out.width = width;
    out.height = height;
    return true;
}

GpuImageReadback readbackResidentImage(const GpuImage& image,
                                       const std::uint64_t byteBudget) noexcept {
    try {
        if (image.impl_ == nullptr || image.impl_->image == VK_NULL_HANDLE) {
            return readbackFailure(GpuImageReadbackCode::DeviceUnavailable, "no resident image");
        }
        GpuImageImpl& impl = *image.impl_;
        if (!impl.onOwnerThread()) {
            return readbackFailure(GpuImageReadbackCode::WrongThread,
                                   "readback must run on the device owner thread");
        }
        oracle_detail::drain(*impl.state);
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

        oracle_detail::Reservation reservation(impl);
        if (!reservation.valid()) {
            return readbackFailure(GpuImageReadbackCode::DeviceUnavailable,
                                   "oracle readback admission is occupied or closed");
        }

        auto& state = *impl.state;
        const VkDevice device = static_cast<VkDevice>(*state.device);
        const auto* dispatcher = state.device.getDispatcher();

        oracle_detail::StagingBuffer staging;
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
        reservation.charge(static_cast<std::uint64_t>(stagingInfo.size));
        if (stagingInfo.size > byteBudget) {
            return readbackFailure(GpuImageReadbackCode::OverBudget,
                                   "the actual oracle staging allocation exceeds the byte budget");
        }

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

        // Make every copied destination byte visible to the host before fence retirement
        // and the subsequent allocation-relative invalidate.
        VkBufferMemoryBarrier toHost{};
        toHost.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toHost.buffer = staging.buffer;
        toHost.size = bytes;
        dispatcher->vkCmdPipelineBarrier(rawCommandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &toHost, 0,
                                         nullptr);

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
        const VkResult waited = oracle_detail::wait(state, rawFence, kReadbackDeadlineNanoseconds);
        if (waited != VK_SUCCESS && waited != VK_ERROR_DEVICE_LOST) {
            reservation.retain(std::move(staging), std::move(pool), std::move(commandBuffer),
                               std::move(fence));
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
