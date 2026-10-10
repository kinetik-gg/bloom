#include "gpu_image_reclamation_private.hpp"
#include "gpu_native_image_allocation_private.hpp"
#include "gpu_resident_display_private.hpp"
#include "gpu_resident_display_readback_private.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace bloom::render {
namespace {

using vulkan_detail::DeviceAllocatorState;

constexpr VkFormat kDisplayFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr std::uint64_t kReadbackDeadlineNanoseconds = 5ULL * 1000ULL * 1000ULL * 1000ULL;

[[nodiscard]] GpuDisplayImageReadback
displayReadbackFailure(const GpuDisplayImageReadbackCode code,
                       const std::string_view message) noexcept {
    GpuDisplayImageReadback result;
    result.code = code;
    try {
        result.message.assign(message);
    } catch (...) {
        result.message.clear();
    }
    return result;
}

} // namespace

bool createResidentBuffer(DeviceAllocatorState& state, const std::uint64_t bytes,
                          const VkBufferUsageFlags usage, const VmaAllocationCreateFlags flags,
                          const bool hostVisible, ResidentBuffer& out) {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = bytes;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocationInfo.flags = flags;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(state.allocator, &bufferInfo, &allocationInfo, &buffer, &allocation,
                        &info) != VK_SUCCESS) {
        return false;
    }
    out.buffer = buffer;
    out.allocation = allocation;
    out.info = info;
    out.bytes = bytes;
    out.hostVisible = hostVisible;
    return true;
}

void destroyResidentBuffer(DeviceAllocatorState& state, ResidentBuffer& buffer) noexcept {
    if (buffer.buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(state.allocator, buffer.buffer, buffer.allocation);
    }
    buffer = ResidentBuffer{};
}

ResidentDisplaySupport queryResidentDisplaySupport(DeviceAllocatorState& state,
                                                   const std::uint32_t width,
                                                   const std::uint32_t height) noexcept {
    ResidentDisplaySupport support;
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
    const std::uint64_t inputBytes = static_cast<std::uint64_t>(width) * height * sizeof(Rgba32f);
    const std::uint64_t packedBytes = static_cast<std::uint64_t>(width) * height * sizeof(Rgba8);
    if (inputBytes > properties.limits.maxStorageBufferRange ||
        packedBytes > properties.limits.maxStorageBufferRange) {
        support.reason = "extent exceeds maxStorageBufferRange";
        return support;
    }
    // The neutral display dispatch is one-dimensional over pixelCount (the shader reads only
    // gl_GlobalInvocationID.x). The product must fit uint32 and the real 1D group count, not
    // the width, must fit the physical limit.
    const std::uint64_t pixelCount =
        static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
    if (pixelCount > std::numeric_limits<std::uint32_t>::max()) {
        support.reason = "the pixel count exceeds the uint32 push-constant range";
        return support;
    }
    const std::uint64_t groupCount = (pixelCount + 255U) / 256U;
    if (groupCount > properties.limits.maxComputeWorkGroupCount[0] ||
        properties.limits.maxComputeWorkGroupSize[0] < 256U ||
        properties.limits.maxComputeWorkGroupInvocations < 256U) {
        support.reason = "dispatch exceeds compute workgroup limits";
        return support;
    }

    VkFormatProperties formatProperties{};
    dispatcher->vkGetPhysicalDeviceFormatProperties(physical, kDisplayFormat, &formatProperties);
    const VkFormatFeatureFlags required =
        VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if ((formatProperties.optimalTilingFeatures & required) != required) {
        support.reason = "RGBA8 UNORM output image is not supported";
        return support;
    }
    support.supported = true;
    support.maxOwnedBytes = properties.limits.maxStorageBufferRange;
    return support;
}

