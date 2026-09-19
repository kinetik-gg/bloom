#include "gpu_composite_private.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

namespace bloom::render {
namespace {

using vulkan_detail::DeviceAllocatorState;

std::atomic<std::int32_t> g_compositeQuarantineCount{0};
std::atomic<bool> g_compositeTeardownIncomplete{false};

constexpr std::int32_t kMaxCompositeQuarantines = 4;

} // namespace

bool compositeQuarantineAllowed() noexcept {
    return g_compositeQuarantineCount.load() < kMaxCompositeQuarantines;
}

void noteCompositeQuarantine() noexcept {
    g_compositeQuarantineCount.fetch_add(1);
    g_compositeTeardownIncomplete.store(true);
}

bool compositeTeardownIncomplete() noexcept { return g_compositeTeardownIncomplete.load(); }

std::uint64_t compositeBufferAllocationBytes(const CompositeBuffer& buffer) noexcept {
    if (!buffer.armed || buffer.state == nullptr || buffer.allocation == VK_NULL_HANDLE) {
        return 0;
    }
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(buffer.state->allocator, buffer.allocation, &info);
    return info.size;
}

std::uint64_t compositeImageAllocationBytes(const GpuImageImpl& image) noexcept {
    if (image.state == nullptr || image.allocation == VK_NULL_HANDLE) {
        return 0;
    }
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(image.state->allocator, image.allocation, &info);
    return info.size;
}

CompositeBuffer::~CompositeBuffer() { release(); }

CompositeBuffer& CompositeBuffer::operator=(CompositeBuffer&& other) noexcept {
    if (this != &other) {
        release();
        state = std::move(other.state);
        buffer = other.buffer;
        allocation = other.allocation;
        bytes = other.bytes;
        hostVisible = other.hostVisible;
        armed = other.armed;
        other.state = nullptr;
        other.buffer = VK_NULL_HANDLE;
        other.allocation = VK_NULL_HANDLE;
        other.bytes = 0;
        other.hostVisible = false;
        other.armed = false;
    }
    return *this;
}

void CompositeBuffer::release() noexcept {
    if (armed && state != nullptr) {
        vmaDestroyBuffer(state->allocator, buffer, allocation);
    }
    state = nullptr;
    buffer = VK_NULL_HANDLE;
    allocation = VK_NULL_HANDLE;
    bytes = 0;
    hostVisible = false;
    armed = false;
}

bool createCompositeBuffer(DeviceAllocatorState& state, const std::uint64_t bytes,
                           const bool hostVisible, const void* const initialData,
                           CompositeBuffer& out) noexcept {
    if (bytes == 0 || state.allocator == VK_NULL_HANDLE) {
        return false;
    }
    // Metadata and status buffers are tiny and bounded by the caller's budget; refuse anything that
    // could not be addressed in one binding.
    if (bytes > state.maxStorageBufferRange) {
        return false;
    }

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = bytes;
    bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (hostVisible) {
        bufferInfo.usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    } else if (initialData != nullptr) {
        bufferInfo.usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    }
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocationCreate{};
    allocationCreate.usage = VMA_MEMORY_USAGE_AUTO;
    if (hostVisible) {
        allocationCreate.flags =
            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }

    CompositeBuffer created;
    created.state = std::shared_ptr<DeviceAllocatorState>(&state, [](DeviceAllocatorState*) {});
    created.bytes = bytes;
    created.hostVisible = hostVisible;
    VmaAllocationInfo allocationInfo{};
    if (vmaCreateBuffer(state.allocator, &bufferInfo, &allocationCreate, &created.buffer,
                        &created.allocation, &allocationInfo) != VK_SUCCESS) {
        return false;
    }
    created.armed = true;

    const VkDevice device = static_cast<VkDevice>(*state.device);
    const auto* dispatcher = state.device.getDispatcher();

    bool directMapped = false;
    if (hostVisible && initialData != nullptr && allocationInfo.pMappedData != nullptr) {
        std::memcpy(allocationInfo.pMappedData, initialData, static_cast<std::size_t>(bytes));
        directMapped = true;
    }
    if (!directMapped && initialData != nullptr) {
        // Transient staging buffer + one-time copy, all RAII, synchronously retired.
        VkBufferCreateInfo stagingInfo{};
        stagingInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        stagingInfo.size = bytes;
        stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        stagingInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo stagingAllocation{};
        stagingAllocation.usage = VMA_MEMORY_USAGE_AUTO;
        stagingAllocation.flags =
            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VkBuffer stagingBuffer = VK_NULL_HANDLE;
        VmaAllocation stagingHandle = VK_NULL_HANDLE;
        VmaAllocationInfo stagingMapped{};
        if (vmaCreateBuffer(state.allocator, &stagingInfo, &stagingAllocation, &stagingBuffer,
                            &stagingHandle, &stagingMapped) != VK_SUCCESS) {
            created.release();
            return false;
        }
        if (stagingMapped.pMappedData == nullptr) {
            vmaDestroyBuffer(state.allocator, stagingBuffer, stagingHandle);
            created.release();
            return false;
        }
        std::memcpy(stagingMapped.pMappedData, initialData, static_cast<std::size_t>(bytes));

        vk::raii::CommandPool pool{nullptr};
        vk::raii::CommandBuffer commandBuffer{nullptr};
        vk::raii::Fence fence{nullptr};
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolInfo.queueFamilyIndex = state.computeQueueFamily;
        VkCommandPool rawPool = VK_NULL_HANDLE;
        bool ok =
            dispatcher->vkCreateCommandPool(device, &poolInfo, nullptr, &rawPool) == VK_SUCCESS;
        if (ok) {
            pool = vk::raii::CommandPool(state.device, rawPool);
            VkCommandBufferAllocateInfo allocate{};
            allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocate.commandPool = rawPool;
            allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocate.commandBufferCount = 1;
            VkCommandBuffer rawBuffer = VK_NULL_HANDLE;
            ok = dispatcher->vkAllocateCommandBuffers(device, &allocate, &rawBuffer) == VK_SUCCESS;
            if (ok) {
                commandBuffer = vk::raii::CommandBuffer(state.device, rawBuffer, rawPool);
                VkCommandBufferBeginInfo begin{};
                begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                ok = dispatcher->vkBeginCommandBuffer(rawBuffer, &begin) == VK_SUCCESS;
                if (ok) {
                    VkBufferCopy copy{};
                    copy.size = bytes;
                    dispatcher->vkCmdCopyBuffer(rawBuffer, stagingBuffer, created.buffer, 1, &copy);
                    ok = dispatcher->vkEndCommandBuffer(rawBuffer) == VK_SUCCESS;
                }
            }
        }
        if (ok) {
            VkFenceCreateInfo fenceInfo{};
            fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            VkFence rawFence = VK_NULL_HANDLE;
            ok = dispatcher->vkCreateFence(device, &fenceInfo, nullptr, &rawFence) == VK_SUCCESS;
            if (ok) {
                fence = vk::raii::Fence(state.device, rawFence);
                VkSubmitInfo submit{};
                submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                submit.commandBufferCount = 1;
                const VkCommandBuffer rawCommandBuffer =
                    static_cast<VkCommandBuffer>(*commandBuffer);
                submit.pCommandBuffers = &rawCommandBuffer;
                const VkResult submitted = dispatcher->vkQueueSubmit(
                    static_cast<VkQueue>(*state.computeQueue), 1, &submit, rawFence);
                ok = submitted == VK_SUCCESS;
                if (ok) {
                    const VkResult waited = dispatcher->vkWaitForFences(device, 1, &rawFence,
                                                                        VK_TRUE, 5'000'000'000ULL);
                    ok = waited == VK_SUCCESS || waited == VK_ERROR_DEVICE_LOST;
                }
            }
        }
        vmaDestroyBuffer(state.allocator, stagingBuffer, stagingHandle);
        if (!ok) {
            created.release();
            return false;
        }
    }

    out = std::move(created);
    return true;
}

bool createCompositePipeline(DeviceAllocatorState& state, const std::uint32_t* const spirvCode,
                             const std::uint32_t spirvBytes, const bool* const bindingIsImage,
                             const std::uint32_t bindingCount, const std::uint32_t pushBytes,
                             std::string& reason, CompositePipeline& out) noexcept {
    const VkDevice rawDevice = static_cast<VkDevice>(*state.device);
    const auto* dispatcher = state.device.getDispatcher();

    vk::ShaderModuleCreateInfo shaderInfo{};
    shaderInfo.codeSize = spirvBytes;
    shaderInfo.pCode = spirvCode;
    VkShaderModule rawShader = VK_NULL_HANDLE;
    if (dispatcher->vkCreateShaderModule(
            rawDevice, reinterpret_cast<const VkShaderModuleCreateInfo*>(&shaderInfo), nullptr,
            &rawShader) != VK_SUCCESS) {
        reason = "the embedded composite shader module was rejected";
        return false;
    }
    out.shaderModule = vk::raii::ShaderModule(state.device, rawShader);

    // `bindingIsImage` names the type of each binding explicitly, because SourceOverV1's third
    // binding is an image while TranslationOpacity's third is a buffer.
    std::vector<vk::DescriptorSetLayoutBinding> bindings;
    bindings.reserve(bindingCount);
    for (std::uint32_t index = 0; index < bindingCount; ++index) {
        vk::DescriptorSetLayoutBinding binding{};
        binding.binding = index;
        binding.descriptorType = bindingIsImage[index] ? vk::DescriptorType::eStorageImage
                                                       : vk::DescriptorType::eStorageBuffer;
        binding.descriptorCount = 1;
        binding.stageFlags = vk::ShaderStageFlagBits::eCompute;
        bindings.push_back(binding);
    }
    vk::DescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.bindingCount = bindingCount;
    layoutInfo.pBindings = bindings.data();
    VkDescriptorSetLayout rawLayout = VK_NULL_HANDLE;
    if (dispatcher->vkCreateDescriptorSetLayout(
            rawDevice, reinterpret_cast<const VkDescriptorSetLayoutCreateInfo*>(&layoutInfo),
            nullptr, &rawLayout) != VK_SUCCESS) {
        reason = "the composite descriptor set layout was rejected";
        return false;
    }
    out.descriptorSetLayout = vk::raii::DescriptorSetLayout(state.device, rawLayout);

    vk::PushConstantRange pushRange{};
    pushRange.stageFlags = vk::ShaderStageFlagBits::eCompute;
    pushRange.offset = 0;
    pushRange.size = pushBytes;
    const vk::DescriptorSetLayout setLayout = *out.descriptorSetLayout;
    vk::PipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &setLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;
    VkPipelineLayout rawPipelineLayout = VK_NULL_HANDLE;
    if (dispatcher->vkCreatePipelineLayout(
            rawDevice, reinterpret_cast<const VkPipelineLayoutCreateInfo*>(&pipelineLayoutInfo),
            nullptr, &rawPipelineLayout) != VK_SUCCESS) {
        reason = "the composite pipeline layout was rejected";
        return false;
    }
    out.pipelineLayout = vk::raii::PipelineLayout(state.device, rawPipelineLayout);

    vk::ComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.stage.stage = vk::ShaderStageFlagBits::eCompute;
    pipelineInfo.stage.module = *out.shaderModule;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = *out.pipelineLayout;
    VkPipeline rawPipeline = VK_NULL_HANDLE;
    if (dispatcher->vkCreateComputePipelines(
            rawDevice, VK_NULL_HANDLE, 1,
            reinterpret_cast<const VkComputePipelineCreateInfo*>(&pipelineInfo), nullptr,
            &rawPipeline) != VK_SUCCESS) {
        reason = "the composite compute pipeline was rejected";
        return false;
    }
    out.pipeline = vk::raii::Pipeline(state.device, rawPipeline);
    return true;
}

// beginSourceOver follows the same shape; it is declared in the public header and defined here.
GpuCompositeDiagnostic GpuComposite::beginSourceOver(const GpuSourceOverParameters& parameters,
                                                     const std::uint64_t byteBudget) {
    if (impl_ == nullptr) {
        return compositeDiagnostic(GpuCompositeDiagnosticCode::DeviceUnavailable,
                                   "the composite pipeline is not initialized");
    }
    Impl& impl = *impl_;
    if (!impl.onOwnerThread()) {
        return compositeDiagnostic(GpuCompositeDiagnosticCode::WrongThread,
                                   "beginSourceOver must run on the device owner thread");
    }
    if (impl.deviceLost) {
        return compositeDiagnostic(GpuCompositeDiagnosticCode::DeviceLost,
                                   "the device was lost; this generation must not be reused");
    }
    if (impl.queueSubmitted || impl.jobState == GpuCompositeJobState::Pending) {
        return compositeDiagnostic(GpuCompositeDiagnosticCode::Busy,
                                   "one job is already in flight");
    }
    if (parameters.source == nullptr || !parameters.source->isValid() ||
        parameters.destination == nullptr || !parameters.destination->isValid()) {
        return compositeDiagnostic(GpuCompositeDiagnosticCode::InvalidArgument,
                                   "the source-over inputs are missing");
    }
    const auto sourceWindow = parameters.source->dataWindow();
    const auto destinationWindow = parameters.destination->dataWindow();
    if (!sourceWindow.has_value() || !destinationWindow.has_value()) {
        return compositeDiagnostic(GpuCompositeDiagnosticCode::InvalidArgument,
                                   "a source-over input has no data window");
    }
    const std::uint32_t destWidth = destinationWindow->extent().width();
    const std::uint32_t destHeight = destinationWindow->extent().height();
    const std::uint32_t sourceWidth = sourceWindow->extent().width();
    const std::uint32_t sourceHeight = sourceWindow->extent().height();
    if (destWidth == 0 || destHeight == 0 || sourceWidth == 0 || sourceHeight == 0) {
        return compositeDiagnostic(GpuCompositeDiagnosticCode::InvalidArgument,
                                   "an extent is empty");
    }
    std::uint64_t imageBytes = 0;
    if (compositeMultiplyOverflows(destWidth, destHeight, imageBytes) ||
        compositeMultiplyOverflows(imageBytes, sizeof(Rgba32f), imageBytes)) {
        return compositeDiagnostic(GpuCompositeDiagnosticCode::OverBudget,
                                   "the source-over request overflows the byte arithmetic");
    }
    const std::uint64_t allowedImage =
        byteBudget < impl.budgets.maxImageBytes ? byteBudget : impl.budgets.maxImageBytes;
    if (imageBytes > allowedImage || sizeof(std::uint32_t) > impl.budgets.maxMetadataBytes) {
        return compositeDiagnostic(GpuCompositeDiagnosticCode::OverBudget,
                                   "the source-over image or metadata exceeds the byte budget");
    }
    if (impl.control->generation != impl.expectedGeneration) {
        impl.deviceLost = true;
        return compositeDiagnostic(
            GpuCompositeDiagnosticCode::DeviceLost,
            "the device generation changed; this pipeline must not be reused");
    }
    // Both inputs must belong to exactly this device generation BEFORE any driver resource is
    // created or bound.
    if (!compositeImageBelongsTo(gpuImageImpl(*parameters.source), impl.control) ||
        !compositeImageBelongsTo(gpuImageImpl(*parameters.destination), impl.control)) {
        return compositeDiagnostic(GpuCompositeDiagnosticCode::InvalidArgument,
                                   "a source-over input image does not belong to this device");
    }
    const SolidImageSupport support = querySolidImageSupport(*impl.control, destWidth, destHeight);
    if (!support.supported || imageBytes > support.maxImageBytes) {
        return compositeDiagnostic(
            support.supported ? GpuCompositeDiagnosticCode::OverBudget
                              : GpuCompositeDiagnosticCode::Unsupported,
            support.supported ? "the destination image exceeds the device limit" : support.reason);
    }

    impl.clearJob();
    const std::uint32_t zeroFlag = 0;
    if (!createCompositeBuffer(*impl.control, sizeof(zeroFlag), true, &zeroFlag, impl.status)) {
        return compositeDiagnostic(GpuCompositeDiagnosticCode::AllocationFailed,
                                   "the status buffer could not be allocated");
    }
    VmaAllocationInfo statusInfo{};
    vmaGetAllocationInfo(impl.control->allocator, impl.status.allocation, &statusInfo);
    impl.statusMapped = statusInfo.pMappedData;

    // The destination is read-only BACKDROP; the operation writes its own resident OUTPUT image, so
    // the caller's destination is never mutated and takeImage() returns a real result.
    const GpuImageImpl* const destRaw = gpuImageImpl(*parameters.destination);
    auto resident = std::make_unique<GpuImageImpl>();
    resident->state = impl.control;
    resident->dataWindow = parameters.destination->dataWindow();
    resident->displayWindow = parameters.destination->displayWindow();
    resident->generation = impl.control->generation;
    if (!createResidentImage(*impl.control, destWidth, destHeight, *resident)) {
        return compositeDiagnostic(GpuCompositeDiagnosticCode::AllocationFailed,
                                   "the source-over output image could not be allocated");
    }
    GpuImageImpl* const outputRaw = resident.get();

    // Enforce the budget on the ACTUAL VMA allocation sizes (allocator rounding included), not the
    // requested pixel bytes. The inputs' retained allocations belong to the caller/cache and are
    // deliberately not counted here.
    std::uint64_t retainedActual = 0;
    {
        const std::uint64_t outputActual = compositeImageAllocationBytes(*outputRaw);
        const std::uint64_t statusActual = compositeBufferAllocationBytes(impl.status);
        if (compositeAddOverflows(outputActual, statusActual, retainedActual)) {
            return compositeDiagnostic(
                GpuCompositeDiagnosticCode::OverBudget,
                "the source-over allocation sizes overflow the byte arithmetic");
        }
    }
    if (retainedActual > byteBudget) {
        return compositeDiagnostic(GpuCompositeDiagnosticCode::OverBudget,
                                   "the actual source-over allocations exceed the byte budget");
    }

    impl.residentImage = std::make_unique<GpuImage>(makeGpuImage(std::move(resident)));
    impl.retainedSource = parameters.source;
    impl.retainedDestination = parameters.destination;
    impl.translationJob = false;

    const VkDevice rawDevice = static_cast<VkDevice>(*impl.control->device);
    const auto* dispatcher = impl.control->device.getDispatcher();
    const VkCommandBuffer rawCommandBuffer = static_cast<VkCommandBuffer>(*impl.commandBuffer);
    const VkFence rawFence = static_cast<VkFence>(*impl.fence);
    if (dispatcher->vkResetFences(rawDevice, 1, &rawFence) != VK_SUCCESS ||
        dispatcher->vkResetCommandBuffer(rawCommandBuffer, 0) != VK_SUCCESS) {
        return compositeDiagnostic(GpuCompositeDiagnosticCode::DeviceUnavailable,
                                   "the composite fence or command buffer could not be reset");
    }

    vk::DescriptorImageInfo sourceInfo{};
    sourceInfo.imageView = gpuImageImpl(*impl.retainedSource)->view;
    sourceInfo.imageLayout = vk::ImageLayout::eGeneral;
    vk::DescriptorImageInfo destInfo{};
    destInfo.imageView = destRaw->view;
    destInfo.imageLayout = vk::ImageLayout::eGeneral;
    vk::DescriptorImageInfo outputInfo{};
    outputInfo.imageView = outputRaw->view;
    outputInfo.imageLayout = vk::ImageLayout::eGeneral;
    vk::DescriptorBufferInfo statusInfoWrite{};
    statusInfoWrite.buffer = impl.status.buffer;
    statusInfoWrite.range = VK_WHOLE_SIZE;
    std::array<vk::WriteDescriptorSet, kSourceOverBindingCount> writes{};
    writes[0] = vk::WriteDescriptorSet{};
    writes[0].dstSet = *impl.sourceOverSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = vk::DescriptorType::eStorageImage;
    writes[0].pImageInfo = &sourceInfo;
    writes[1] = writes[0];
    writes[1].dstBinding = 1;
    writes[1].pImageInfo = &destInfo;
    writes[2] = writes[0];
    writes[2].dstBinding = 2;
    writes[2].pImageInfo = &outputInfo;
    writes[3] = vk::WriteDescriptorSet{};
    writes[3].dstSet = *impl.sourceOverSet;
    writes[3].dstBinding = 3;
    writes[3].descriptorCount = 1;
    writes[3].descriptorType = vk::DescriptorType::eStorageBuffer;
    writes[3].pBufferInfo = &statusInfoWrite;
    dispatcher->vkUpdateDescriptorSets(rawDevice, static_cast<std::uint32_t>(writes.size()),
                                       reinterpret_cast<const VkWriteDescriptorSet*>(writes.data()),
                                       0, nullptr);

    vk::CommandBufferBeginInfo beginInfo{};
    beginInfo.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
    if (dispatcher->vkBeginCommandBuffer(
            rawCommandBuffer, reinterpret_cast<const VkCommandBufferBeginInfo*>(&beginInfo)) !=
        VK_SUCCESS) {
        return compositeDiagnostic(GpuCompositeDiagnosticCode::DeviceUnavailable,
                                   "the composite command buffer could not begin");
    }
    // The output is fresh (UNDEFINED); the source and backdrop are produced by earlier operations
    // and are already GENERAL.
    VkImageMemoryBarrier outputBarrier{};
    outputBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    outputBarrier.srcAccessMask = 0;
    outputBarrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    outputBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    outputBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    outputBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    outputBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    outputBarrier.image = outputRaw->image;
    outputBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    dispatcher->vkCmdPipelineBarrier(rawCommandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                                     nullptr, 1, &outputBarrier);

    impl.commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, *impl.sourceOver.pipeline);
    impl.commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
                                          *impl.sourceOver.pipelineLayout, 0, {*impl.sourceOverSet},
                                          {});
    const CompositeSourceOverPush push{
        destWidth,
        destHeight,
        sourceWidth,
        sourceHeight,
        static_cast<std::int32_t>(destinationWindow->originX() - sourceWindow->originX()),
        static_cast<std::int32_t>(destinationWindow->originY() - sourceWindow->originY())};
    impl.commandBuffer.pushConstants(*impl.sourceOver.pipelineLayout,
                                     vk::ShaderStageFlagBits::eCompute, 0,
                                     sizeof(CompositeSourceOverPush), &push);
    // One invocation per destination pixel: X tiles the row in 256-wide groups, Y is the row.
    const auto groups = static_cast<std::uint32_t>(
        compositeDispatchGroupCount(static_cast<std::uint64_t>(destWidth)));
    impl.commandBuffer.dispatch(groups, destHeight, 1);

