#include "gpu_ocio_program_private.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace bloom::render {
namespace {

using vulkan_detail::DeviceAllocatorState;

struct BloomOcioPush final {
    std::uint32_t pixelCount;
    std::uint32_t width;
    std::uint32_t height;
};
static_assert(sizeof(BloomOcioPush) == 12);

// Portable unsigned checked arithmetic (no compiler builtins), so MSVC/Windows builds compile
// unchanged. `out` is only written when the operation cannot overflow.
[[nodiscard]] bool addChecked(const std::uint64_t a, const std::uint64_t b,
                              std::uint64_t& out) noexcept {
    if (a > std::numeric_limits<std::uint64_t>::max() - b) {
        return false;
    }
    out = a + b;
    return true;
}
[[nodiscard]] bool mulChecked(const std::uint64_t a, const std::uint64_t b,
                              std::uint64_t& out) noexcept {
    if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a) {
        return false;
    }
    out = a * b;
    return true;
}
[[nodiscard]] bool cancellationRequested(const GpuOcioProgramCancellation& cancellation) noexcept {
    if (!cancellation) {
        return false;
    }
    try {
        return cancellation();
    } catch (...) {
        // A throwing callback is treated as a cancellation request; after submission this retains
        // resources instead of unwinding into a destructive path.
        return true;
    }
}

[[nodiscard]] bool texelCount(const OcioGpuTextureDesc& texture, std::uint64_t& out) noexcept {
    switch (texture.dimensions) {
    case OcioGpuTextureDimensions::OneD:
        out = texture.width;
        return true;
    case OcioGpuTextureDimensions::TwoD:
        return mulChecked(texture.width, texture.height, out);
    case OcioGpuTextureDimensions::ThreeD:
        return mulChecked(texture.edgeLength, texture.edgeLength, out) &&
               mulChecked(out, texture.edgeLength, out);
    }
    return false;
}

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

[[nodiscard]] bool createStatusBuffer(DeviceAllocatorState& state, ResidentBuffer& out) {
    return createResidentBuffer(
        state, ocio_program_detail::kStatusBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT, true, out);
}

} // namespace

