#include "gpu_solid_private.hpp"
#include "shaders/checker_spirv.inc"

namespace bloom::render {

bool GpuSolid::Impl::createCheckerPipeline() {
    if (*checkerPipeline)
        return true;
    if (checkerAttempted)
        return false;
    checkerAttempted = true;
    const VkDevice device = static_cast<VkDevice>(*control->device);
    const auto* dispatcher = control->device.getDispatcher();
    vk::ShaderModuleCreateInfo shaderInfo{};
    shaderInfo.codeSize = vulkan_detail::kCheckerSpirvByteCount;
    shaderInfo.pCode = vulkan_detail::kCheckerSpirvCode;
    VkShaderModule shader = VK_NULL_HANDLE;
    if (dispatcher->vkCreateShaderModule(
            device, reinterpret_cast<const VkShaderModuleCreateInfo*>(&shaderInfo), nullptr,
            &shader) != VK_SUCCESS) {
        createDiagnostic = gpuSolidDiagnostic(GpuSolidDiagnosticCode::ShaderRejected,
                                             "the pinned CheckerV1 shader was rejected");
        return false;
    }
    checkerShaderModule = vk::raii::ShaderModule(control->device, shader);
    const auto setLayout = *descriptorSetLayout;
    vk::PushConstantRange push{};
    push.stageFlags = vk::ShaderStageFlagBits::eCompute;
    push.offset = 0;
    push.size = sizeof(CheckerPushConstants);
    vk::PipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &push;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (dispatcher->vkCreatePipelineLayout(
            device, reinterpret_cast<const VkPipelineLayoutCreateInfo*>(&layoutInfo), nullptr,
            &layout) != VK_SUCCESS) {
        createDiagnostic = gpuSolidDiagnostic(GpuSolidDiagnosticCode::ShaderRejected,
                                             "the CheckerV1 push-constant layout was rejected");
        return false;
    }
    checkerPipelineLayout = vk::raii::PipelineLayout(control->device, layout);
    vk::ComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.stage.stage = vk::ShaderStageFlagBits::eCompute;
    pipelineInfo.stage.module = *checkerShaderModule;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = *checkerPipelineLayout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (dispatcher->vkCreateComputePipelines(
            device, VK_NULL_HANDLE, 1,
            reinterpret_cast<const VkComputePipelineCreateInfo*>(&pipelineInfo), nullptr,
            &pipeline) != VK_SUCCESS) {
        createDiagnostic = gpuSolidDiagnostic(GpuSolidDiagnosticCode::ShaderRejected,
                                             "the CheckerV1 compute pipeline was rejected");
        return false;
    }
    checkerPipeline = vk::raii::Pipeline(control->device, pipeline);
    return true;
}

} // namespace bloom::render