    VkImageMemoryBarrier toRead = outputBarrier;
    toRead.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dispatcher->vkCmdPipelineBarrier(rawCommandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                                     nullptr, 1, &toRead);

    if (dispatcher->vkEndCommandBuffer(rawCommandBuffer) != VK_SUCCESS) {
        return compositeDiagnostic(GpuCompositeDiagnosticCode::DeviceUnavailable,
                                   "the composite command buffer could not end");
    }
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &rawCommandBuffer;
    const VkResult submitted = dispatcher->vkQueueSubmit(
        static_cast<VkQueue>(*impl.control->computeQueue), 1, &submit, rawFence);
    if (submitted == VK_ERROR_DEVICE_LOST) {
        impl.deviceLost = true;
        return compositeDiagnostic(GpuCompositeDiagnosticCode::DeviceLost,
                                   "the device was lost during submission");
    }
    if (submitted != VK_SUCCESS) {
        return compositeDiagnostic(GpuCompositeDiagnosticCode::DeviceUnavailable,
                                   "the source-over dispatch could not submit");
    }
    impl.queueSubmitted = true;
    impl.jobState = GpuCompositeJobState::Pending;
    impl.jobDiagnostic = GpuCompositeDiagnostic{};
    return {};
}

} // namespace bloom::render
