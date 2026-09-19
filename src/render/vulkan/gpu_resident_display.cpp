#include <bloom/render/gpu_resident_display.hpp>

#include "gpu_image_private.hpp"
#include "gpu_resident_display_private.hpp"
#include "shaders/neutral_display_spirv.inc"

#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <utility>

namespace bloom::render {
namespace {

using vulkan_detail::DeviceAllocatorState;
using GpuRendererAccess = bloom::render::GpuRendererAccess;

constexpr std::uint32_t kWorkgroupSizeX = 256;
constexpr std::uint64_t kDrainTimeoutNanoseconds = 2ULL * 1000ULL * 1000ULL * 1000ULL;
constexpr std::uint64_t kMaxOwnedBytes = 512ULL * 1024ULL * 1024ULL;
constexpr std::int32_t kMaxQuarantines = 4;

std::atomic<std::int32_t> g_quarantineCount{0};
std::atomic<bool> g_teardownIncomplete{false};

[[nodiscard]] GpuResidentDisplayDiagnostic
makeDiagnostic(const GpuResidentDisplayDiagnosticCode code, std::string message) {
    return GpuResidentDisplayDiagnostic{code, std::move(message)};
}

[[nodiscard]] bool quarantineAllowed() noexcept {
    return g_quarantineCount.load() < kMaxQuarantines;
}
void noteQuarantine() noexcept {
    g_quarantineCount.fetch_add(1);
    g_teardownIncomplete.store(true);
}

enum class NativeOutcome { Ok, Failed, Cancelled, Timeout };

} // namespace

GpuResidentDisplay::GpuResidentDisplay(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
GpuResidentDisplay::GpuResidentDisplay(GpuResidentDisplay&& other) noexcept = default;
GpuResidentDisplay& GpuResidentDisplay::operator=(GpuResidentDisplay&& other) noexcept {
    if (this != &other) {
        releaseImpl();
        impl_ = std::move(other.impl_);
    }
    return *this;
}
GpuResidentDisplay::~GpuResidentDisplay() { releaseImpl(); }

void GpuResidentDisplay::releaseImpl() noexcept {
    if (impl_ == nullptr) {
        return;
    }
    if (!impl_->onOwnerThread()) {
        noteQuarantine();
        [[maybe_unused]] const auto* const quarantined = impl_.release();
        return;
    }
    if (!impl_->drainAndRetire()) {
        noteQuarantine();
        [[maybe_unused]] const auto* const quarantined = impl_.release();
        return;
    }
    impl_.reset();
}

GpuResidentDisplayJobState GpuResidentDisplay::state() const noexcept {
    return impl_ != nullptr ? impl_->jobState : GpuResidentDisplayJobState::Idle;
}
const GpuResidentDisplayDiagnostic& GpuResidentDisplay::diagnostic() const noexcept {
    static const GpuResidentDisplayDiagnostic none{};
    return impl_ != nullptr ? impl_->jobDiagnostic : none;
}
bool GpuResidentDisplay::isBoundTo(GpuDevice& device) const noexcept {
    if (impl_ == nullptr || !impl_->onOwnerThread()) {
        return false;
    }
    const auto deviceState = GpuRendererAccess::state(device);
    return deviceState != nullptr && deviceState == impl_->control;
}

bool GpuResidentDisplay::Impl::drainAndRetire() noexcept {
    if (!queueSubmitted) {
        return true;
    }
    if (deviceLost) {
        queueSubmitted = false;
        return true;
    }
    const VkFence rawFence = static_cast<VkFence>(*fence);
    const VkResult waited = control->device.getDispatcher()->vkWaitForFences(
        static_cast<VkDevice>(*control->device), 1, &rawFence, VK_TRUE, kDrainTimeoutNanoseconds);
    if (waited == VK_SUCCESS || waited == VK_ERROR_DEVICE_LOST) {
        queueSubmitted = false;
        deviceLost = deviceLost || waited == VK_ERROR_DEVICE_LOST;
        return true;
    }
    return false;
}

GpuResidentDisplay::Impl::~Impl() {
    assert(owner == std::this_thread::get_id());
    residentImage.reset();
    input.reset();
    if (control != nullptr) {
        destroyResidentBuffer(*control, jobInput);
        destroyResidentBuffer(*control, jobOutput);
        destroyResidentBuffer(*control, jobStatus);
    }
}

bool GpuResidentDisplay::Impl::createPipeline() {
    const VkDevice rawDevice = static_cast<VkDevice>(*control->device);
    const auto* dispatcher = control->device.getDispatcher();

    vk::ShaderModuleCreateInfo shaderInfo{};
    shaderInfo.codeSize = vulkan_detail::kNeutralDisplaySpirvByteCount;
    shaderInfo.pCode = vulkan_detail::kNeutralDisplaySpirvCode;
    VkShaderModule rawShader = VK_NULL_HANDLE;
    if (dispatcher->vkCreateShaderModule(
            rawDevice, reinterpret_cast<const VkShaderModuleCreateInfo*>(&shaderInfo), nullptr,
            &rawShader) != VK_SUCCESS) {
        createDiagnostic = makeDiagnostic(GpuResidentDisplayDiagnosticCode::ShaderRejected,
                                          "the embedded Neutral V1 shader module was rejected");
        return false;
    }
    shaderModule = vk::raii::ShaderModule(control->device, rawShader);

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
        createDiagnostic =
            makeDiagnostic(GpuResidentDisplayDiagnosticCode::ShaderRejected,
                           "the resident display descriptor set layout was rejected");
        return false;
    }
    descriptorSetLayout = vk::raii::DescriptorSetLayout(control->device, rawLayout);

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
        createDiagnostic = makeDiagnostic(GpuResidentDisplayDiagnosticCode::ShaderRejected,
                                          "the resident display pipeline layout was rejected");
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
        createDiagnostic = makeDiagnostic(GpuResidentDisplayDiagnosticCode::ShaderRejected,
                                          "the embedded Neutral V1 compute pipeline was rejected");
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
        createDiagnostic =
            makeDiagnostic(GpuResidentDisplayDiagnosticCode::AllocationFailed,
                           "the resident display descriptor pool could not be created");
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
        createDiagnostic =
            makeDiagnostic(GpuResidentDisplayDiagnosticCode::AllocationFailed,
                           "the resident display descriptor set could not be allocated");
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
        createDiagnostic = makeDiagnostic(GpuResidentDisplayDiagnosticCode::AllocationFailed,
                                          "the resident display command pool could not be created");
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
        createDiagnostic =
            makeDiagnostic(GpuResidentDisplayDiagnosticCode::AllocationFailed,
                           "the resident display command buffer could not be allocated");
        return false;
    }
    commandBuffer = vk::raii::CommandBuffer(control->device, rawCommandBuffer, *commandPool);

