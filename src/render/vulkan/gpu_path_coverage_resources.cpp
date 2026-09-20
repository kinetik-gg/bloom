#include "gpu_path_coverage_private.hpp"

#include "shaders/path_coverage_spirv.inc"

#include <array>
#include <cstdint>
#include <string>

// GpuPathCoverage pipeline creation, split out of gpu_path_coverage.cpp to keep
// each translation unit under the file budget. It owns no job state; it builds
// the cached shader module, descriptor layout/pool/set, pipeline layout, and
// pipeline on the device owner thread. The per-job command pool/buffer and fence
// are created in begin() so an unproven submission can be moved whole into the
// bounded quarantine.

namespace bloom::render {

bool GpuPathCoverageImpl::createPipeline() {
    const VkDevice rawDevice = static_cast<VkDevice>(*control->device);
    const auto* dispatcher = control->device.getDispatcher();
    // Private device fact for the flattened 2D dispatch grid.
    maxWorkGroupCountY = control->physicalDevice.getProperties().limits.maxComputeWorkGroupCount[1];

    vk::ShaderModuleCreateInfo shaderInfo{};
    shaderInfo.codeSize = vulkan_detail::kPathCoverageSpirvByteCount;
    shaderInfo.pCode = vulkan_detail::kPathCoverageSpirvCode;
    VkShaderModule rawShader = VK_NULL_HANDLE;
    if (dispatcher->vkCreateShaderModule(
            rawDevice, reinterpret_cast<const VkShaderModuleCreateInfo*>(&shaderInfo), nullptr,
            &rawShader) != VK_SUCCESS) {
        createDiagnostic =
            {GpuPathCoverageDiagnosticCode::ShaderRejected,
             "the embedded GpuPathCoverage shader module was rejected"};
        return false;
    }
    shaderModule = vk::raii::ShaderModule(control->device, rawShader);

    std::array<vk::DescriptorSetLayoutBinding, 3> bindings{};
    for (std::uint32_t index = 0; index < bindings.size(); ++index) {
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
        createDiagnostic =
            {GpuPathCoverageDiagnosticCode::ShaderRejected,
             "the GpuPathCoverage descriptor set layout was rejected"};
        return false;
    }
    descriptorSetLayout = vk::raii::DescriptorSetLayout(control->device, rawLayout);

    vk::PushConstantRange pushRange{};
    pushRange.stageFlags = vk::ShaderStageFlagBits::eCompute;
    pushRange.offset = 0;
    pushRange.size = static_cast<std::uint32_t>(sizeof(GpuPathCoveragePushConstants));
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
        createDiagnostic =
            {GpuPathCoverageDiagnosticCode::ShaderRejected,
             "the GpuPathCoverage pipeline layout was rejected"};
        return false;
    }
    pipelineLayout = vk::raii::PipelineLayout(control->device, rawPipelineLayout);

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
        createDiagnostic =
            {GpuPathCoverageDiagnosticCode::ShaderRejected,
             "the embedded GpuPathCoverage compute pipeline was rejected"};
        return false;
    }
    pipeline = vk::raii::Pipeline(control->device, rawPipeline);

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
        createDiagnostic = {GpuPathCoverageDiagnosticCode::AllocationFailed,
                            "the GpuPathCoverage descriptor pool could not be created"};
        return false;
    }
    descriptorPool = vk::raii::DescriptorPool(control->device, rawPool);

    vk::DescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.descriptorPool = *descriptorPool;
    allocateInfo.descriptorSetCount = 1;
    allocateInfo.pSetLayouts = &setLayout;
    VkDescriptorSet rawSet = VK_NULL_HANDLE;
    if (dispatcher->vkAllocateDescriptorSets(
            rawDevice, reinterpret_cast<const VkDescriptorSetAllocateInfo*>(&allocateInfo),
            &rawSet) != VK_SUCCESS) {
        createDiagnostic = {GpuPathCoverageDiagnosticCode::AllocationFailed,
                            "the GpuPathCoverage descriptor set could not be allocated"};
        return false;
    }
    descriptorSet = vk::raii::DescriptorSet(control->device, rawSet, *descriptorPool);
    return true;
}


} // namespace bloom::render
