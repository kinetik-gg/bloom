#include "gpu_ocio_program_private.hpp"

#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace bloom::render {
namespace {

using vulkan_detail::DeviceAllocatorState;

struct BloomOcioPush final {
    std::uint32_t pixelCount;
    std::uint32_t width;
    std::uint32_t height;
};
static_assert(sizeof(BloomOcioPush) == 12);

[[nodiscard]] GpuOcioProgramDiagnostic makeDiagnostic(const GpuOcioProgramDiagnosticCode code,
                                                      std::string message) noexcept {
    GpuOcioProgramDiagnostic diagnostic;
    diagnostic.code = code;
    try {
        diagnostic.message = std::move(message);
    } catch (...) {
        diagnostic.message.clear();
    }
    return diagnostic;
}

} // namespace

void GpuOcioProgram::Impl::fail(const GpuOcioProgramDiagnosticCode code, std::string message) {
    jobState = GpuOcioProgramJobState::Failure;
    jobDiagnostic = makeDiagnostic(code, std::move(message));
}

void GpuOcioProgram::Impl::releaseTransient() {
    if (control != nullptr && packed.buffer != VK_NULL_HANDLE) {
        destroyResidentBuffer(*control, packed);
    }
    effectOutput.reset();
    displayOutput.reset();
    inputRetained.reset();
    lastJobBytes = 0;
}

void GpuOcioProgram::Impl::clearJob() {
    jobState = GpuOcioProgramJobState::Idle;
    jobPixelCount = 0;
    jobWidth = 0;
    jobHeight = 0;
    discardRequested.store(false);
    jobDiagnostic = {};
    releaseTransient();
}