bool createDisplayImage(DeviceAllocatorState& state, const std::uint32_t width,
                        const std::uint32_t height, GpuDisplayImageImpl& out) {
    if (!image_detail::reserveImage(out)) {
        image_detail::releaseImageSlot(out);
        return false;
    }
    VkImageCreateInfo imageInfo;
    std::memset(&imageInfo, 0, sizeof(imageInfo));
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = kDisplayFormat;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!vulkan_detail::allocateCoveredImage(state, imageInfo, out.image, out.allocation,
                                             out.storageToken)) {
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
    viewInfo.format = kDisplayFormat;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    const bool viewFault =
        state.nativeImageAllocationFault == vulkan_detail::NativeImageAllocationFault::View;
    if (viewFault)
        state.nativeImageAllocationFault = vulkan_detail::NativeImageAllocationFault::None;
    if (viewFault ||
        state.device.getDispatcher()->vkCreateImageView(static_cast<VkDevice>(*state.device),
                                                        &viewInfo, nullptr, &view) != VK_SUCCESS) {
        out.destroy();
        image_detail::releaseImageSlot(out);
        return false;
    }
    out.view = view;
    out.allocationBytes = nativeInfo.size;
    out.width = width;
    out.height = height;
    return true;
}

GpuDisplayImageImpl::~GpuDisplayImageImpl() {
    destroy();
    image_detail::releaseImageSlot(*this);
}

bool GpuDisplayImageImpl::onOwnerThread() const noexcept {
    return state != nullptr && state->owner == std::this_thread::get_id() &&
           state->ownerLifetimeToken != 0 &&
           state->ownerLifetimeToken == vulkan_detail::deviceOwnerLifetimeToken();
}

