// Resource creation, capacity management, and owner-thread retirement for the fixed Bloom Neutral
// v1 display compute operation. Kept separate from the dispatch/poll translation unit so each stays
// within the repository's source-size budget; both share Impl through the private header.

#include "gpu_neutral_display_private.hpp"
#include "shaders/neutral_display_spirv.inc"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace bloom::render::neutral_display_detail {

namespace {
std::atomic<std::uint32_t> g_quarantineCount{0};
std::atomic<bool> g_teardownIncomplete{false};
std::atomic<std::int32_t> g_fenceOverride{0};
} // namespace

std::uint64_t gpuBytesForPixels(const std::uint32_t pixelCount) noexcept {
    const std::uint64_t pixels = pixelCount;
    return (pixels * kInputBytesPerPixel) + (pixels * kOutputBytesPerPixel) + kStatusBytes;
}

void destroyStorageBuffer(vulkan_detail::DeviceAllocatorState& state,
                          StorageBuffer& storage) noexcept {
    if (storage.buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(state.allocator, storage.buffer, storage.allocation);
    }
    storage = StorageBuffer{};
}

bool createStorageBuffer(vulkan_detail::DeviceAllocatorState& state, const std::uint64_t bytes,
                         const VkBufferUsageFlags usage, const VmaAllocationCreateFlags flags,
                         StorageBuffer& storage) {
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
    VmaAllocationInfo mappedInfo{};
    if (vmaCreateBuffer(state.allocator, &bufferInfo, &allocationInfo, &buffer, &allocation,
                        &mappedInfo) != VK_SUCCESS) {
        return false;
    }
    if (mappedInfo.pMappedData == nullptr) {
        vmaDestroyBuffer(state.allocator, buffer, allocation);
        return false;
    }
    storage.buffer = buffer;
    storage.allocation = allocation;
    storage.info = mappedInfo;
    storage.capacityBytes = bytes;
    return true;
}

std::string errorFlagMessage(const std::uint32_t flags) {
    std::string message = "the compute shader rejected the frame (error flags=";
    message += std::to_string(flags);
    message += ')';
    if ((flags & 1U) != 0U) {
        message += " [non-finite input]";
    }
    if ((flags & 2U) != 0U) {
        message += " [non-finite unpremultiply]";
    }
    if ((flags & 4U) != 0U) {
        message += " [non-finite display output]";
    }
    if ((flags & 8U) != 0U) {
        message += " [subnormal input not qualified]";
    }
    return message;
}

bool quarantineAllowed() noexcept { return g_quarantineCount.load() < kMaxQuarantines; }

void noteQuarantine() noexcept {
    g_quarantineCount.fetch_add(1);
    g_teardownIncomplete.store(true);
}

bool teardownIncomplete() noexcept { return g_teardownIncomplete.load(); }

void setFenceOverride(const FenceOverride override) noexcept {
    g_fenceOverride.store(static_cast<std::int32_t>(override));
}

VkResult effectiveFenceStatus(const VkResult real) noexcept {
    switch (g_fenceOverride.load()) {
    case static_cast<std::int32_t>(FenceOverride::NotReady):
        return VK_NOT_READY;
    case static_cast<std::int32_t>(FenceOverride::Success):
        return VK_SUCCESS;
    case static_cast<std::int32_t>(FenceOverride::Unknown):
        return VK_ERROR_UNKNOWN;
    default:
        return real;
    }
}

} // namespace bloom::render::neutral_display_detail