void GpuOcioProgram::Impl::beginImpl(const bool display,
                                     const std::shared_ptr<const GpuImage>& input,
                                     const std::span<const std::byte> uniformBytes,
                                     const std::uint64_t byteBudget) {
    const GpuImageImpl* const inputImpl = gpuImageImpl(*input);
    if (inputImpl == nullptr || !inputImpl->onOwnerThread() ||
        inputImpl->state.get() != control.get() || inputImpl->width == 0 ||
        inputImpl->height == 0) {
        fail(GpuOcioProgramDiagnosticCode::ForeignInput,
             "the input is not a resident image of this device");
        return;
    }
    const std::uint64_t pixelCount =
        static_cast<std::uint64_t>(inputImpl->width) * inputImpl->height;
    if (pixelCount == 0 || pixelCount > std::numeric_limits<std::uint32_t>::max()) {
        fail(GpuOcioProgramDiagnosticCode::InvalidArgument, "the pixel count is invalid");
        return;
    }
    const std::span<const std::byte> values =
        uniformBytes.empty() ? std::span<const std::byte>(desc.uniformBufferData) : uniformBytes;
    if (values.size() != desc.uniformBufferSize) {
        fail(GpuOcioProgramDiagnosticCode::InvalidArgument,
             "the uniform bytes do not match the declared UBO size");
        return;
    }
    const std::uint64_t transientBytes = display ? pixelCount * 4U : 0U;
    if (transientBytes > byteBudget || transientBytes > budgets.maxOwnedBytes) {
        fail(GpuOcioProgramDiagnosticCode::OverBudget,
             "the request output exceeds the byte budget");
        return;
    }
    auto& state = *control;
    const VkDevice device = static_cast<VkDevice>(*state.device);
    const auto* dispatcher = state.device.getDispatcher();

    if (display) {
        auto imageImpl = std::make_unique<GpuDisplayImageImpl>();
        imageImpl->state = control;
        imageImpl->generation = expectedGeneration;
        // The output has exactly the input's geometry: propagate the data/display window and pixel
        // aspect so a downstream native operation (composite/merge) can consume the resident
        // result.
        imageImpl->dataWindow = inputImpl->dataWindow;
        imageImpl->displayWindow = inputImpl->displayWindow;
        imageImpl->pixelAspect = inputImpl->pixelAspect;
        if (!createDisplayImage(state, inputImpl->width, inputImpl->height, *imageImpl)) {
            fail(GpuOcioProgramDiagnosticCode::AllocationFailed,
                 "the display output image could not be created");
            return;
        }
        if (packed.buffer == VK_NULL_HANDLE &&
            !createResidentBuffer(state, pixelCount * 4U,
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                  0, false, packed)) {
            fail(GpuOcioProgramDiagnosticCode::AllocationFailed,
                 "the packed display buffer could not be created");
            return;
        }
        displayOutput =
            std::make_unique<GpuDisplayImage>(makeGpuDisplayImage(std::move(imageImpl)));
        ocio_program_detail::writeBufferDescriptor(ioSet, state, 1, packed.buffer, packed.bytes);
    } else {
        auto imageImpl = std::make_unique<GpuImageImpl>();
        imageImpl->state = control;
        imageImpl->generation = expectedGeneration;
        // The output has exactly the input's geometry: propagate the data/display window and pixel
        // aspect so a downstream native operation (composite/merge) can consume the resident
        // result.
        imageImpl->dataWindow = inputImpl->dataWindow;
        imageImpl->displayWindow = inputImpl->displayWindow;
        imageImpl->pixelAspect = inputImpl->pixelAspect;
        if (!createResidentImage(state, inputImpl->width, inputImpl->height, *imageImpl)) {
            fail(GpuOcioProgramDiagnosticCode::AllocationFailed,
                 "the effect output image could not be created");
            return;
        }
        ocio_program_detail::writeImageDescriptor(ioSet, state, 1, imageImpl->view);
        effectOutput = std::make_unique<GpuImage>(makeGpuImage(std::move(imageImpl)));
    }
    ocio_program_detail::writeImageDescriptor(ioSet, state, 0, inputImpl->view);
    std::memset(statusMapped, 0, static_cast<std::size_t>(ocio_program_detail::kStatusBytes));
    if (vmaFlushAllocation(state.allocator, status.allocation, 0,
                           static_cast<VkDeviceSize>(ocio_program_detail::kStatusBytes)) !=
        VK_SUCCESS) {
        fail(GpuOcioProgramDiagnosticCode::AllocationFailed,
             "the status buffer could not be flushed before submission");
        return;
    }
    if (desc.uniformBufferSize > 0) {
        std::memcpy(uniformMapped, values.data(), values.size());
        if (vmaFlushAllocation(state.allocator, uniformBuffer.allocation, 0,
                               static_cast<VkDeviceSize>(values.size())) != VK_SUCCESS) {
            fail(GpuOcioProgramDiagnosticCode::AllocationFailed,
                 "the uniform buffer could not be flushed before submission");
            return;
        }
    }

    const VkCommandBuffer raw = static_cast<VkCommandBuffer>(*commandBuffer);
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (dispatcher->vkBeginCommandBuffer(raw, &beginInfo) != VK_SUCCESS) {
        fail(GpuOcioProgramDiagnosticCode::AllocationFailed, "the command buffer could not begin");
        return;
    }

    VkImageMemoryBarrier inputBarrier{};
    inputBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    inputBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    inputBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    inputBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    inputBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    inputBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    inputBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    inputBarrier.image = inputImpl->image;
    inputBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    dispatcher->vkCmdPipelineBarrier(raw, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                                     nullptr, 1, &inputBarrier);
    if (!display) {
        VkImageMemoryBarrier outBarrier{};
        outBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        outBarrier.srcAccessMask = 0;
        outBarrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        outBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        outBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        outBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        outBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        outBarrier.image = gpuImageImpl(*effectOutput)->image;
        outBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        dispatcher->vkCmdPipelineBarrier(raw, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                                         nullptr, 1, &outBarrier);
    }

    const VkPipelineLayout layout = static_cast<VkPipelineLayout>(*pipelineLayout);
    const VkDescriptorSet ocio = static_cast<VkDescriptorSet>(*ocioSet);
    const VkDescriptorSet io = static_cast<VkDescriptorSet>(*ioSet);
    const std::array sets{ocio, io};
    dispatcher->vkCmdBindPipeline(raw, VK_PIPELINE_BIND_POINT_COMPUTE,
                                  static_cast<VkPipeline>(*pipeline));
    dispatcher->vkCmdBindDescriptorSets(raw, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0,
                                        static_cast<std::uint32_t>(sets.size()), sets.data(), 0,
                                        nullptr);
    const BloomOcioPush push{static_cast<std::uint32_t>(pixelCount), inputImpl->width,
                             inputImpl->height};
    dispatcher->vkCmdPushConstants(raw, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                                   &push);
    dispatcher->vkCmdDispatch(
        raw,
        static_cast<std::uint32_t>((pixelCount + ocio_program_detail::kWorkgroupSizeX - 1ULL) /
                                   ocio_program_detail::kWorkgroupSizeX),
        1, 1);

    if (display) {
        VkBufferMemoryBarrier packedBarrier{};
        packedBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        packedBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        packedBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        packedBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        packedBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        packedBarrier.buffer = packed.buffer;
        packedBarrier.offset = 0;
        packedBarrier.size = VK_WHOLE_SIZE;
        dispatcher->vkCmdPipelineBarrier(raw, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1,
                                         &packedBarrier, 0, nullptr);
        const GpuDisplayImageImpl* const outputImpl = gpuDisplayImageImpl(*displayOutput);
        VkImageMemoryBarrier toTransfer{};
        toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toTransfer.srcAccessMask = 0;
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = outputImpl->image;
        toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        dispatcher->vkCmdPipelineBarrier(raw, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                                         1, &toTransfer);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {inputImpl->width, inputImpl->height, 1};
        dispatcher->vkCmdCopyBufferToImage(raw, packed.buffer, outputImpl->image,
                                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        VkImageMemoryBarrier toRead = toTransfer;
        toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        dispatcher->vkCmdPipelineBarrier(raw, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                                         nullptr, 1, &toRead);
    }

    if (dispatcher->vkEndCommandBuffer(raw) != VK_SUCCESS) {
        fail(GpuOcioProgramDiagnosticCode::AllocationFailed, "the command buffer could not end");
        return;
    }
    const VkFence rawFence = static_cast<VkFence>(*fence);
    dispatcher->vkResetFences(device, 1, &rawFence);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &raw;
    if (dispatcher->vkQueueSubmit(static_cast<VkQueue>(*state.computeQueue), 1, &submit,
                                  rawFence) != VK_SUCCESS) {
        fail(GpuOcioProgramDiagnosticCode::DeviceLost, "the dispatch submission failed");
        return;
    }
    queueSubmitted = true;
    jobState = GpuOcioProgramJobState::Pending;
    jobPixelCount = static_cast<std::uint32_t>(pixelCount);
    jobWidth = inputImpl->width;
    jobHeight = inputImpl->height;
    lastJobBytes = transientBytes;
    inputRetained = input;
}

bool GpuOcioProgram::Impl::checkStatus() {
    if (vmaInvalidateAllocation(control->allocator, status.allocation, 0,
                                static_cast<VkDeviceSize>(ocio_program_detail::kStatusBytes)) !=
        VK_SUCCESS) {
        jobDiagnostic = makeDiagnostic(GpuOcioProgramDiagnosticCode::DeviceLost,
                                       "the status buffer could not be invalidated; refusing to "
                                       "publish GPU output");
        return false;
    }
    std::uint32_t flags = 0;
    std::memcpy(&flags, statusMapped, sizeof(flags));
    if (flags == 0) {
        return true;
    }
    jobDiagnostic =
        makeDiagnostic(GpuOcioProgramDiagnosticCode::Unsupported,
                       "the shader rejected the frame (error flags=" + std::to_string(flags) + ")");
    return false;
}

bool GpuOcioProgram::Impl::drainAndRetire() noexcept {
    if (!queueSubmitted) {
        return true;
    }
    if (deviceLost) {
        queueSubmitted = false;
        return true;
    }
    const VkFence rawFence = static_cast<VkFence>(*fence);
    const VkResult waited = control->device.getDispatcher()->vkWaitForFences(
        static_cast<VkDevice>(*control->device), 1, &rawFence, VK_TRUE,
        ocio_program_detail::kDrainTimeoutNanoseconds);
    if (waited == VK_SUCCESS) {
        queueSubmitted = false;
        return true;
    }
    if (waited == VK_ERROR_DEVICE_LOST) {
        deviceLost = true;
        queueSubmitted = false;
        return true;
    }
    return false;
}

GpuOcioProgram::Impl::~Impl() {
    if (control == nullptr) {
        return;
    }
    releaseTransient();
    for (auto& texture : textures) {
        ocio_program_detail::destroySampledResource(*control, texture);
    }
    if (uniformBuffer.buffer != VK_NULL_HANDLE) {
        destroyResidentBuffer(*control, uniformBuffer);
    }
    if (status.buffer != VK_NULL_HANDLE) {
        destroyResidentBuffer(*control, status);
    }
}

} // namespace bloom::render