bool GpuOcioProgram::Impl::createOcioResources() {
    std::uint64_t lutBytes = 0;
    retainedResourceBytes = 0;
    for (const auto& texture : desc.textures) {
        if (cancellationRequested(cancellation)) {
            createDiagnostic = makeDiagnostic(GpuOcioProgramDiagnosticCode::Cancelled,
                                              "OCIO GPU program creation was cancelled");
            return false;
        }
        const std::uint64_t sampleCount = texture.samples.size();
        if (sampleCount > std::numeric_limits<std::uint64_t>::max() / sizeof(float)) {
            createDiagnostic = makeDiagnostic(GpuOcioProgramDiagnosticCode::OverBudget,
                                              "a LUT sample byte count overflows");
            return false;
        }
        const std::uint64_t sampleBytes = sampleCount * sizeof(float);
        if (sampleBytes > budgets.maxLutBytes || lutBytes > budgets.maxLutBytes - sampleBytes) {
            createDiagnostic = makeDiagnostic(GpuOcioProgramDiagnosticCode::OverBudget,
                                              "the aggregate LUT bytes exceed the budget");
            return false;
        }
        lutBytes += sampleBytes;
        std::uint64_t texels = 0;
        std::uint64_t imageBytes = 0;
        std::uint64_t transientPeak = 0;
        if (!texelCount(texture, texels) || !mulChecked(texels, 16U, imageBytes) ||
            !mulChecked(imageBytes, 2U, transientPeak)) {
            createDiagnostic = makeDiagnostic(GpuOcioProgramDiagnosticCode::OverBudget,
                                              "a LUT texture byte count overflows");
            return false;
        }
        // Guard BEFORE any allocation: retained + status + worst-case (RGBA image + staging) peak
        // must fit the owned-byte ceiling. Every subtraction is guarded against underflow.
        std::uint64_t required = 0;
        if (!addChecked(transientPeak, ocio_program_detail::kStatusBytes, required) ||
            retainedResourceBytes > budgets.maxOwnedBytes ||
            required > budgets.maxOwnedBytes - retainedResourceBytes) {
            createDiagnostic = makeDiagnostic(GpuOcioProgramDiagnosticCode::OverBudget,
                                              "the upload peak exceeds the owned-byte budget");
            return false;
        }
        const std::uint64_t remaining = retainedResourceBytes > budgets.maxOwnedBytes
                                            ? 0U
                                            : budgets.maxOwnedBytes - retainedResourceBytes;
        ocio_program_detail::SampledResource resource;
        const auto outcome = ocio_program_detail::createSampledResource(control, texture, remaining,
                                                                        cancellation, resource);
        if (outcome == ocio_program_detail::UploadOutcome::OverBudget) {
            createDiagnostic = makeDiagnostic(GpuOcioProgramDiagnosticCode::OverBudget,
                                              "the actual LUT allocation exceeds the budget");
            return false;
        }
        if (outcome == ocio_program_detail::UploadOutcome::Cancelled) {
            createDiagnostic = makeDiagnostic(GpuOcioProgramDiagnosticCode::Cancelled,
                                              "the LUT upload was cancelled after submission");
            return false;
        }
        if (outcome == ocio_program_detail::UploadOutcome::Quarantined) {
            createDiagnostic = makeDiagnostic(GpuOcioProgramDiagnosticCode::DeviceUnavailable,
                                              "the LUT upload could not be proven retired");
            return false;
        }
        if (outcome != ocio_program_detail::UploadOutcome::Retired) {
            createDiagnostic =
                makeDiagnostic(GpuOcioProgramDiagnosticCode::Unsupported,
                               "the LUT texture format is unsupported on this device");
            return false;
        }
        textures.push_back(resource);
        std::uint64_t updated = 0;
        if (!addChecked(retainedResourceBytes, resource.allocationBytes, updated) ||
            updated > budgets.maxOwnedBytes) {
            createDiagnostic = makeDiagnostic(GpuOcioProgramDiagnosticCode::OverBudget,
                                              "the retained OCIO resources exceed the budget");
            return false;
        }
        retainedResourceBytes = updated;
    }
    if (desc.uniformBufferSize > 0) {
        if (retainedResourceBytes > budgets.maxOwnedBytes ||
            desc.uniformBufferSize > budgets.maxOwnedBytes - retainedResourceBytes) {
            createDiagnostic = makeDiagnostic(GpuOcioProgramDiagnosticCode::OverBudget,
                                              "the uniform buffer exceeds the owned-byte budget");
            return false;
        }
        if (!createResidentBuffer(
                *control, desc.uniformBufferSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                true, uniformBuffer) ||
            uniformBuffer.info.pMappedData == nullptr) {
            createDiagnostic = makeDiagnostic(GpuOcioProgramDiagnosticCode::AllocationFailed,
                                              "the OCIO uniform buffer could not be created");
            return false;
        }
        uniformMapped = static_cast<std::uint8_t*>(uniformBuffer.info.pMappedData);
        std::uint64_t updated = 0;
        if (!addChecked(retainedResourceBytes, uniformBuffer.info.size, updated) ||
            updated > budgets.maxOwnedBytes) {
            createDiagnostic = makeDiagnostic(GpuOcioProgramDiagnosticCode::OverBudget,
                                              "the retained OCIO resources exceed the budget");
            return false;
        }
        retainedResourceBytes = updated;
    }
    if (retainedResourceBytes > budgets.maxOwnedBytes ||
        ocio_program_detail::kStatusBytes > budgets.maxOwnedBytes - retainedResourceBytes) {
        createDiagnostic = makeDiagnostic(GpuOcioProgramDiagnosticCode::OverBudget,
                                          "the status buffer exceeds the owned-byte budget");
        return false;
    }
    if (!createStatusBuffer(*control, status)) {
        createDiagnostic = makeDiagnostic(GpuOcioProgramDiagnosticCode::AllocationFailed,
                                          "the status buffer could not be created");
        return false;
    }
    statusMapped = static_cast<std::uint8_t*>(status.info.pMappedData);
    if (statusMapped == nullptr) {
        createDiagnostic = makeDiagnostic(GpuOcioProgramDiagnosticCode::AllocationFailed,
                                          "the status buffer is not host mapped");
        return false;
    }
    std::uint64_t statusUpdated = 0;
    if (!addChecked(retainedResourceBytes, status.info.size, statusUpdated) ||
        statusUpdated > budgets.maxOwnedBytes) {
        createDiagnostic = makeDiagnostic(GpuOcioProgramDiagnosticCode::OverBudget,
                                          "the retained OCIO resources exceed the budget");
        return false;
    }
    retainedResourceBytes = statusUpdated;
    if (retainedResourceBytes > budgets.maxOwnedBytes) {
        createDiagnostic = makeDiagnostic(GpuOcioProgramDiagnosticCode::OverBudget,
                                          "the retained OCIO resources exceed the budget");
        return false;
    }
    return true;
}