    vk::FenceCreateInfo fenceInfo{};
    VkFence rawFence = VK_NULL_HANDLE;
    if (dispatcher->vkCreateFence(rawDevice, reinterpret_cast<const VkFenceCreateInfo*>(&fenceInfo),
                                  nullptr, &rawFence) != VK_SUCCESS) {
        createDiagnostic = makeDiagnostic(GpuResidentDisplayDiagnosticCode::AllocationFailed,
                                          "the resident display fence could not be created");
        return false;
    }
    fence = vk::raii::Fence(control->device, rawFence);
    return true;
}

GpuResidentDisplayCreateResult
GpuResidentDisplay::create(GpuDevice& device, const GpuResidentDisplayBudgets& budgets) {
    if (budgets.maxOwnedBytes == 0 || budgets.maxOwnedBytes > kMaxOwnedBytes) {
        return {nullptr, makeDiagnostic(GpuResidentDisplayDiagnosticCode::InvalidArgument,
                                        "the resident display budget is out of range")};
    }
    if (device.state() != GpuDeviceState::Ready) {
        return {nullptr, makeDiagnostic(GpuResidentDisplayDiagnosticCode::DeviceUnavailable,
                                        "the GPU device is not Ready")};
    }
    if (GpuRendererAccess::owner(device) != std::this_thread::get_id()) {
        return {nullptr, makeDiagnostic(GpuResidentDisplayDiagnosticCode::WrongThread,
                                        "the resident display must be created on the device owner "
                                        "thread")};
    }
    if (!quarantineAllowed()) {
        return {nullptr, makeDiagnostic(GpuResidentDisplayDiagnosticCode::DeviceUnavailable,
                                        "too many undrained GPU generations are quarantined")};
    }
    auto control = GpuRendererAccess::state(device);
    if (control == nullptr) {
        return {nullptr, makeDiagnostic(GpuResidentDisplayDiagnosticCode::DeviceUnavailable,
                                        "the GPU device exposes no renderer state")};
    }
    auto impl = std::make_unique<Impl>();
    impl->owner = std::this_thread::get_id();
    impl->control = std::move(control);
    impl->budgets = budgets;
    impl->expectedGeneration = impl->control->generation;
    if (!impl->createPipeline()) {
        return {nullptr, impl->createDiagnostic};
    }
    return {std::unique_ptr<GpuResidentDisplay>(new GpuResidentDisplay(std::move(impl))),
            GpuResidentDisplayDiagnostic{}};
}

