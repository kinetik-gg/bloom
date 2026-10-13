#include "gpu_composite_private.hpp"

#include "gpu_composite_fault.hpp"
#include "gpu_native_buffer_allocation_private.hpp"

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

namespace bloom::render {
using vulkan_detail::DeviceAllocatorState;

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
        storageToken = std::move(other.storageToken);
        stagingStorageToken = std::move(other.stagingStorageToken);
        buffer = other.buffer;
        allocation = other.allocation;
        stagingBuffer = std::exchange(other.stagingBuffer, VK_NULL_HANDLE);
        stagingAllocation = std::exchange(other.stagingAllocation, VK_NULL_HANDLE);
        stagingBytes = std::exchange(other.stagingBytes, 0);
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

void CompositeBuffer::releaseStaging() noexcept {
    if (stagingBuffer != VK_NULL_HANDLE && state != nullptr) {
        vmaDestroyBuffer(state->allocator, stagingBuffer, stagingAllocation);
        if (stagingStorageToken.bytes() != 0 && state->nativeBufferBackingsDestroyed != UINT64_MAX)
            ++state->nativeBufferBackingsDestroyed;
        composite_detail::metadataStagingLiveBytes.fetch_sub(stagingBytes);
        composite_detail::metadataStagingReclaims.fetch_add(1);
    }
    stagingBuffer = VK_NULL_HANDLE;
    stagingAllocation = VK_NULL_HANDLE;
    stagingBytes = 0;
    stagingStorageToken = {};
}

void CompositeBuffer::recordInitialization(const VkCommandBuffer commandBuffer) const noexcept {
    if (stagingBuffer == VK_NULL_HANDLE) {
        return;
    }
    const auto* dispatcher = state->device.getDispatcher();
    VkBufferCopy copy{};
    copy.size = bytes;
    dispatcher->vkCmdCopyBuffer(commandBuffer, stagingBuffer, buffer, 1, &copy);
    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = buffer;
    barrier.size = bytes;
    dispatcher->vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1,
                                     &barrier, 0, nullptr);
}

void CompositeBuffer::release() noexcept {
    releaseStaging();
    if (armed && state != nullptr) {
        vmaDestroyBuffer(state->allocator, buffer, allocation);
        if (storageToken.bytes() != 0 && state->nativeBufferBackingsDestroyed != UINT64_MAX)
            ++state->nativeBufferBackingsDestroyed;
    }
    storageToken = {};
    state = nullptr;
    buffer = VK_NULL_HANDLE;
    allocation = VK_NULL_HANDLE;
    bytes = 0;
    hostVisible = false;
    armed = false;
}