void GpuDisplayImageImpl::destroy() noexcept {
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

bool GpuDisplayImage::promoteStorageOwnership() const noexcept {
    return impl_ && impl_->onOwnerThread() &&
           !impl_->submissionUnretired.load(std::memory_order_acquire) &&
           !impl_->state->processingPhaseUnproven &&
           (impl_->storageToken.bytes() == 0 || impl_->storageToken.promote());
}

GpuDisplayImage::GpuDisplayImage(std::unique_ptr<GpuDisplayImageImpl> impl) noexcept
    : impl_(std::move(impl)) {}
GpuDisplayImage::GpuDisplayImage(GpuDisplayImage&& other) noexcept
    : impl_(std::move(other.impl_)) {}
GpuDisplayImage& GpuDisplayImage::operator=(GpuDisplayImage&& other) noexcept {
    if (this != &other) {
        releaseOwnedImpl();
        impl_ = std::move(other.impl_);
    }
    return *this;
}
GpuDisplayImage::~GpuDisplayImage() { releaseOwnedImpl(); }

void GpuDisplayImage::releaseOwnedImpl() noexcept {
    if (impl_ == nullptr) {
        return;
    }
    // Never touch Vulkan from a foreign thread and never destroy an image referenced by an
    // unretired readback; its pre-reserved bounded owner slot retains the entire impl instead.
    if (!impl_->onOwnerThread() || impl_->submissionUnretired.load(std::memory_order_acquire)) {
        image_detail::orphanImage(*impl_);
        static_cast<void>(impl_.release()); // NOLINT(bugprone-unused-return-value)
        return;
    }
    impl_.reset();
}

bool GpuDisplayImage::isValid() const noexcept {
    return impl_ != nullptr && impl_->image != VK_NULL_HANDLE && !impl_->deviceLost;
}
std::uint32_t GpuDisplayImage::width() const noexcept {
    return impl_ != nullptr ? impl_->width : 0;
}
std::uint32_t GpuDisplayImage::height() const noexcept {
    return impl_ != nullptr ? impl_->height : 0;
}
std::optional<ImageWindow> GpuDisplayImage::dataWindow() const noexcept {
    return impl_ != nullptr ? impl_->dataWindow : std::nullopt;
}
std::optional<ImageWindow> GpuDisplayImage::displayWindow() const noexcept {
    return impl_ != nullptr ? impl_->displayWindow : std::nullopt;
}
core::PixelAspectRatio GpuDisplayImage::pixelAspect() const noexcept {
    return impl_ != nullptr ? impl_->pixelAspect : core::PixelAspectRatio::square();
}
std::uint32_t GpuDisplayImage::generation() const noexcept {
    return impl_ != nullptr ? impl_->generation : 0;
}
std::uint64_t GpuDisplayImage::allocationBytes() const noexcept {
    return impl_ != nullptr ? impl_->allocationBytes : 0;
}
bool GpuDisplayImage::isBoundTo(GpuDevice& device) const noexcept {
    if (impl_ == nullptr || !impl_->onOwnerThread()) {
        return false;
    }
    const auto deviceState = GpuRendererAccess::state(device);
    return deviceState != nullptr && deviceState == impl_->state;
}
GpuDisplayImage makeGpuDisplayImage(std::unique_ptr<GpuDisplayImageImpl> impl) noexcept {
    return GpuDisplayImage(std::move(impl));
}

const GpuDisplayImageImpl* gpuDisplayImageImpl(const GpuDisplayImage& image) noexcept {
    return image.impl_.get();
}

GpuDisplayImageReadback readbackResidentDisplayImage(const GpuDisplayImage& image,
                                                     const std::uint64_t byteBudget) noexcept {
    try {
        if (image.impl_ == nullptr || image.impl_->image == VK_NULL_HANDLE) {
            return displayReadbackFailure(GpuDisplayImageReadbackCode::DeviceUnavailable,
                                          "no resident display image");
        }
        GpuDisplayImageImpl& impl = *image.impl_;
        if (!impl.onOwnerThread()) {
            return displayReadbackFailure(GpuDisplayImageReadbackCode::WrongThread,
                                          "readback must run on the device owner thread");
        }
        oracle_detail::drain(*impl.state);
        if (impl.deviceLost) {
            return displayReadbackFailure(GpuDisplayImageReadbackCode::DeviceLost,
                                          "the device was lost");
        }
        if (impl.submissionUnretired) {
            return displayReadbackFailure(GpuDisplayImageReadbackCode::DeviceUnavailable,
                                          "a prior display readback is not retired");
        }
        const std::uint64_t bytes =
            static_cast<std::uint64_t>(impl.width) * impl.height * sizeof(Rgba8);
        if (bytes == 0 || bytes > byteBudget) {
            return displayReadbackFailure(GpuDisplayImageReadbackCode::OverBudget,
                                          "the readback exceeds the requested byte budget");
        }
        oracle_detail::Reservation reservation(impl);
        if (!reservation.valid()) {
            return displayReadbackFailure(GpuDisplayImageReadbackCode::DeviceUnavailable,
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
        VmaAllocationCreateInfo stagingAllocation{};
        stagingAllocation.usage = VMA_MEMORY_USAGE_AUTO;
        stagingAllocation.flags =
            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo stagingInfo{};
        if (vmaCreateBuffer(state.allocator, &bufferInfo, &stagingAllocation, &staging.buffer,
                            &staging.allocation, &stagingInfo) != VK_SUCCESS) {
            return displayReadbackFailure(GpuDisplayImageReadbackCode::ReadbackFailed,
                                          "the display staging buffer could not be created");
        }
        staging.armed = true;
        reservation.charge(static_cast<std::uint64_t>(stagingInfo.size));
        if (stagingInfo.size > byteBudget) {
            return displayReadbackFailure(
                GpuDisplayImageReadbackCode::OverBudget,
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
                return displayReadbackFailure(GpuDisplayImageReadbackCode::ReadbackFailed,
                                              "the display command pool could not be created");
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
                return displayReadbackFailure(GpuDisplayImageReadbackCode::ReadbackFailed,
                                              "the display command buffer could not be allocated");
            }
            commandBuffer = vk::raii::CommandBuffer(state.device, raw, *pool);
        }
        vk::raii::Fence fence{nullptr};
        {
            VkFenceCreateInfo fenceInfo{};
            fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            VkFence raw = VK_NULL_HANDLE;
            if (dispatcher->vkCreateFence(device, &fenceInfo, nullptr, &raw) != VK_SUCCESS) {
                return displayReadbackFailure(GpuDisplayImageReadbackCode::ReadbackFailed,
                                              "the display fence could not be created");
            }
            fence = vk::raii::Fence(state.device, raw);
        }

        const VkCommandBuffer rawCommandBuffer = static_cast<VkCommandBuffer>(*commandBuffer);
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (dispatcher->vkBeginCommandBuffer(rawCommandBuffer, &beginInfo) != VK_SUCCESS) {
            return displayReadbackFailure(GpuDisplayImageReadbackCode::ReadbackFailed,
                                          "the display command buffer could not begin");
        }
        VkImageMemoryBarrier toTransfer{};
        toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toTransfer.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toTransfer.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = impl.image;
        toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        dispatcher->vkCmdPipelineBarrier(rawCommandBuffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                                         1, &toTransfer);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {impl.width, impl.height, 1};
        dispatcher->vkCmdCopyImageToBuffer(rawCommandBuffer, impl.image,
                                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.buffer, 1,
                                           &copy);
        VkImageMemoryBarrier toRead = toTransfer;
        toRead.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        dispatcher->vkCmdPipelineBarrier(rawCommandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                                         nullptr, 1, &toRead);
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
            return displayReadbackFailure(GpuDisplayImageReadbackCode::ReadbackFailed,
                                          "the display command buffer could not end");
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
            return displayReadbackFailure(GpuDisplayImageReadbackCode::DeviceLost,
                                          "the device was lost during display submit");
        }
        if (submitted != VK_SUCCESS) {
            return displayReadbackFailure(GpuDisplayImageReadbackCode::ReadbackFailed,
                                          "the display readback submission failed");
        }
        const VkResult waited = oracle_detail::wait(state, rawFence, kReadbackDeadlineNanoseconds);
        if (waited != VK_SUCCESS && waited != VK_ERROR_DEVICE_LOST) {
            reservation.retain(std::move(staging), std::move(pool), std::move(commandBuffer),
                               std::move(fence));
            return displayReadbackFailure(GpuDisplayImageReadbackCode::ReadbackFailed,
                                          "the display readback completion is unknown; retained");
        }
        if (waited == VK_ERROR_DEVICE_LOST) {
            impl.deviceLost = true;
            return displayReadbackFailure(GpuDisplayImageReadbackCode::DeviceLost,
                                          "the device was lost during the display readback");
        }
        impl.submissionUnretired = false;
        if (vmaInvalidateAllocation(state.allocator, staging.allocation, 0, bytes) != VK_SUCCESS) {
            return displayReadbackFailure(GpuDisplayImageReadbackCode::ReadbackFailed,
                                          "the display readback buffer could not be invalidated");
        }
        std::vector<Rgba8> pixels;
        pixels.resize(static_cast<std::size_t>(impl.width) * impl.height, Rgba8{0, 0, 0, 0});
        std::memcpy(pixels.data(), stagingInfo.pMappedData, static_cast<std::size_t>(bytes));
        return GpuDisplayImageReadback{GpuDisplayImageReadbackCode::None, {}, std::move(pixels)};
    } catch (const std::bad_alloc&) {
        return displayReadbackFailure(GpuDisplayImageReadbackCode::ReadbackFailed,
                                      "the display readback host buffer could not be allocated");
    } catch (...) {
        return displayReadbackFailure(GpuDisplayImageReadbackCode::ReadbackFailed,
                                      "the display readback failed unexpectedly");
    }
}

// Current native loss truth survives cancellation and failed-job retirement.
bool GpuResidentDisplay::deviceLost() const noexcept {
    return impl_ != nullptr &&
           (impl_->deviceLost ||
            impl_->jobDiagnostic.code == GpuResidentDisplayDiagnosticCode::DeviceLost);
}

// Folded additive accessor (declared in the public header). It reports the native Impl's
// queueSubmitted flag, the authoritative retirement test: logical job state is not proof because an
// unknown fence result sets Failure without clearing the submission. No field or layout change.
bool GpuResidentDisplay::hasUnretiredSubmission() const noexcept {
    return impl_ != nullptr && impl_->queueSubmitted;
}

} // namespace bloom::render