bool GpuOcioProgram::Impl::createPipeline() {
    const VkDevice device = static_cast<VkDevice>(*control->device);
    const auto* dispatcher = control->device.getDispatcher();

    vk::ShaderModuleCreateInfo shaderInfo{};
    shaderInfo.codeSize = spirv.size() * sizeof(std::uint32_t);
    shaderInfo.pCode = spirv.data();
    VkShaderModule rawShader = VK_NULL_HANDLE;
    if (dispatcher->vkCreateShaderModule(
            device, reinterpret_cast<const VkShaderModuleCreateInfo*>(&shaderInfo), nullptr,
            &rawShader) != VK_SUCCESS) {
        createDiagnostic =
            makeDiagnostic(GpuOcioProgramDiagnosticCode::ShaderRejected, "SPIR-V was rejected");
        return false;
    }
    shaderModule = vk::raii::ShaderModule(control->device, rawShader);

    std::vector<vk::DescriptorSetLayoutBinding> ocioBindings;
    if (desc.uniformBufferSize > 0) {
        vk::DescriptorSetLayoutBinding binding;
        binding.binding = 0;
        binding.descriptorType = vk::DescriptorType::eUniformBuffer;
        binding.descriptorCount = 1;
        binding.stageFlags = vk::ShaderStageFlagBits::eCompute;
        ocioBindings.push_back(binding);
    }
    for (const auto& texture : textures) {
        vk::DescriptorSetLayoutBinding binding;
        binding.binding = texture.binding;
        binding.descriptorType = vk::DescriptorType::eCombinedImageSampler;
        binding.descriptorCount = 1;
        binding.stageFlags = vk::ShaderStageFlagBits::eCompute;
        ocioBindings.push_back(binding);
    }
    vk::DescriptorSetLayoutCreateInfo ocioLayoutInfo{};
    ocioLayoutInfo.bindingCount = static_cast<std::uint32_t>(ocioBindings.size());
    ocioLayoutInfo.pBindings = ocioBindings.data();
    VkDescriptorSetLayout rawOcioLayout = VK_NULL_HANDLE;
    if (dispatcher->vkCreateDescriptorSetLayout(
            device, reinterpret_cast<const VkDescriptorSetLayoutCreateInfo*>(&ocioLayoutInfo),
            nullptr, &rawOcioLayout) != VK_SUCCESS) {
        createDiagnostic = makeDiagnostic(GpuOcioProgramDiagnosticCode::AllocationFailed,
                                          "the OCIO descriptor layout was rejected");
        return false;
    }
    ocioSetLayout = vk::raii::DescriptorSetLayout(control->device, rawOcioLayout);

    std::array<vk::DescriptorSetLayoutBinding, 3> ioBindings{};
    for (std::uint32_t index = 0; index < ioBindings.size(); ++index) {
        ioBindings[index].binding = index;
        ioBindings[index].descriptorCount = 1;
        ioBindings[index].stageFlags = vk::ShaderStageFlagBits::eCompute;
    }
    ioBindings[0].descriptorType = vk::DescriptorType::eStorageImage;
    ioBindings[1].descriptorType =
        displayArm ? vk::DescriptorType::eStorageBuffer : vk::DescriptorType::eStorageImage;
    ioBindings[2].descriptorType = vk::DescriptorType::eStorageBuffer;
    vk::DescriptorSetLayoutCreateInfo ioLayoutInfo{};
    ioLayoutInfo.bindingCount = static_cast<std::uint32_t>(ioBindings.size());
    ioLayoutInfo.pBindings = ioBindings.data();
    VkDescriptorSetLayout rawIoLayout = VK_NULL_HANDLE;
    if (dispatcher->vkCreateDescriptorSetLayout(
            device, reinterpret_cast<const VkDescriptorSetLayoutCreateInfo*>(&ioLayoutInfo),
            nullptr, &rawIoLayout) != VK_SUCCESS) {
        createDiagnostic = makeDiagnostic(GpuOcioProgramDiagnosticCode::AllocationFailed,
                                          "the Bloom I/O descriptor layout was rejected");
        return false;
    }
    ioSetLayout = vk::raii::DescriptorSetLayout(control->device, rawIoLayout);

    const std::array<vk::DescriptorSetLayout, 2> layouts{*ocioSetLayout, *ioSetLayout};
    vk::PushConstantRange pushRange{};
    pushRange.stageFlags = vk::ShaderStageFlagBits::eCompute;
    pushRange.offset = 0;
    pushRange.size = static_cast<std::uint32_t>(sizeof(BloomOcioPush));
    vk::PipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.setLayoutCount = static_cast<std::uint32_t>(layouts.size());
    pipelineLayoutInfo.pSetLayouts = layouts.data();
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;
    VkPipelineLayout rawPipelineLayout = VK_NULL_HANDLE;
    if (dispatcher->vkCreatePipelineLayout(
            device, reinterpret_cast<const VkPipelineLayoutCreateInfo*>(&pipelineLayoutInfo),
            nullptr, &rawPipelineLayout) != VK_SUCCESS) {
        createDiagnostic = makeDiagnostic(GpuOcioProgramDiagnosticCode::AllocationFailed,
                                          "the pipeline layout was rejected");
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
            device, VK_NULL_HANDLE, 1,
            reinterpret_cast<const VkComputePipelineCreateInfo*>(&pipelineInfo), nullptr,
            &rawPipeline) != VK_SUCCESS) {
        createDiagnostic = makeDiagnostic(GpuOcioProgramDiagnosticCode::ShaderRejected,
                                          "the compute pipeline was rejected");
        return false;
    }
    pipeline = vk::raii::Pipeline(control->device, rawPipeline);

    std::array<vk::DescriptorPoolSize, 4> poolSizes{};
    std::uint32_t poolCount = 0;
    const auto addPool = [&](const vk::DescriptorType type, const std::uint32_t count) {
        if (count != 0) {
            poolSizes[poolCount] = vk::DescriptorPoolSize{type, count};
            ++poolCount;
        }
    };
    addPool(vk::DescriptorType::eUniformBuffer, desc.uniformBufferSize > 0 ? 1U : 0U);
    addPool(vk::DescriptorType::eCombinedImageSampler, static_cast<std::uint32_t>(textures.size()));
    addPool(vk::DescriptorType::eStorageImage, displayArm ? 1U : 2U);
    addPool(vk::DescriptorType::eStorageBuffer, displayArm ? 2U : 1U);
    vk::DescriptorPoolCreateInfo poolInfo{};
    poolInfo.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    poolInfo.maxSets = 2;
    poolInfo.poolSizeCount = poolCount;
    poolInfo.pPoolSizes = poolSizes.data();
    VkDescriptorPool rawPool = VK_NULL_HANDLE;
    if (dispatcher->vkCreateDescriptorPool(
            device, reinterpret_cast<const VkDescriptorPoolCreateInfo*>(&poolInfo), nullptr,
            &rawPool) != VK_SUCCESS) {
        createDiagnostic = makeDiagnostic(GpuOcioProgramDiagnosticCode::AllocationFailed,
                                          "the descriptor pool could not be created");
        return false;
    }
    descriptorPool = vk::raii::DescriptorPool(control->device, rawPool);

    const std::array<vk::DescriptorSetLayout, 2> allocateLayouts{*ocioSetLayout, *ioSetLayout};
    VkDescriptorSet rawSets[2]{VK_NULL_HANDLE, VK_NULL_HANDLE};
    vk::DescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.descriptorPool = *descriptorPool;
    allocateInfo.descriptorSetCount = 2;
    allocateInfo.pSetLayouts = allocateLayouts.data();
    if (dispatcher->vkAllocateDescriptorSets(
            device, reinterpret_cast<const VkDescriptorSetAllocateInfo*>(&allocateInfo), rawSets) !=
        VK_SUCCESS) {
        createDiagnostic = makeDiagnostic(GpuOcioProgramDiagnosticCode::AllocationFailed,
                                          "the descriptor sets could not be allocated");
        return false;
    }
    ocioSet = vk::raii::DescriptorSet(control->device, rawSets[0], *descriptorPool);
    ioSet = vk::raii::DescriptorSet(control->device, rawSets[1], *descriptorPool);

    vk::CommandPoolCreateInfo commandPoolInfo{};
    commandPoolInfo.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
    commandPoolInfo.queueFamilyIndex = control->computeQueueFamily;
    VkCommandPool rawCommandPool = VK_NULL_HANDLE;
    if (dispatcher->vkCreateCommandPool(
            device, reinterpret_cast<const VkCommandPoolCreateInfo*>(&commandPoolInfo), nullptr,
            &rawCommandPool) != VK_SUCCESS) {
        createDiagnostic = makeDiagnostic(GpuOcioProgramDiagnosticCode::AllocationFailed,
                                          "the command pool could not be created");
        return false;
    }
    commandPool = vk::raii::CommandPool(control->device, rawCommandPool);
    vk::CommandBufferAllocateInfo commandBufferInfo{};
    commandBufferInfo.commandPool = *commandPool;
    commandBufferInfo.level = vk::CommandBufferLevel::ePrimary;
    commandBufferInfo.commandBufferCount = 1;
    VkCommandBuffer rawCommandBuffer = VK_NULL_HANDLE;
    if (dispatcher->vkAllocateCommandBuffers(
            device, reinterpret_cast<const VkCommandBufferAllocateInfo*>(&commandBufferInfo),
            &rawCommandBuffer) != VK_SUCCESS) {
        createDiagnostic = makeDiagnostic(GpuOcioProgramDiagnosticCode::AllocationFailed,
                                          "the command buffer could not be allocated");
        return false;
    }
    commandBuffer = vk::raii::CommandBuffer(control->device, rawCommandBuffer, *commandPool);
    vk::FenceCreateInfo fenceInfo{};
    VkFence rawFence = VK_NULL_HANDLE;
    if (dispatcher->vkCreateFence(device, reinterpret_cast<const VkFenceCreateInfo*>(&fenceInfo),
                                  nullptr, &rawFence) != VK_SUCCESS) {
        createDiagnostic = makeDiagnostic(GpuOcioProgramDiagnosticCode::AllocationFailed,
                                          "the fence could not be created");
        return false;
    }
    fence = vk::raii::Fence(control->device, rawFence);
    return true;
}