bool createCompositeBuffer(const std::shared_ptr<DeviceAllocatorState>& owner,
                           const std::uint64_t bytes, const bool hostVisible,
                           const void* const initialData, CompositeBuffer& out) noexcept {
    if (owner == nullptr || bytes == 0 || owner->allocator == VK_NULL_HANDLE) {
        return false;
    }
    DeviceAllocatorState& state = *owner;
    // Metadata and status buffers are tiny and bounded by the caller's budget; refuse anything that
    // could not be addressed in one binding.
    if (bytes > state.maxStorageBufferRange) {
        return false;
    }

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = bytes;
    bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (hostVisible || initialData != nullptr) {
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
    created.state = owner;
    created.bytes = bytes;
    created.hostVisible = hostVisible;
    VmaAllocationInfo allocationInfo{};
    if (vulkan_detail::allocateCoveredBuffer(
            state, bufferInfo, allocationCreate, std::numeric_limits<std::uint64_t>::max(),
            created.buffer, created.allocation, allocationInfo, created.storageToken,
            vulkan_detail::NativeBufferPurpose::Processing) !=
        vulkan_detail::NativeBufferAllocationOutcome::Allocated) {
        return false;
    }
    created.armed = true;

    if (hostVisible && initialData != nullptr && allocationInfo.pMappedData == nullptr) {
        return false;
    }
    bool directMapped = false;
    if (hostVisible && initialData != nullptr && allocationInfo.pMappedData != nullptr) {
        std::memcpy(allocationInfo.pMappedData, initialData, static_cast<std::size_t>(bytes));
        if (composite_detail::mappedMemoryVisibility(false, state.allocator, created.allocation,
                                                     bytes) != VK_SUCCESS) {
            return false;
        }
        directMapped = true;
    }
    if (!directMapped && initialData != nullptr) {
        // Retained staging is recorded into the owning operation command buffer.
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
        NativeBufferStorageToken stagingToken;
        if (vulkan_detail::allocateCoveredBuffer(
                state, stagingInfo, stagingAllocation, std::numeric_limits<std::uint64_t>::max(),
                stagingBuffer, stagingHandle, stagingMapped, stagingToken,
                vulkan_detail::NativeBufferPurpose::Processing) !=
            vulkan_detail::NativeBufferAllocationOutcome::Allocated) {
            created.release();
            return false;
        }
        created.stagingBuffer = stagingBuffer;
        created.stagingAllocation = stagingHandle;
        created.stagingBytes = stagingMapped.size;
        created.stagingStorageToken = std::move(stagingToken);
        composite_detail::metadataStagingLiveBytes.fetch_add(created.stagingBytes);
        if (stagingMapped.pMappedData == nullptr) {
            return false;
        }
        std::memcpy(stagingMapped.pMappedData, initialData, static_cast<std::size_t>(bytes));
        if (composite_detail::mappedMemoryVisibility(false, state.allocator, stagingHandle,
                                                     bytes) != VK_SUCCESS) {
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

// Lazily builds the composite native resource set under the bounded slot. Defined in this cohesive
// translation unit (rather than gpu_composite.cpp) so the public API file stays within its budget.
void GpuComposite::Impl::resetPipelineResources() noexcept {
    // Children before parents: a descriptor set frees through its pool and a command buffer frees
    // through its command pool, so both parent handles must outlive the child. Reset in reverse
    // declaration order to match the implicit member destruction order.
    fence = vk::raii::Fence{nullptr};
    commandBuffer = vk::raii::CommandBuffer{nullptr};
    commandPool = vk::raii::CommandPool{nullptr};
    sourceOverSet = vk::raii::DescriptorSet{nullptr};
    translationSet = vk::raii::DescriptorSet{nullptr};
    descriptorPool = vk::raii::DescriptorPool{nullptr};
    sourceOver = CompositePipeline{};
    translation = CompositePipeline{};
    pipelinesReady = false;
}

bool GpuComposite::Impl::ensureResidentReady() {
    if (!acquireResidentSlot()) {
        createDiagnostic =
            compositeDiagnostic(GpuCompositeDiagnosticCode::DeviceUnavailable,
                                "the bounded composite resident pool is full; no native resources "
                                "were allocated");
        return false;
    }
    if (!pipelinesReady) {
        if (!createPipelines()) {
            resetPipelineResources();
            releaseResidentSlot();
            return false;
        }
        pipelinesReady = true;
    }
    return true;
}

bool GpuComposite::Impl::createPipelines() {
    constexpr bool kTranslationBindings[kTranslationBindingCount] = {true, true, false, false,
                                                                     false};
    constexpr bool kSourceOverBindings[kSourceOverBindingCount] = {true, true, true, false};
    std::string reason;
    if (!createCompositePipeline(*control, vulkan_detail::kTranslationOpacitySpirvCode,
                                 vulkan_detail::kTranslationOpacitySpirvByteCount,
                                 kTranslationBindings, kTranslationBindingCount,
                                 kTranslationPushBytes, reason, translation)) {
        createDiagnostic = compositeDiagnostic(GpuCompositeDiagnosticCode::ShaderRejected, reason);
        return false;
    }
    if (!createCompositePipeline(*control, vulkan_detail::kSourceOverSpirvCode,
                                 vulkan_detail::kSourceOverSpirvByteCount, kSourceOverBindings,
                                 kSourceOverBindingCount, kSourceOverPushBytes, reason,
                                 sourceOver)) {
        createDiagnostic = compositeDiagnostic(GpuCompositeDiagnosticCode::ShaderRejected, reason);
        return false;
    }

    const std::array poolSizes{vk::DescriptorPoolSize{vk::DescriptorType::eStorageImage, 5},
                               vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 5}};
    vk::DescriptorPoolCreateInfo poolInfo{};
    poolInfo.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    poolInfo.maxSets = 2;
    poolInfo.poolSizeCount = static_cast<std::uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    const VkDevice rawDevice = static_cast<VkDevice>(*control->device);
    const auto* dispatcher = control->device.getDispatcher();
    VkDescriptorPool rawPool = VK_NULL_HANDLE;
    if (dispatcher->vkCreateDescriptorPool(
            rawDevice, reinterpret_cast<const VkDescriptorPoolCreateInfo*>(&poolInfo), nullptr,
            &rawPool) != VK_SUCCESS) {
        createDiagnostic =
            compositeDiagnostic(GpuCompositeDiagnosticCode::AllocationFailed,
                                "the composite descriptor pool could not be created");
        return false;
    }
    descriptorPool = vk::raii::DescriptorPool(control->device, rawPool);
    const std::array setLayouts{*translation.descriptorSetLayout, *sourceOver.descriptorSetLayout};
    vk::DescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.descriptorPool = *descriptorPool;
    allocateInfo.descriptorSetCount = 2;
    allocateInfo.pSetLayouts = setLayouts.data();
    std::array<VkDescriptorSet, 2> rawSets{};
    if (dispatcher->vkAllocateDescriptorSets(
            rawDevice, reinterpret_cast<const VkDescriptorSetAllocateInfo*>(&allocateInfo),
            rawSets.data()) != VK_SUCCESS) {
        createDiagnostic = compositeDiagnostic(GpuCompositeDiagnosticCode::AllocationFailed,
                                               "the composite descriptor sets could not be "
                                               "allocated");
        return false;
    }
    translationSet = vk::raii::DescriptorSet(control->device, rawSets[0], *descriptorPool);
    sourceOverSet = vk::raii::DescriptorSet(control->device, rawSets[1], *descriptorPool);

    vk::CommandPoolCreateInfo commandPoolInfo{};
    commandPoolInfo.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
    commandPoolInfo.queueFamilyIndex = control->computeQueueFamily;
    VkCommandPool rawCommandPool = VK_NULL_HANDLE;
    if (dispatcher->vkCreateCommandPool(
            rawDevice, reinterpret_cast<const VkCommandPoolCreateInfo*>(&commandPoolInfo), nullptr,
            &rawCommandPool) != VK_SUCCESS) {
        createDiagnostic = compositeDiagnostic(GpuCompositeDiagnosticCode::AllocationFailed,
                                               "the composite command pool could not be created");
        return false;
    }
    commandPool = vk::raii::CommandPool(control->device, rawCommandPool);
    vk::CommandBufferAllocateInfo commandBufferInfo{};
    commandBufferInfo.commandPool = *commandPool;
    commandBufferInfo.level = vk::CommandBufferLevel::ePrimary;
    commandBufferInfo.commandBufferCount = 1;
    VkCommandBuffer rawCommandBuffer = VK_NULL_HANDLE;
    if (dispatcher->vkAllocateCommandBuffers(
            rawDevice, reinterpret_cast<const VkCommandBufferAllocateInfo*>(&commandBufferInfo),
            &rawCommandBuffer) != VK_SUCCESS) {
        createDiagnostic = compositeDiagnostic(GpuCompositeDiagnosticCode::AllocationFailed,
                                               "the composite command buffer could not be "
                                               "allocated");
        return false;
    }
    commandBuffer = vk::raii::CommandBuffer(control->device, rawCommandBuffer, *commandPool);
    vk::FenceCreateInfo fenceInfo{};
    VkFence rawFence = VK_NULL_HANDLE;
    if (dispatcher->vkCreateFence(rawDevice, reinterpret_cast<const VkFenceCreateInfo*>(&fenceInfo),
                                  nullptr, &rawFence) != VK_SUCCESS) {
        createDiagnostic = compositeDiagnostic(GpuCompositeDiagnosticCode::AllocationFailed,
                                               "the composite fence could not be created");
        return false;
    }
    fence = vk::raii::Fence(control->device, rawFence);
    // TEST-ONLY fault: the full native set has been created, so the caller's cleanup must free
    // every child before its parent and the slot must be returned for a clean retry.
    if (static_cast<composite_detail::CompositeRetirementFault>(
            composite_detail::compositeRetirementFault().load()) ==
        composite_detail::CompositeRetirementFault::FailPipelineCreation) {
        createDiagnostic = compositeDiagnostic(GpuCompositeDiagnosticCode::ShaderRejected,
                                               "injected composite pipeline creation failure");
        return false;
    }
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
        return compositeDiagnostic(
            GpuCompositeDiagnosticCode::OverBudget,
            "the source-over image or metadata exceeds the byte budget [requiredImageBytes=" +
                std::to_string(imageBytes) + ", byteBudget=" + std::to_string(byteBudget) +
                ", producerMaxImageBytes=" + std::to_string(impl.budgets.maxImageBytes) +
                ", allowedImageBytes=" + std::to_string(allowedImage) +
                ", destination=" + std::to_string(destWidth) + "x" + std::to_string(destHeight) +
                ", requiredMetadataBytes=" + std::to_string(sizeof(std::uint32_t)) +
                ", maxMetadataBytes=" + std::to_string(impl.budgets.maxMetadataBytes) + ']');
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

    // Opportunistic, non-blocking retirement of orphaned foreign-released residents.
    Impl::drainResidentOrphansOnOwnerThread();
    // Acquire the bounded resident slot BEFORE the first native allocation, and create the
    // pipelines lazily under it. A full pool refuses cleanly without allocating anything.
    if (!impl.ensureResidentReady()) {
        return impl.createDiagnostic;
    }

    impl.clearJob();
    // Before submission every request-owned allocation is immediately reclaimable. Once the
    // queue accepts the command, only the existing completion/retirement owner may release it.
    struct PreparationCleanup final {
        GpuComposite::Impl& impl;
        ~PreparationCleanup() {
            if (!impl.queueSubmitted) {
                impl.clearJob();
            }
        }
    } preparationCleanup{impl};
    const std::uint32_t zeroFlag = 0;
    if (!createCompositeBuffer(impl.control, sizeof(zeroFlag), true, &zeroFlag, impl.status)) {
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
    resident->pixelAspect = parameters.destination->pixelAspect();
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
        return compositeDiagnostic(
            GpuCompositeDiagnosticCode::OverBudget,
            "the actual source-over allocations exceed the byte budget [actualRetainedBytes=" +
                std::to_string(retainedActual) + ", byteBudget=" + std::to_string(byteBudget) +
                ", producerMaxImageBytes=" + std::to_string(impl.budgets.maxImageBytes) +
                ", destination=" + std::to_string(destWidth) + "x" + std::to_string(destHeight) +
                ']');
    }
    impl.lastJobBytes = retainedActual;

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

    composite_detail::statusToHost(*impl.control, rawCommandBuffer, impl.status.buffer,
                                   sizeof(std::uint32_t));

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