namespace bloom::render {

using namespace neutral_display_detail;

void GpuNeutralDisplay::Impl::destroyBuffers() noexcept {
    destroyStorageBuffer(*state, input);
    destroyStorageBuffer(*state, output);
    destroyStorageBuffer(*state, status);
    capacityPixels = 0;
    ownedBytes = 0;
}

bool GpuNeutralDisplay::Impl::createPipeline() noexcept {
    const VkDevice rawDevice = static_cast<VkDevice>(*state->device);
    const auto* dispatcher = state->device.getDispatcher();

    vk::ShaderModuleCreateInfo shaderInfo{};
    shaderInfo.codeSize = vulkan_detail::kNeutralDisplaySpirvByteCount;
    shaderInfo.pCode = vulkan_detail::kNeutralDisplaySpirvCode;
    VkShaderModule rawShader = VK_NULL_HANDLE;
    if (dispatcher->vkCreateShaderModule(
            rawDevice, reinterpret_cast<const VkShaderModuleCreateInfo*>(&shaderInfo), nullptr,
            &rawShader) != VK_SUCCESS) {
        createDiagnostic = {GpuNeutralDisplayDiagnosticCode::ShaderRejected,
                            "the embedded display shader module was rejected"};
        return false;
    }
    shaderModule = vk::raii::ShaderModule(state->device, rawShader);

    std::array<vk::DescriptorSetLayoutBinding, 3> bindings{};
    for (std::uint32_t index = 0; index < 3; ++index) {
        bindings[index].binding = index;
        bindings[index].descriptorType = vk::DescriptorType::eStorageBuffer;
        bindings[index].descriptorCount = 1;
        bindings[index].stageFlags = vk::ShaderStageFlagBits::eCompute;
    }
    vk::DescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    VkDescriptorSetLayout rawLayout = VK_NULL_HANDLE;
    if (dispatcher->vkCreateDescriptorSetLayout(
            rawDevice, reinterpret_cast<const VkDescriptorSetLayoutCreateInfo*>(&layoutInfo),
            nullptr, &rawLayout) != VK_SUCCESS) {
        createDiagnostic = {GpuNeutralDisplayDiagnosticCode::ShaderRejected,
                            "the display descriptor set layout was rejected"};
        return false;
    }
    descriptorSetLayout = vk::raii::DescriptorSetLayout(state->device, rawLayout);

    vk::PushConstantRange pushRange{};
    pushRange.stageFlags = vk::ShaderStageFlagBits::eCompute;
    pushRange.offset = 0;
    pushRange.size = static_cast<std::uint32_t>(sizeof(std::uint32_t));
    const vk::DescriptorSetLayout setLayout = *descriptorSetLayout;
    vk::PipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &setLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;
    VkPipelineLayout rawPipelineLayout = VK_NULL_HANDLE;
    if (dispatcher->vkCreatePipelineLayout(
            rawDevice, reinterpret_cast<const VkPipelineLayoutCreateInfo*>(&pipelineLayoutInfo),
            nullptr, &rawPipelineLayout) != VK_SUCCESS) {
        createDiagnostic = {GpuNeutralDisplayDiagnosticCode::ShaderRejected,
                            "the display pipeline layout was rejected"};
        return false;
    }
    pipelineLayout = vk::raii::PipelineLayout(state->device, rawPipelineLayout);

    vk::ComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.stage.stage = vk::ShaderStageFlagBits::eCompute;
    pipelineInfo.stage.module = *shaderModule;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = *pipelineLayout;
    VkPipeline rawPipeline = VK_NULL_HANDLE;
    if (dispatcher->vkCreateComputePipelines(
            rawDevice, VK_NULL_HANDLE, 1,
            reinterpret_cast<const VkComputePipelineCreateInfo*>(&pipelineInfo), nullptr,
            &rawPipeline) != VK_SUCCESS) {
        createDiagnostic = {GpuNeutralDisplayDiagnosticCode::ShaderRejected,
                            "the embedded display compute pipeline was rejected"};
        return false;
    }
    pipeline = vk::raii::Pipeline(state->device, rawPipeline);

    const vk::DescriptorPoolSize poolSize{vk::DescriptorType::eStorageBuffer, 3};
    vk::DescriptorPoolCreateInfo poolInfo{};
    poolInfo.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    VkDescriptorPool rawPool = VK_NULL_HANDLE;
    if (dispatcher->vkCreateDescriptorPool(
            rawDevice, reinterpret_cast<const VkDescriptorPoolCreateInfo*>(&poolInfo), nullptr,
            &rawPool) != VK_SUCCESS) {
        createDiagnostic = {GpuNeutralDisplayDiagnosticCode::AllocationFailed,
                            "the display descriptor pool could not be created"};
        return false;
    }
    descriptorPool = vk::raii::DescriptorPool(state->device, rawPool);

    vk::DescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.descriptorPool = *descriptorPool;
    allocateInfo.descriptorSetCount = 1;
    allocateInfo.pSetLayouts = &setLayout;
    VkDescriptorSet rawSet = VK_NULL_HANDLE;
    if (dispatcher->vkAllocateDescriptorSets(
            rawDevice, reinterpret_cast<const VkDescriptorSetAllocateInfo*>(&allocateInfo),
            &rawSet) != VK_SUCCESS) {
        createDiagnostic = {GpuNeutralDisplayDiagnosticCode::AllocationFailed,
                            "the display descriptor set could not be allocated"};
        return false;
    }
    descriptorSet = vk::raii::DescriptorSet(state->device, rawSet, *descriptorPool);

    vk::CommandPoolCreateInfo commandPoolInfo{};
    commandPoolInfo.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
    commandPoolInfo.queueFamilyIndex = state->computeQueueFamily;
    VkCommandPool rawCommandPool = VK_NULL_HANDLE;
    if (dispatcher->vkCreateCommandPool(
            rawDevice, reinterpret_cast<const VkCommandPoolCreateInfo*>(&commandPoolInfo), nullptr,
            &rawCommandPool) != VK_SUCCESS) {
        createDiagnostic = {GpuNeutralDisplayDiagnosticCode::AllocationFailed,
                            "the display command pool could not be created"};
        return false;
    }
    commandPool = vk::raii::CommandPool(state->device, rawCommandPool);

    vk::CommandBufferAllocateInfo commandBufferInfo{};
    commandBufferInfo.commandPool = *commandPool;
    commandBufferInfo.level = vk::CommandBufferLevel::ePrimary;
    commandBufferInfo.commandBufferCount = 1;
    VkCommandBuffer rawCommandBuffer = VK_NULL_HANDLE;
    if (dispatcher->vkAllocateCommandBuffers(
            rawDevice, reinterpret_cast<const VkCommandBufferAllocateInfo*>(&commandBufferInfo),
            &rawCommandBuffer) != VK_SUCCESS) {
        createDiagnostic = {GpuNeutralDisplayDiagnosticCode::AllocationFailed,
                            "the display command buffer could not be allocated"};
        return false;
    }
    commandBuffer = vk::raii::CommandBuffer(state->device, rawCommandBuffer, *commandPool);

    vk::FenceCreateInfo fenceInfo{};
    VkFence rawFence = VK_NULL_HANDLE;
    if (dispatcher->vkCreateFence(rawDevice, reinterpret_cast<const VkFenceCreateInfo*>(&fenceInfo),
                                  nullptr, &rawFence) != VK_SUCCESS) {
        createDiagnostic = {GpuNeutralDisplayDiagnosticCode::AllocationFailed,
                            "the display fence could not be created"};
        return false;
    }
    fence = vk::raii::Fence(state->device, rawFence);

    if (!createStorageBuffer(*state, kStatusBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                 VMA_ALLOCATION_CREATE_MAPPED_BIT,
                             status)) {
        createDiagnostic = {GpuNeutralDisplayDiagnosticCode::AllocationFailed,
                            "the status buffer could not be allocated"};
        return false;
    }
    ownedBytes = kStatusBytes;
    return true;
}

bool GpuNeutralDisplay::Impl::ensureCapacity(const std::uint32_t pixelCount,
                                             const std::uint64_t perRequestBudget) {
    const std::uint64_t required = gpuBytesForPixels(pixelCount);
    const std::uint64_t allowed =
        perRequestBudget < budgets.maxOwnedBytes ? perRequestBudget : budgets.maxOwnedBytes;
    if (required > allowed) {
        return false;
    }
    // The retained buffers must fit the tighter of the hard ceiling and this request's budget;
    // otherwise a later, tighter request would run against retained capacity larger than its own
    // admission cost. Reallocate (shrink or grow) whenever the current capacity is outside
    // [pixelCount, allowed].
    const bool capacityFits =
        capacityPixels >= pixelCount &&
        gpuBytesForPixels(static_cast<std::uint32_t>(capacityPixels)) <= allowed;
    if (capacityFits) {
        return true;
    }

    // Free the idle old buffers before allocating so the grow peak is bounded by the new size, not
    // old+new. Nothing is in flight: ensureCapacity is only reached while the pipeline is idle.
    destroyStorageBuffer(*state, input);
    destroyStorageBuffer(*state, output);
    capacityPixels = 0;
    ownedBytes = 0;

    const std::uint64_t inputBytes = static_cast<std::uint64_t>(pixelCount) * kInputBytesPerPixel;
    const std::uint64_t outputBytes = static_cast<std::uint64_t>(pixelCount) * kOutputBytesPerPixel;
    if (!createStorageBuffer(*state, inputBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                 VMA_ALLOCATION_CREATE_MAPPED_BIT,
                             input)) {
        return false;
    }
    if (!createStorageBuffer(*state, outputBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                 VMA_ALLOCATION_CREATE_MAPPED_BIT,
                             output)) {
        destroyStorageBuffer(*state, input);
        return false;
    }
    capacityPixels = pixelCount;
    ownedBytes = required;
    return true;
}

void GpuNeutralDisplay::Impl::updateDescriptors() const {
    const std::array<vk::DescriptorBufferInfo, 3> bufferInfos{
        vk::DescriptorBufferInfo{input.buffer, 0, VK_WHOLE_SIZE},
        vk::DescriptorBufferInfo{output.buffer, 0, VK_WHOLE_SIZE},
        vk::DescriptorBufferInfo{status.buffer, 0, VK_WHOLE_SIZE}};

    std::array<vk::WriteDescriptorSet, 3> writes{};
    for (std::uint32_t index = 0; index < 3; ++index) {
        writes[index].dstSet = *descriptorSet;
        writes[index].dstBinding = index;
        writes[index].descriptorCount = 1;
        writes[index].descriptorType = vk::DescriptorType::eStorageBuffer;
        writes[index].pBufferInfo = &bufferInfos[index];
    }
    state->device.getDispatcher()->vkUpdateDescriptorSets(
        static_cast<VkDevice>(*state->device), static_cast<std::uint32_t>(writes.size()),
        reinterpret_cast<const VkWriteDescriptorSet*>(writes.data()), 0, nullptr);
}

bool GpuNeutralDisplay::Impl::drainAndRetire() noexcept {
    if (!queueSubmitted) {
        return true;
    }
    if (deviceLost) {
        // A lost device's queue can never signal the fence again; Vulkan guarantees the submission
        // is no longer executing, so retirement is proved and destruction is safe.
        queueSubmitted = false;
        return true;
    }
    const VkFence rawFence = static_cast<VkFence>(*fence);
    const VkResult waited = state->device.getDispatcher()->vkWaitForFences(
        static_cast<VkDevice>(*state->device), 1, &rawFence, VK_TRUE, kDrainTimeoutNanoseconds);
    if (waited == VK_SUCCESS) {
        queueSubmitted = false;
        return true;
    }
    if (waited == VK_ERROR_DEVICE_LOST) {
        deviceLost = true;
        queueSubmitted = false;
        return true;
    }
    // Timeout or any other status: retirement is unproved. Keep queueSubmitted set so no caller
    // believes the generation is safe to destroy.
    return false;
}

GpuNeutralDisplay::Impl::~Impl() {
    if (state == nullptr) {
        return;
    }
    assert(owner == std::this_thread::get_id());
    // The owning GpuNeutralDisplay proved retirement before invoking this destructor; destroy the
    // idle buffers here. If retirement was not proved the owner releases (leaks) this Impl instead
    // of destroying it, so this body never runs against a queue-busy generation.
    destroyBuffers();
}

} // namespace bloom::render