GpuResidentDisplayDiagnostic GpuResidentDisplay::begin(std::shared_ptr<const GpuImage> input,
                                                       const std::uint64_t byteBudget) {
    if (impl_ == nullptr) {
        return makeDiagnostic(GpuResidentDisplayDiagnosticCode::DeviceUnavailable,
                              "the resident display is not initialized");
    }
    Impl& impl = *impl_;
    if (!impl.onOwnerThread()) {
        return makeDiagnostic(GpuResidentDisplayDiagnosticCode::WrongThread,
                              "begin must run on the device owner thread");
    }
    if (impl.deviceLost) {
        return makeDiagnostic(GpuResidentDisplayDiagnosticCode::DeviceLost,
                              "the device was lost; this generation must not be reused");
    }
    if (impl.queueSubmitted || impl.jobState == GpuResidentDisplayJobState::Pending) {
        return makeDiagnostic(GpuResidentDisplayDiagnosticCode::Busy,
                              "one job is already in flight");
    }
    if (input == nullptr || !input->isValid()) {
        return makeDiagnostic(GpuResidentDisplayDiagnosticCode::InvalidArgument,
                              "the input resident image is invalid");
    }
    const GpuImageImpl* inputImpl = gpuImageImpl(*input);
    if (inputImpl == nullptr || inputImpl->state != impl.control) {
        return makeDiagnostic(GpuResidentDisplayDiagnosticCode::DeviceUnavailable,
                              "the input image belongs to a different device generation");
    }
    const std::uint32_t width = inputImpl->width;
    const std::uint32_t height = inputImpl->height;
    if (width == 0 || height == 0 || !inputImpl->dataWindow.has_value() ||
        !inputImpl->displayWindow.has_value()) {
        return makeDiagnostic(GpuResidentDisplayDiagnosticCode::InvalidArgument,
                              "the input image has no valid geometry");
    }
    const std::uint64_t inputBytes = static_cast<std::uint64_t>(width) * height * sizeof(Rgba32f);
    const std::uint64_t packedBytes = static_cast<std::uint64_t>(width) * height * sizeof(Rgba8);
    const std::uint64_t required = inputBytes + packedBytes + sizeof(std::uint32_t) + packedBytes;
    const std::uint64_t allowed =
        byteBudget < impl.budgets.maxOwnedBytes ? byteBudget : impl.budgets.maxOwnedBytes;
    if (required > allowed) {
        return makeDiagnostic(
            GpuResidentDisplayDiagnosticCode::OverBudget,
            "the resident display job exceeds the configured or requested budget");
    }
    if (impl.control->generation != impl.expectedGeneration) {
        impl.deviceLost = true;
        return makeDiagnostic(GpuResidentDisplayDiagnosticCode::DeviceLost,
                              "the device generation changed; this pipeline must not be reused");
    }
    const ResidentDisplaySupport support =
        queryResidentDisplaySupport(*impl.control, width, height);
    if (!support.supported) {
        return makeDiagnostic(GpuResidentDisplayDiagnosticCode::Unsupported, support.reason);
    }
    if (required > support.maxOwnedBytes) {
        return makeDiagnostic(GpuResidentDisplayDiagnosticCode::OverBudget,
                              "the job exceeds the device owned-bytes limit");
    }

    impl.clearJob();
    impl.input = std::move(input);
    if (!createResidentBuffer(*impl.control, inputBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0,
                              false, impl.jobInput) ||
        !createResidentBuffer(*impl.control, packedBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0,
                              false, impl.jobOutput) ||
        !createResidentBuffer(
            *impl.control, sizeof(std::uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT, true,
            impl.jobStatus)) {
        impl.fail(GpuResidentDisplayDiagnosticCode::AllocationFailed,
                  "the resident display buffers could not be allocated");
        return impl.jobDiagnostic;
    }
    auto displayImpl = std::make_unique<GpuDisplayImageImpl>();
    displayImpl->state = impl.control;
    displayImpl->dataWindow = inputImpl->dataWindow;
    displayImpl->displayWindow = inputImpl->displayWindow;
    displayImpl->pixelAspect = inputImpl->pixelAspect;
    displayImpl->generation = impl.control->generation;
    if (!createDisplayImage(*impl.control, width, height, *displayImpl)) {
        impl.fail(GpuResidentDisplayDiagnosticCode::AllocationFailed,
                  "the resident display output image could not be allocated");
        return impl.jobDiagnostic;
    }
    GpuDisplayImageImpl* const displayRaw = displayImpl.get();
    impl.residentImage =
        std::make_unique<GpuDisplayImage>(makeGpuDisplayImage(std::move(displayImpl)));

    const VkDevice rawDevice = static_cast<VkDevice>(*impl.control->device);
    const auto* dispatcher = impl.control->device.getDispatcher();
    const VkCommandBuffer rawCommandBuffer = static_cast<VkCommandBuffer>(*impl.commandBuffer);
    const VkFence rawFence = static_cast<VkFence>(*impl.fence);
    if (dispatcher->vkResetFences(rawDevice, 1, &rawFence) != VK_SUCCESS ||
        dispatcher->vkResetCommandBuffer(rawCommandBuffer, 0) != VK_SUCCESS) {
        impl.fail(GpuResidentDisplayDiagnosticCode::DeviceUnavailable,
                  "the resident display fence or command buffer could not be reset");
        return impl.jobDiagnostic;
    }

    // The shader atomicOr-accumulates into the status word, so it MUST be zeroed by the host
    // before the dispatch; otherwise recycled mapped memory is reported as a shader
    // rejection. The hostToCompute barrier below already declares VK_ACCESS_HOST_WRITE_BIT
    // for exactly this write.
    if (impl.jobStatus.info.pMappedData == nullptr) {
        impl.fail(GpuResidentDisplayDiagnosticCode::DeviceUnavailable,
                  "the resident display status buffer is not mapped");
        return impl.jobDiagnostic;
    }
    *static_cast<std::uint32_t*>(impl.jobStatus.info.pMappedData) = 0U;
    if (vmaFlushAllocation(impl.control->allocator, impl.jobStatus.allocation, 0,
                           sizeof(std::uint32_t)) != VK_SUCCESS) {
        impl.fail(GpuResidentDisplayDiagnosticCode::DeviceUnavailable,
                  "the resident display status buffer could not be flushed");
        return impl.jobDiagnostic;
    }

    std::array<vk::DescriptorBufferInfo, 3> infos{};
    infos[0] = vk::DescriptorBufferInfo{impl.jobInput.buffer, 0, VK_WHOLE_SIZE};
    infos[1] = vk::DescriptorBufferInfo{impl.jobOutput.buffer, 0, VK_WHOLE_SIZE};
    infos[2] = vk::DescriptorBufferInfo{impl.jobStatus.buffer, 0, VK_WHOLE_SIZE};
    std::array<vk::WriteDescriptorSet, 3> writes{};
    for (std::uint32_t index = 0; index < 3; ++index) {
        writes[index].dstSet = *impl.descriptorSet;
        writes[index].dstBinding = index;
        writes[index].descriptorCount = 1;
        writes[index].descriptorType = vk::DescriptorType::eStorageBuffer;
        writes[index].pBufferInfo = &infos[index];
    }
    dispatcher->vkUpdateDescriptorSets(rawDevice, static_cast<std::uint32_t>(writes.size()),
                                       reinterpret_cast<const VkWriteDescriptorSet*>(writes.data()),
                                       0, nullptr);

    vk::CommandBufferBeginInfo beginInfo{};
    beginInfo.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
    if (dispatcher->vkBeginCommandBuffer(
            rawCommandBuffer, reinterpret_cast<const VkCommandBufferBeginInfo*>(&beginInfo)) !=
        VK_SUCCESS) {
        impl.fail(GpuResidentDisplayDiagnosticCode::DeviceUnavailable,
                  "the resident display command buffer could not begin");
        return impl.jobDiagnostic;
    }

    VkImageMemoryBarrier inputToTransfer{};
    inputToTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    inputToTransfer.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    inputToTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    inputToTransfer.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    inputToTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    inputToTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    inputToTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    inputToTransfer.image = inputImpl->image;
    inputToTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    dispatcher->vkCmdPipelineBarrier(rawCommandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                     &inputToTransfer);
    VkBufferImageCopy toBuffer{};
    toBuffer.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    toBuffer.imageExtent = {width, height, 1};
    dispatcher->vkCmdCopyImageToBuffer(rawCommandBuffer, inputImpl->image,
                                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, impl.jobInput.buffer,
                                       1, &toBuffer);
    VkImageMemoryBarrier inputToGeneral = inputToTransfer;
    inputToGeneral.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    inputToGeneral.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    inputToGeneral.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    inputToGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    dispatcher->vkCmdPipelineBarrier(rawCommandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                                     nullptr, 1, &inputToGeneral);

    std::array<VkBufferMemoryBarrier, 2> hostToCompute{};
    hostToCompute[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    hostToCompute[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    hostToCompute[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    hostToCompute[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    hostToCompute[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    hostToCompute[0].buffer = impl.jobInput.buffer;
    hostToCompute[0].offset = 0;
    hostToCompute[0].size = inputBytes;
    hostToCompute[1] = hostToCompute[0];
    hostToCompute[1].buffer = impl.jobStatus.buffer;
    hostToCompute[1].srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    hostToCompute[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    hostToCompute[1].size = sizeof(std::uint32_t);
    dispatcher->vkCmdPipelineBarrier(rawCommandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                                     static_cast<std::uint32_t>(hostToCompute.size()),
                                     hostToCompute.data(), 0, nullptr);

    impl.commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, *impl.pipeline);
    impl.commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *impl.pipelineLayout, 0,
                                          {*impl.descriptorSet}, {});
    // One-dimensional dispatch over the whole pixel count: the shader reads only
    // gl_GlobalInvocationID.x, so a 2D dispatch re-runs lanes and leaves the tail of every
    // width-grouped row unprocessed for non-multiples of the workgroup size.
    const std::uint64_t pixelCount64 =
        static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
    if (pixelCount64 > std::numeric_limits<std::uint32_t>::max()) {
        impl.fail(GpuResidentDisplayDiagnosticCode::OverBudget,
                  "the pixel count exceeds the uint32 push-constant range");
        return impl.jobDiagnostic;
    }
    const std::uint32_t pixelCount = static_cast<std::uint32_t>(pixelCount64);
    impl.commandBuffer.pushConstants(*impl.pipelineLayout, vk::ShaderStageFlagBits::eCompute, 0,
                                     static_cast<std::uint32_t>(sizeof(std::uint32_t)),
                                     &pixelCount);
    const std::uint32_t groups = (pixelCount + kWorkgroupSizeX - 1U) / kWorkgroupSizeX;
    impl.commandBuffer.dispatch(groups, 1, 1);

    VkBufferMemoryBarrier packedToTransfer = hostToCompute[0];
    packedToTransfer.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    packedToTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    packedToTransfer.buffer = impl.jobOutput.buffer;
    packedToTransfer.size = packedBytes;
    dispatcher->vkCmdPipelineBarrier(rawCommandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1,
                                     &packedToTransfer, 0, nullptr);

    VkImageMemoryBarrier outputToTransfer{};
    outputToTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    outputToTransfer.srcAccessMask = 0;
    outputToTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    outputToTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    outputToTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    outputToTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    outputToTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    outputToTransfer.image = displayRaw->image;
    outputToTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    dispatcher->vkCmdPipelineBarrier(rawCommandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                     &outputToTransfer);
    VkBufferImageCopy toImage{};
    toImage.bufferOffset = 0;
    toImage.bufferRowLength = 0;
    toImage.bufferImageHeight = 0;
    toImage.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    toImage.imageExtent = {width, height, 1};
    dispatcher->vkCmdCopyBufferToImage(rawCommandBuffer, impl.jobOutput.buffer, displayRaw->image,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &toImage);
    VkImageMemoryBarrier outputToRead = outputToTransfer;
    outputToRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    outputToRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    outputToRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    outputToRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    dispatcher->vkCmdPipelineBarrier(rawCommandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                                     nullptr, 1, &outputToRead);

    if (dispatcher->vkEndCommandBuffer(rawCommandBuffer) != VK_SUCCESS) {
        impl.fail(GpuResidentDisplayDiagnosticCode::DeviceUnavailable,
                  "the resident display command buffer could not end");
        return impl.jobDiagnostic;
    }
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &rawCommandBuffer;
    const VkResult submitted = dispatcher->vkQueueSubmit(
        static_cast<VkQueue>(*impl.control->computeQueue), 1, &submit, rawFence);
    if (submitted != VK_SUCCESS) {
        impl.fail(submitted == VK_ERROR_DEVICE_LOST
                      ? GpuResidentDisplayDiagnosticCode::DeviceLost
                      : GpuResidentDisplayDiagnosticCode::DeviceUnavailable,
                  "the resident display dispatch could not be submitted");
        return impl.jobDiagnostic;
    }
    impl.queueSubmitted = true;
    impl.jobState = GpuResidentDisplayJobState::Pending;
    impl.jobDiagnostic = GpuResidentDisplayDiagnostic{};
    return {};
}

GpuResidentDisplayPollResult GpuResidentDisplay::poll() {
    if (impl_ == nullptr) {
        return GpuResidentDisplayPollResult::Failure;
    }
    Impl& impl = *impl_;
    if (!impl.onOwnerThread()) {
        return GpuResidentDisplayPollResult::WrongThread;
    }
    if (impl.jobState == GpuResidentDisplayJobState::Ready) {
        return GpuResidentDisplayPollResult::Ready;
    }
    const bool pending = impl.jobState == GpuResidentDisplayJobState::Pending;
    if (!pending && !impl.queueSubmitted) {
        return GpuResidentDisplayPollResult::Failure;
    }
    const VkFence rawFence = static_cast<VkFence>(*impl.fence);
    const VkResult status = impl.control->device.getDispatcher()->vkGetFenceStatus(
        static_cast<VkDevice>(*impl.control->device), rawFence);
    if (status == VK_NOT_READY) {
        return pending ? GpuResidentDisplayPollResult::Pending
                       : GpuResidentDisplayPollResult::Failure;
    }
    if (status == VK_ERROR_DEVICE_LOST) {
        impl.deviceLost = true;
        impl.queueSubmitted = false;
        if (pending) {
            impl.fail(GpuResidentDisplayDiagnosticCode::DeviceLost,
                      "the device was lost while polling");
        }
        return GpuResidentDisplayPollResult::Failure;
    }
    if (status != VK_SUCCESS) {
        if (pending) {
            impl.fail(GpuResidentDisplayDiagnosticCode::DeviceUnavailable,
                      "the resident display fence returned an unexpected status; not retired");
        }
        return GpuResidentDisplayPollResult::Failure;
    }
    impl.queueSubmitted = false;
    if (!pending) {
        return GpuResidentDisplayPollResult::Failure;
    }
    if (impl.discardRequested.load()) {
        impl.clearJob();
        impl.fail(GpuResidentDisplayDiagnosticCode::Cancelled,
                  "the resident display job was cancelled");
        return GpuResidentDisplayPollResult::Failure;
    }
    if (vmaInvalidateAllocation(impl.control->allocator, impl.jobStatus.allocation, 0,
                                sizeof(std::uint32_t)) != VK_SUCCESS) {
        impl.fail(GpuResidentDisplayDiagnosticCode::DeviceUnavailable,
                  "the status word could not be invalidated");
        return GpuResidentDisplayPollResult::Failure;
    }
    const auto flags = *static_cast<const std::uint32_t*>(impl.jobStatus.info.pMappedData);
    if (flags != 0U) {
        impl.fail(GpuResidentDisplayDiagnosticCode::ShaderRejected,
                  "the display shader rejected the frame (flags=" + std::to_string(flags) + ")");
        return GpuResidentDisplayPollResult::Failure;
    }
    if (impl.residentImage == nullptr) {
        impl.fail(GpuResidentDisplayDiagnosticCode::DeviceUnavailable,
                  "the resident display output is missing");
        return GpuResidentDisplayPollResult::Failure;
    }
    impl.jobState = GpuResidentDisplayJobState::Ready;
    impl.jobDiagnostic = GpuResidentDisplayDiagnostic{};
    return GpuResidentDisplayPollResult::Ready;
}

const GpuDisplayImage* GpuResidentDisplay::image() const noexcept {
    if (impl_ == nullptr || impl_->jobState != GpuResidentDisplayJobState::Ready) {
        return nullptr;
    }
    return impl_->residentImage.get();
}
GpuDisplayImage GpuResidentDisplay::takeImage() noexcept {
    if (impl_ == nullptr || impl_->residentImage == nullptr) {
        return GpuDisplayImage{};
    }
    GpuDisplayImage taken = std::move(*impl_->residentImage);
    impl_->residentImage.reset();
    impl_->clearJob();
    return taken;
}
GpuDisplayImageReadback GpuResidentDisplay::readback() noexcept {
    if (impl_ == nullptr || impl_->residentImage == nullptr) {
        GpuDisplayImageReadback result;
        result.code = GpuDisplayImageReadbackCode::DeviceUnavailable;
        result.message = "no resident display image to read back";
        return result;
    }
    return readbackResidentDisplayImage(*impl_->residentImage, impl_->budgets.maxOwnedBytes);
}
void GpuResidentDisplay::cancel() noexcept {
    if (impl_ != nullptr) {
        impl_->discardRequested.store(true);
    }
}
bool GpuResidentDisplay::teardownDrainIncomplete() noexcept { return g_teardownIncomplete.load(); }

// Folded additive accessor (declared in the public header). It reports the native Impl's
// queueSubmitted flag, the authoritative retirement test: logical job state is not proof because an
// unknown fence result sets Failure without clearing the submission. No field or layout change.
bool GpuResidentDisplay::hasUnretiredSubmission() const noexcept {
    return impl_ != nullptr && impl_->queueSubmitted;
}

} // namespace bloom::render