void GpuOcioProgram::Impl::writeStaticDescriptors() {
    if (desc.uniformBufferSize > 0) {
        ocio_program_detail::writeUniformDescriptor(ocioSet, *control, 0, uniformBuffer.buffer,
                                                    uniformBuffer.bytes);
    }
    for (const auto& texture : textures) {
        ocio_program_detail::writeCombinedDescriptor(ocioSet, *control, texture.binding,
                                                     texture.sampler, texture.view);
    }
    ocio_program_detail::writeBufferDescriptor(ioSet, *control, 2, status.buffer, status.bytes);
    if (displayArm) {
        // The packed destination buffer is written and sized by beginDisplay; its descriptor is
        // refreshed there because the buffer is created on first use.
    }
}

GpuOcioProgramCreateResult GpuOcioProgram::create(GpuDevice& device, OcioGpuProgramDesc program,
                                                  std::span<const std::uint32_t> spirv,
                                                  const GpuOcioProgramBudgets& budgets,
                                                  GpuOcioProgramCancellation cancellation) {
    if (device.state() != GpuDeviceState::Ready) {
        return {nullptr, makeDiagnostic(GpuOcioProgramDiagnosticCode::DeviceUnavailable,
                                        "the GPU device is not Ready")};
    }
    if (GpuRendererAccess::owner(device) != std::this_thread::get_id()) {
        return {nullptr, makeDiagnostic(GpuOcioProgramDiagnosticCode::WrongThread,
                                        "the OCIO program must be created on the device owner "
                                        "thread")};
    }
    auto state = GpuRendererAccess::state(device);
    if (state == nullptr) {
        return {nullptr, makeDiagnostic(GpuOcioProgramDiagnosticCode::DeviceUnavailable,
                                        "the device exposes no renderer state")};
    }
    if (!ocio_program_detail::quarantineAllowed()) {
        return {nullptr, makeDiagnostic(GpuOcioProgramDiagnosticCode::DeviceUnavailable,
                                        "too many undrained GPU generations are quarantined")};
    }
    if (spirv.empty()) {
        return {nullptr, makeDiagnostic(GpuOcioProgramDiagnosticCode::InvalidArgument,
                                        "no SPIR-V module was supplied")};
    }
    if (validateOcioGpuProgram(program, {}) != OcioGpuProgramError::None) {
        return {nullptr, makeDiagnostic(GpuOcioProgramDiagnosticCode::InvalidArgument,
                                        "the program description is not valid")};
    }
    if (!program.uniformBufferData.empty() &&
        program.uniformBufferData.size() != program.uniformBufferSize) {
        return {nullptr,
                makeDiagnostic(GpuOcioProgramDiagnosticCode::InvalidArgument,
                               "the immutable uniform snapshot does not match the UBO size")};
    }
    VkPhysicalDeviceProperties deviceLimits{};
    state->physicalDevice.getDispatcher()->vkGetPhysicalDeviceProperties(
        static_cast<VkPhysicalDevice>(*state->physicalDevice), &deviceLimits);
    auto impl = std::make_unique<Impl>();
    impl->control = state;
    impl->owner = state->owner;
    impl->budgets = budgets;
    // A caller-supplied cap may only lower the real device limit; it can never authorize a
    // vkCmdDispatch above maxComputeWorkGroupCount. Zero selects the physical limit.
    impl->maxWorkGroupCountX = ocio_program_detail::effectiveWorkGroupCount(
        budgets.maxWorkGroupCountX, deviceLimits.limits.maxComputeWorkGroupCount[0]);
    impl->maxWorkGroupCountY = ocio_program_detail::effectiveWorkGroupCount(
        budgets.maxWorkGroupCountY, deviceLimits.limits.maxComputeWorkGroupCount[1]);
    impl->expectedGeneration = static_cast<std::uint32_t>(device.ownershipEpoch());
    impl->desc = std::move(program);
    impl->spirv.assign(spirv.begin(), spirv.end());
    impl->cancellation = std::move(cancellation);
    impl->displayArm = impl->desc.stage == OcioGpuProgramStage::DisplayPacking;
    if (!impl->createOcioResources()) {
        return {nullptr,
                makeDiagnostic(impl->createDiagnostic.code, impl->createDiagnostic.message)};
    }
    if (!impl->createPipeline()) {
        return {nullptr,
                makeDiagnostic(impl->createDiagnostic.code, impl->createDiagnostic.message)};
    }
    impl->writeStaticDescriptors();
    return {std::unique_ptr<GpuOcioProgram>(new GpuOcioProgram(std::move(impl))),
            GpuOcioProgramDiagnostic{}};
}

} // namespace bloom::render
