#include "gpu_affine_private.hpp"

#include "gpu_affine_fault.hpp"

#include <array>
#include <cstdint>
#include <string>

// Cohesive lazy pipeline/resource setup for GpuAffine, split out of gpu_affine.cpp so both
// translation units stay within the source-size budget. Reuses the generic CompositePipeline helper
// from gpu_composite_resources.cpp unchanged; the compact AffineBilinearV1 kernel and its O(1) map
// are untouched.

namespace bloom::render {

void GpuAffine::Impl::resetPipelineResources() noexcept {
    // Children before parents: a descriptor set frees through its pool and a command buffer frees
    // through its command pool, so both parent handles must outlive the child. Reset in reverse
    // declaration order to match the implicit member destruction order.
    fence = vk::raii::Fence{nullptr};
    commandBuffer = vk::raii::CommandBuffer{nullptr};
    commandPool = vk::raii::CommandPool{nullptr};
    affineSet = vk::raii::DescriptorSet{nullptr};
    descriptorPool = vk::raii::DescriptorPool{nullptr};
    affine = CompositePipeline{};
    pipelinesReady = false;
}

bool GpuAffine::Impl::ensureResidentReady() {
    if (!acquireResidentSlot()) {
        createDiagnostic =
            affineDiagnostic(GpuAffineDiagnosticCode::DeviceUnavailable,
                             "the bounded affine resident pool is full; no native resources were "
                             "allocated");
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

bool GpuAffine::Impl::createPipelines() {
    constexpr bool kAffineBindings[kAffineBindingCount] = {true, true, false, false};
    std::string reason;
    if (!createCompositePipeline(*control, vulkan_detail::kAffineBilinearSpirvCode,
                                 vulkan_detail::kAffineBilinearSpirvByteCount, kAffineBindings,
                                 kAffineBindingCount, kAffinePushBytes, reason, affine)) {
        createDiagnostic = affineDiagnostic(GpuAffineDiagnosticCode::ShaderRejected, reason);
        return false;
    }

    const std::array poolSizes{vk::DescriptorPoolSize{vk::DescriptorType::eStorageImage, 2},
                               vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 2}};
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
        createDiagnostic = affineDiagnostic(GpuAffineDiagnosticCode::AllocationFailed,
                                            "the affine descriptor pool could not be created");
        return false;
    }
    descriptorPool = vk::raii::DescriptorPool(control->device, rawPool);
    const vk::DescriptorSetLayout setLayout = *affine.descriptorSetLayout;
    vk::DescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.descriptorPool = *descriptorPool;
    allocateInfo.descriptorSetCount = 1;
    allocateInfo.pSetLayouts = &setLayout;
    VkDescriptorSet rawSet = VK_NULL_HANDLE;
    if (dispatcher->vkAllocateDescriptorSets(
            rawDevice, reinterpret_cast<const VkDescriptorSetAllocateInfo*>(&allocateInfo),
            &rawSet) != VK_SUCCESS) {
        createDiagnostic = affineDiagnostic(GpuAffineDiagnosticCode::AllocationFailed,
                                            "the affine descriptor set could not be allocated");
        return false;
    }
    affineSet = vk::raii::DescriptorSet(control->device, rawSet, *descriptorPool);

    vk::CommandPoolCreateInfo commandPoolInfo{};
    commandPoolInfo.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
    commandPoolInfo.queueFamilyIndex = control->computeQueueFamily;
    VkCommandPool rawCommandPool = VK_NULL_HANDLE;
    if (dispatcher->vkCreateCommandPool(
            rawDevice, reinterpret_cast<const VkCommandPoolCreateInfo*>(&commandPoolInfo), nullptr,
            &rawCommandPool) != VK_SUCCESS) {
        createDiagnostic = affineDiagnostic(GpuAffineDiagnosticCode::AllocationFailed,
                                            "the affine command pool could not be created");
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
        createDiagnostic = affineDiagnostic(GpuAffineDiagnosticCode::AllocationFailed,
                                            "the affine command buffer could not be allocated");
        return false;
    }
    commandBuffer = vk::raii::CommandBuffer(control->device, rawCommandBuffer, *commandPool);
    vk::FenceCreateInfo fenceInfo{};
    VkFence rawFence = VK_NULL_HANDLE;
    if (dispatcher->vkCreateFence(rawDevice, reinterpret_cast<const VkFenceCreateInfo*>(&fenceInfo),
                                  nullptr, &rawFence) != VK_SUCCESS) {
        createDiagnostic = affineDiagnostic(GpuAffineDiagnosticCode::AllocationFailed,
                                            "the affine fence could not be created");
        return false;
    }
    fence = vk::raii::Fence(control->device, rawFence);
    // TEST-ONLY fault: the full native set has been created, so the caller's cleanup must free
    // every child before its parent and the slot must be returned for a clean retry.
    if (static_cast<affine_detail::AffineRetirementFault>(
            affine_detail::affineRetirementFault().load()) ==
        affine_detail::AffineRetirementFault::FailPipelineCreation) {
        createDiagnostic = affineDiagnostic(GpuAffineDiagnosticCode::ShaderRejected,
                                            "injected affine pipeline creation failure");
        return false;
    }
    return true;
}

} // namespace bloom::render
