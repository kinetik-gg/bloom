#include "gpu_initialization_observation.hpp"
#include "gpu_solid_private.hpp"
#include "shaders/solid_spirv.inc"
#include <cassert>

namespace bloom::render {
namespace {
std::atomic<std::uint8_t> initializationFault{0};
std::atomic<std::uint64_t> initializationCreated{0};
std::atomic<std::uint64_t> initializationDestroyed{0};
std::atomic<std::uint64_t> initializationFaultHits{0};
std::atomic<std::uint64_t> initializationSlotReturns{0};

bool initializationFails(solid_detail::SolidInitializationFault boundary) noexcept {
    if (initializationFault.load() != static_cast<std::uint8_t>(boundary)) {
        return false;
    }
    initializationFaultHits.fetch_add(1);
    return true;
}

} // namespace

namespace solid_detail {
void recordSolidInitializationSlotReturn() noexcept { initializationSlotReturns.fetch_add(1); }
void setSolidInitializationFaultForTest(SolidInitializationFault fault) noexcept {
    initializationFault.store(static_cast<std::uint8_t>(fault));
}
SolidInitializationStats solidInitializationStats() noexcept {
    return {initializationCreated.load(), initializationDestroyed.load(),
            initializationFaultHits.load(), initializationSlotReturns.load()};
}
} // namespace solid_detail

namespace primitive_detail {
InitializationAllocationStats initializationAllocationStats(GpuDevice& device) {
    const auto control = GpuRendererAccess::state(device);
    assert(control != nullptr && control->owner == std::this_thread::get_id());
    VmaTotalStatistics statistics{};
    vmaCalculateStatistics(control->allocator, &statistics);
    return {statistics.total.statistics.allocationCount,
            statistics.total.statistics.allocationBytes};
}
} // namespace primitive_detail

void GpuSolid::Impl::resetPipelineResources() noexcept {
    // Free children through their still-live pools, matching reverse member destruction.
    checkerPipeline = vk::raii::Pipeline{nullptr};
    checkerPipelineLayout = vk::raii::PipelineLayout{nullptr};
    checkerShaderModule = vk::raii::ShaderModule{nullptr};
    checkerAttempted = false;
    if (*fence) {
        initializationDestroyed.fetch_add(1);
    }
    fence = vk::raii::Fence{nullptr};
    if (*commandBuffer) {
        initializationDestroyed.fetch_add(1);
    }
    commandBuffer = vk::raii::CommandBuffer{nullptr};
    if (*commandPool) {
        initializationDestroyed.fetch_add(1);
    }
    commandPool = vk::raii::CommandPool{nullptr};
    if (*descriptorSet) {
        initializationDestroyed.fetch_add(1);
    }
    descriptorSet = vk::raii::DescriptorSet{nullptr};
    if (*descriptorPool) {
        initializationDestroyed.fetch_add(1);
    }
    descriptorPool = vk::raii::DescriptorPool{nullptr};
    if (*pipeline) {
        initializationDestroyed.fetch_add(1);
    }
    pipeline = vk::raii::Pipeline{nullptr};
    if (*pipelineLayout) {
        initializationDestroyed.fetch_add(1);
    }
    pipelineLayout = vk::raii::PipelineLayout{nullptr};
    if (*descriptorSetLayout) {
        initializationDestroyed.fetch_add(1);
    }
    descriptorSetLayout = vk::raii::DescriptorSetLayout{nullptr};
    if (*shaderModule) {
        initializationDestroyed.fetch_add(1);
    }
    shaderModule = vk::raii::ShaderModule{nullptr};
    pipelineReady = false;
}

bool GpuSolid::Impl::createPipeline() {
    const VkDevice rawDevice = static_cast<VkDevice>(*control->device);
    const auto* dispatcher = control->device.getDispatcher();

    vk::ShaderModuleCreateInfo shaderInfo{};
    shaderInfo.codeSize = vulkan_detail::kSolidSpirvByteCount;
    shaderInfo.pCode = vulkan_detail::kSolidSpirvCode;
    VkShaderModule rawShader = VK_NULL_HANDLE;
    if (initializationFails(solid_detail::SolidInitializationFault::BeforeShaderModule)) {
        createDiagnostic =
            gpuSolidDiagnostic(GpuSolidDiagnosticCode::AllocationFailed,
                               "injected native initialization failure before ShaderModule");
        return false;
    }
    if (dispatcher->vkCreateShaderModule(
            rawDevice, reinterpret_cast<const VkShaderModuleCreateInfo*>(&shaderInfo), nullptr,
            &rawShader) != VK_SUCCESS) {
        createDiagnostic = gpuSolidDiagnostic(GpuSolidDiagnosticCode::ShaderRejected,
                                              "the embedded SolidV1 shader module was rejected");
        return false;
    }
    shaderModule = vk::raii::ShaderModule(control->device, rawShader);
    initializationCreated.fetch_add(1);

    vk::DescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = vk::DescriptorType::eStorageImage;
    binding.descriptorCount = 1;
    binding.stageFlags = vk::ShaderStageFlagBits::eCompute;
    vk::DescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    VkDescriptorSetLayout rawLayout = VK_NULL_HANDLE;
    if (initializationFails(solid_detail::SolidInitializationFault::BeforeDescriptorSetLayout)) {
        createDiagnostic =
            gpuSolidDiagnostic(GpuSolidDiagnosticCode::AllocationFailed,
                               "injected native initialization failure before DescriptorSetLayout");
        return false;
    }
    if (dispatcher->vkCreateDescriptorSetLayout(
            rawDevice, reinterpret_cast<const VkDescriptorSetLayoutCreateInfo*>(&layoutInfo),
            nullptr, &rawLayout) != VK_SUCCESS) {
        createDiagnostic = gpuSolidDiagnostic(GpuSolidDiagnosticCode::ShaderRejected,
                                              "the SolidV1 descriptor set layout was rejected");
        return false;
    }
    descriptorSetLayout = vk::raii::DescriptorSetLayout(control->device, rawLayout);
    initializationCreated.fetch_add(1);

    vk::PushConstantRange pushRange{};
    pushRange.stageFlags = vk::ShaderStageFlagBits::eCompute;
    pushRange.offset = 0;
    pushRange.size = static_cast<std::uint32_t>(sizeof(SolidPushConstants));
    const vk::DescriptorSetLayout setLayout = *descriptorSetLayout;
    vk::PipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &setLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;
    VkPipelineLayout rawPipelineLayout = VK_NULL_HANDLE;
    if (initializationFails(solid_detail::SolidInitializationFault::BeforePipelineLayout)) {
        createDiagnostic =
            gpuSolidDiagnostic(GpuSolidDiagnosticCode::AllocationFailed,
                               "injected native initialization failure before PipelineLayout");
        return false;
    }
    if (dispatcher->vkCreatePipelineLayout(
            rawDevice, reinterpret_cast<const VkPipelineLayoutCreateInfo*>(&pipelineLayoutInfo),
            nullptr, &rawPipelineLayout) != VK_SUCCESS) {
        createDiagnostic = gpuSolidDiagnostic(GpuSolidDiagnosticCode::ShaderRejected,
                                              "the SolidV1 pipeline layout was rejected");
        return false;
    }
    pipelineLayout = vk::raii::PipelineLayout(control->device, rawPipelineLayout);
    initializationCreated.fetch_add(1);

    vk::ComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.stage.stage = vk::ShaderStageFlagBits::eCompute;
    pipelineInfo.stage.module = *shaderModule;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = *pipelineLayout;
    VkPipeline rawPipeline = VK_NULL_HANDLE;
    if (initializationFails(solid_detail::SolidInitializationFault::BeforePipeline)) {
        createDiagnostic =
            gpuSolidDiagnostic(GpuSolidDiagnosticCode::AllocationFailed,
                               "injected native initialization failure before Pipeline");
        return false;
    }
    if (dispatcher->vkCreateComputePipelines(
            rawDevice, VK_NULL_HANDLE, 1,
            reinterpret_cast<const VkComputePipelineCreateInfo*>(&pipelineInfo), nullptr,
            &rawPipeline) != VK_SUCCESS) {
        createDiagnostic = gpuSolidDiagnostic(GpuSolidDiagnosticCode::ShaderRejected,
                                              "the embedded SolidV1 compute pipeline was rejected");
        return false;
    }
    pipeline = vk::raii::Pipeline(control->device, rawPipeline);
    initializationCreated.fetch_add(1);

    const vk::DescriptorPoolSize poolSize{vk::DescriptorType::eStorageImage, 1};
    vk::DescriptorPoolCreateInfo poolInfo{};
    poolInfo.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    VkDescriptorPool rawPool = VK_NULL_HANDLE;
    if (initializationFails(solid_detail::SolidInitializationFault::BeforeDescriptorPool)) {
        createDiagnostic =
            gpuSolidDiagnostic(GpuSolidDiagnosticCode::AllocationFailed,
                               "injected native initialization failure before DescriptorPool");
        return false;
    }
    if (dispatcher->vkCreateDescriptorPool(
            rawDevice, reinterpret_cast<const VkDescriptorPoolCreateInfo*>(&poolInfo), nullptr,
            &rawPool) != VK_SUCCESS) {
        createDiagnostic = gpuSolidDiagnostic(GpuSolidDiagnosticCode::AllocationFailed,
                                              "the SolidV1 descriptor pool could not be created");
        return false;
    }
    descriptorPool = vk::raii::DescriptorPool(control->device, rawPool);
    initializationCreated.fetch_add(1);

    vk::DescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.descriptorPool = *descriptorPool;
    allocateInfo.descriptorSetCount = 1;
    allocateInfo.pSetLayouts = &setLayout;
    VkDescriptorSet rawSet = VK_NULL_HANDLE;
    if (initializationFails(solid_detail::SolidInitializationFault::BeforeDescriptorSet)) {
        createDiagnostic =
            gpuSolidDiagnostic(GpuSolidDiagnosticCode::AllocationFailed,
                               "injected native initialization failure before DescriptorSet");
        return false;
    }
    if (dispatcher->vkAllocateDescriptorSets(
            rawDevice, reinterpret_cast<const VkDescriptorSetAllocateInfo*>(&allocateInfo),
            &rawSet) != VK_SUCCESS) {
        createDiagnostic = gpuSolidDiagnostic(GpuSolidDiagnosticCode::AllocationFailed,
                                              "the SolidV1 descriptor set could not be allocated");
        return false;
    }
    descriptorSet = vk::raii::DescriptorSet(control->device, rawSet, *descriptorPool);
    initializationCreated.fetch_add(1);

    vk::CommandPoolCreateInfo commandPoolInfo{};
    commandPoolInfo.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
    commandPoolInfo.queueFamilyIndex = control->computeQueueFamily;
    VkCommandPool rawCommandPool = VK_NULL_HANDLE;
    if (initializationFails(solid_detail::SolidInitializationFault::BeforeCommandPool)) {
        createDiagnostic =
            gpuSolidDiagnostic(GpuSolidDiagnosticCode::AllocationFailed,
                               "injected native initialization failure before CommandPool");
        return false;
    }
    if (dispatcher->vkCreateCommandPool(
            rawDevice, reinterpret_cast<const VkCommandPoolCreateInfo*>(&commandPoolInfo), nullptr,
            &rawCommandPool) != VK_SUCCESS) {
        createDiagnostic = gpuSolidDiagnostic(GpuSolidDiagnosticCode::AllocationFailed,
                                              "the SolidV1 command pool could not be created");
        return false;
    }
    commandPool = vk::raii::CommandPool(control->device, rawCommandPool);
    initializationCreated.fetch_add(1);

    vk::CommandBufferAllocateInfo commandBufferInfo{};
    commandBufferInfo.commandPool = *commandPool;
    commandBufferInfo.level = vk::CommandBufferLevel::ePrimary;
    commandBufferInfo.commandBufferCount = 1;
    VkCommandBuffer rawCommandBuffer = VK_NULL_HANDLE;
    if (initializationFails(solid_detail::SolidInitializationFault::BeforeCommandBuffer)) {
        createDiagnostic =
            gpuSolidDiagnostic(GpuSolidDiagnosticCode::AllocationFailed,
                               "injected native initialization failure before CommandBuffer");
        return false;
    }
    if (dispatcher->vkAllocateCommandBuffers(
            rawDevice, reinterpret_cast<const VkCommandBufferAllocateInfo*>(&commandBufferInfo),
            &rawCommandBuffer) != VK_SUCCESS) {
        createDiagnostic = gpuSolidDiagnostic(GpuSolidDiagnosticCode::AllocationFailed,
                                              "the SolidV1 command buffer could not be allocated");
        return false;
    }
    commandBuffer = vk::raii::CommandBuffer(control->device, rawCommandBuffer, *commandPool);
    initializationCreated.fetch_add(1);

    vk::FenceCreateInfo fenceInfo{};
    VkFence rawFence = VK_NULL_HANDLE;
    if (initializationFails(solid_detail::SolidInitializationFault::BeforeFence)) {
        createDiagnostic =
            gpuSolidDiagnostic(GpuSolidDiagnosticCode::AllocationFailed,
                               "injected native initialization failure before Fence");
        return false;
    }
    if (dispatcher->vkCreateFence(rawDevice, reinterpret_cast<const VkFenceCreateInfo*>(&fenceInfo),
                                  nullptr, &rawFence) != VK_SUCCESS) {
        createDiagnostic = gpuSolidDiagnostic(GpuSolidDiagnosticCode::AllocationFailed,
                                              "the SolidV1 fence could not be created");
        return false;
    }
    fence = vk::raii::Fence(control->device, rawFence);
    initializationCreated.fetch_add(1);
    if (initializationFails(solid_detail::SolidInitializationFault::Complete)) {
        createDiagnostic =
            gpuSolidDiagnostic(GpuSolidDiagnosticCode::AllocationFailed,
                               "injected native initialization failure after complete setup");
        return false;
    }
    return true;
}

} // namespace bloom::render
