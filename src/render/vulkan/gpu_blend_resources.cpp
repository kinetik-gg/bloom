#include "gpu_blend_private.hpp"

#include "gpu_blend_fault.hpp"

#include <array>
#include <cstdint>
#include <string>

// Cohesive lazy pipeline/resource setup for GpuBlend, split out of gpu_blend.cpp so both
// translation units stay within the source-size budget. Reuses the generic CompositePipeline helper
// from gpu_composite_resources.cpp unchanged; it introduces no second pipeline abstraction.

namespace bloom::render {

void GpuBlend::Impl::resetPipelineResources() noexcept {
    // Children before parents: a descriptor set frees through its pool and a command buffer frees
    // through its command pool, so both parent handles must outlive the child. Reset in reverse
    // declaration order to match the implicit member destruction order.
    fence = vk::raii::Fence{nullptr};
    commandBuffer = vk::raii::CommandBuffer{nullptr};
    commandPool = vk::raii::CommandPool{nullptr};
    descriptorSet = vk::raii::DescriptorSet{nullptr};
    descriptorPool = vk::raii::DescriptorPool{nullptr};
    pipelinePortable = CompositePipeline{};
    pipelineF64 = CompositePipeline{};
    pipeline = CompositePipeline{};
    pipelinesReady = false;
}

bool GpuBlend::Impl::ensureResidentReady() {
    if (!acquireResidentSlot()) {
        createDiagnostic =
            blendDiagnostic(GpuBlendDiagnosticCode::DeviceUnavailable,
                            "the bounded blend resident pool is full; no native resources were "
                            "allocated");
        return false;
    }
    if (!pipelinesReady) {
        if (!createPipeline()) {
            resetPipelineResources();
            releaseResidentSlot();
            return false;
        }
        pipelinesReady = true;
    }
    return true;
}

bool GpuBlend::Impl::createPipeline() {
    constexpr bool kBlendBindings[kBlendBindingCount] = {true, true, true, false};
    std::string reason;
    if (!createCompositePipeline(*control, vulkan_detail::kBlendSpirvCode,
                                 vulkan_detail::kBlendSpirvByteCount, kBlendBindings,
                                 kBlendBindingCount, kBlendPushBytes, reason, pipeline)) {
        createDiagnostic = blendDiagnostic(GpuBlendDiagnosticCode::ShaderRejected, reason);
        return false;
    }
    // The portable compensated-Float32 kernel needs no Float64 capability and no 64-bit integer
    // type, so it is built unconditionally and backs the six general modes on every device that
    // lacks the exact Float64 companion.
    if (!createCompositePipeline(*control, vulkan_detail::kBlendPortableSpirvCode,
                                 vulkan_detail::kBlendPortableSpirvByteCount, kBlendBindings,
                                 kBlendBindingCount, kBlendPushBytes, reason, pipelinePortable)) {
        createDiagnostic = blendDiagnostic(GpuBlendDiagnosticCode::ShaderRejected, reason);
        return false;
    }
    // The exact Float64 companion is built only when the device advertised and enabled the core
    // shaderFloat64 feature AND the caller's policy allows it. The SPIR-V requires that capability,
    // so it must not be created otherwise. If the device claims support but rejects the pipeline,
    // creation fails closed rather than silently losing the exactness the general modes need.
    // `generalUsesF64` is resolved once in create() (so shaderIdentity is stable before the first
    // begin) and is deliberately not reset by a failed creation.
    if (generalUsesF64) {
        if (!createCompositePipeline(*control, vulkan_detail::kBlendF64SpirvCode,
                                     vulkan_detail::kBlendF64SpirvByteCount, kBlendBindings,
                                     kBlendBindingCount, kBlendPushBytes, reason, pipelineF64)) {
            createDiagnostic = blendDiagnostic(GpuBlendDiagnosticCode::ShaderRejected, reason);
            return false;
        }
    }

    const std::array poolSizes{vk::DescriptorPoolSize{vk::DescriptorType::eStorageImage, 3},
                               vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 1}};
    vk::DescriptorPoolCreateInfo poolInfo{};
    poolInfo.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = static_cast<std::uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    const VkDevice rawDevice = static_cast<VkDevice>(*control->device);
    const auto* dispatcher = control->device.getDispatcher();
    VkDescriptorPool rawPool = VK_NULL_HANDLE;
    if (dispatcher->vkCreateDescriptorPool(
            rawDevice, reinterpret_cast<const VkDescriptorPoolCreateInfo*>(&poolInfo), nullptr,
            &rawPool) != VK_SUCCESS) {
        createDiagnostic = blendDiagnostic(GpuBlendDiagnosticCode::AllocationFailed,
                                           "the blend descriptor pool could not be created");
        return false;
    }
    descriptorPool = vk::raii::DescriptorPool(control->device, rawPool);
    const std::array setLayouts{*pipeline.descriptorSetLayout};
    vk::DescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.descriptorPool = *descriptorPool;
    allocateInfo.descriptorSetCount = 1;
    allocateInfo.pSetLayouts = setLayouts.data();
    VkDescriptorSet rawSet = VK_NULL_HANDLE;
    if (dispatcher->vkAllocateDescriptorSets(
            rawDevice, reinterpret_cast<const VkDescriptorSetAllocateInfo*>(&allocateInfo),
            &rawSet) != VK_SUCCESS) {
        createDiagnostic = blendDiagnostic(GpuBlendDiagnosticCode::AllocationFailed,
                                           "the blend descriptor set could not be allocated");
        return false;
    }
    descriptorSet = vk::raii::DescriptorSet(control->device, rawSet, *descriptorPool);

    vk::CommandPoolCreateInfo commandPoolInfo{};
    commandPoolInfo.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
    commandPoolInfo.queueFamilyIndex = control->computeQueueFamily;
    VkCommandPool rawCommandPool = VK_NULL_HANDLE;
    if (dispatcher->vkCreateCommandPool(
            rawDevice, reinterpret_cast<const VkCommandPoolCreateInfo*>(&commandPoolInfo), nullptr,
            &rawCommandPool) != VK_SUCCESS) {
        createDiagnostic = blendDiagnostic(GpuBlendDiagnosticCode::AllocationFailed,
                                           "the blend command pool could not be created");
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
        createDiagnostic = blendDiagnostic(GpuBlendDiagnosticCode::AllocationFailed,
                                           "the blend command buffer could not be allocated");
        return false;
    }
    commandBuffer = vk::raii::CommandBuffer(control->device, rawCommandBuffer, *commandPool);
    vk::FenceCreateInfo fenceInfo{};
    VkFence rawFence = VK_NULL_HANDLE;
    if (dispatcher->vkCreateFence(rawDevice, reinterpret_cast<const VkFenceCreateInfo*>(&fenceInfo),
                                  nullptr, &rawFence) != VK_SUCCESS) {
        createDiagnostic = blendDiagnostic(GpuBlendDiagnosticCode::AllocationFailed,
                                           "the blend fence could not be created");
        return false;
    }
    fence = vk::raii::Fence(control->device, rawFence);
    // TEST-ONLY fault: the full native set has been created, so the caller's cleanup must free
    // every child before its parent and the slot must be returned for a clean retry.
    if (static_cast<blend_detail::BlendRetirementFault>(
            blend_detail::blendRetirementFault().load()) ==
        blend_detail::BlendRetirementFault::FailPipelineCreation) {
        createDiagnostic = blendDiagnostic(GpuBlendDiagnosticCode::ShaderRejected,
                                           "injected blend pipeline creation failure");
        return false;
    }
    return true;
}

} // namespace bloom::render
