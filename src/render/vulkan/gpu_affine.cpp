#include <bloom/render/gpu_affine.hpp>

#include "gpu_affine_private.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace bloom::render {
namespace {

constexpr std::uint64_t kAffineDrainTimeoutNanoseconds = 2ULL * 1000ULL * 1000ULL * 1000ULL;

[[nodiscard]] GpuAffineDiagnostic makeDiagnostic(const GpuAffineDiagnosticCode code,
                                                 std::string message) {
    return affineDiagnostic(code, std::move(message));
}

// Overflow-checked per-pixel metadata size for one output window; refuses a window whose pixel
// count or byte size cannot be represented. Used before any host allocation.
[[nodiscard]] bool affineMetadataBytes(ImageWindow outputWindow, std::uint64_t& out) noexcept {
    const std::uint64_t width = outputWindow.extent().width();
    const std::uint64_t height = outputWindow.extent().height();
    std::uint64_t pixels = 0;
    if (width == 0 || height == 0 || compositeMultiplyOverflows(width, height, pixels) ||
        compositeMultiplyOverflows(pixels, sizeof(GpuAffineSample), out)) {
        return false;
    }
    return true;
}

} // namespace

void GpuAffine::Impl::fail(const GpuAffineDiagnosticCode code, std::string message) {
    jobState = GpuAffineJobState::Failure;
    jobDiagnostic = affineDiagnostic(code, std::move(message));
}

void GpuAffine::Impl::clearJob() {
    jobState = GpuAffineJobState::Idle;
    jobDiagnostic = GpuAffineDiagnostic{};
    discardRequested.store(false);
    residentImage.reset();
    retainedSource.reset();
    samples.release();
    status.release();
    statusMapped = nullptr;
}

GpuAffine::GpuAffine(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
GpuAffine::GpuAffine(GpuAffine&& other) noexcept = default;
GpuAffine& GpuAffine::operator=(GpuAffine&& other) noexcept {
    if (this != &other) {
        releaseImpl();
        impl_ = std::move(other.impl_);
    }
    return *this;
}
GpuAffine::~GpuAffine() { releaseImpl(); }

void GpuAffine::releaseImpl() noexcept {
    if (impl_ == nullptr) {
        return;
    }
    if (!impl_->onOwnerThread() || !impl_->drainAndRetire()) {
        noteAffineQuarantine();
        [[maybe_unused]] const auto* const quarantined = impl_.release();
        return;
    }
    impl_.reset();
}

GpuAffineJobState GpuAffine::state() const noexcept {
    return impl_ != nullptr ? impl_->jobState : GpuAffineJobState::Idle;
}
const GpuAffineDiagnostic& GpuAffine::diagnostic() const noexcept {
    static const GpuAffineDiagnostic none{};
    if (impl_ == nullptr) {
        return none;
    }
    return impl_->jobDiagnostic;
}
bool GpuAffine::isBoundTo(GpuDevice& device) const noexcept {
    if (impl_ == nullptr || !impl_->onOwnerThread()) {
        return false;
    }
    const auto deviceState = GpuRendererAccess::state(device);
    return deviceState != nullptr && deviceState == impl_->control;
}

bool GpuAffine::hasUnretiredSubmission() const noexcept {
    return impl_ != nullptr && impl_->queueSubmitted;
}

std::uint64_t GpuAffine::lastJobAllocationBytes() const noexcept {
    return impl_ != nullptr ? impl_->lastJobBytes : 0;
}

bool GpuAffine::Impl::drainAndRetire() noexcept {
    if (!queueSubmitted) {
        return true;
    }
    if (deviceLost) {
        queueSubmitted = false;
        return true;
    }
    const VkFence rawFence = static_cast<VkFence>(*fence);
    const VkResult waited = control->device.getDispatcher()->vkWaitForFences(
        static_cast<VkDevice>(*control->device), 1, &rawFence, VK_TRUE,
        kAffineDrainTimeoutNanoseconds);
    if (waited == VK_SUCCESS || waited == VK_ERROR_DEVICE_LOST) {
        queueSubmitted = false;
        deviceLost = deviceLost || waited == VK_ERROR_DEVICE_LOST;
        return true;
    }
    return false;
}

GpuAffine::Impl::~Impl() {
    assert(owner == std::this_thread::get_id());
    retainedSource.reset();
    residentImage.reset();
}

bool GpuAffine::Impl::createPipelines() {
    constexpr bool kAffineBindings[kAffineBindingCount] = {true, true, false, false};
    std::string reason;
    if (!createCompositePipeline(*control, vulkan_detail::kAffineBilinearSpirvCode,
                                 vulkan_detail::kAffineBilinearSpirvByteCount, kAffineBindings,
                                 kAffineBindingCount, kAffinePushBytes, reason, affine)) {
        createDiagnostic = makeDiagnostic(GpuAffineDiagnosticCode::ShaderRejected, reason);
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
        createDiagnostic = makeDiagnostic(GpuAffineDiagnosticCode::AllocationFailed,
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
        createDiagnostic = makeDiagnostic(GpuAffineDiagnosticCode::AllocationFailed,
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
        createDiagnostic = makeDiagnostic(GpuAffineDiagnosticCode::AllocationFailed,
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
        createDiagnostic = makeDiagnostic(GpuAffineDiagnosticCode::AllocationFailed,
                                          "the affine command buffer could not be allocated");
        return false;
    }
    commandBuffer = vk::raii::CommandBuffer(control->device, rawCommandBuffer, *commandPool);
    vk::FenceCreateInfo fenceInfo{};
    VkFence rawFence = VK_NULL_HANDLE;
    if (dispatcher->vkCreateFence(rawDevice, reinterpret_cast<const VkFenceCreateInfo*>(&fenceInfo),
                                  nullptr, &rawFence) != VK_SUCCESS) {
        createDiagnostic = makeDiagnostic(GpuAffineDiagnosticCode::AllocationFailed,
                                          "the affine fence could not be created");
        return false;
    }
    fence = vk::raii::Fence(control->device, rawFence);
    return true;
}

GpuAffineCreateResult GpuAffine::create(GpuDevice& device, const GpuAffineBudgets& budgets) {
    if (budgets.maxImageBytes == 0 || budgets.maxMetadataBytes == 0) {
        return {nullptr, makeDiagnostic(GpuAffineDiagnosticCode::InvalidArgument,
                                        "the affine budget is out of range")};
    }
    if (device.state() != GpuDeviceState::Ready) {
        return {nullptr, makeDiagnostic(GpuAffineDiagnosticCode::DeviceUnavailable,
                                        "the GPU device is not Ready")};
    }
    if (GpuRendererAccess::owner(device) != std::this_thread::get_id()) {
        return {nullptr, makeDiagnostic(GpuAffineDiagnosticCode::WrongThread,
                                        "the affine pipeline must be created on the device owner "
                                        "thread")};
    }
    if (!affineQuarantineAllowed()) {
        return {nullptr, makeDiagnostic(GpuAffineDiagnosticCode::DeviceUnavailable,
                                        "too many undrained GPU generations are quarantined")};
    }
    auto control = GpuRendererAccess::state(device);
    if (control == nullptr) {
        return {nullptr, makeDiagnostic(GpuAffineDiagnosticCode::DeviceUnavailable,
                                        "the GPU device exposes no renderer state")};
    }
    auto impl = std::make_unique<Impl>();
    impl->owner = std::this_thread::get_id();
    impl->control = std::move(control);
    impl->budgets = budgets;
    impl->expectedGeneration = impl->control->generation;
    if (!impl->createPipelines()) {
        return {nullptr, impl->createDiagnostic};
    }
    return {std::unique_ptr<GpuAffine>(new GpuAffine(std::move(impl))), GpuAffineDiagnostic{}};
}

bool GpuAffine::Impl::checkStatusFlag() {
    if (statusMapped == nullptr) {
        return true;
    }
    std::uint32_t flag = 0;
    std::memcpy(&flag, statusMapped, sizeof(flag));
    if (flag != 0) {
        fail(GpuAffineDiagnosticCode::StatusFlagRejected,
             "the affine kernel rejected the frame (non-finite or subnormal input); use the CPU "
             "reference path");
        return false;
    }
    return true;
}

GpuAffineDiagnostic GpuAffine::Impl::preflightCheap() {
    if (!onOwnerThread()) {
        return affineDiagnostic(GpuAffineDiagnosticCode::WrongThread,
                                "beginAffine must run on the device owner thread");
    }
    if (deviceLost) {
        return affineDiagnostic(GpuAffineDiagnosticCode::DeviceLost,
                                "the device was lost; this generation must not be reused");
    }
    if (queueSubmitted || jobState == GpuAffineJobState::Pending) {
        return affineDiagnostic(GpuAffineDiagnosticCode::Busy, "one job is already in flight");
    }
    if (control != nullptr && control->generation != expectedGeneration) {
        deviceLost = true;
        return affineDiagnostic(GpuAffineDiagnosticCode::DeviceLost,
                                "the device generation changed; this pipeline must not be reused");
    }
    return GpuAffineDiagnostic{};
}

GpuAffineDiagnostic GpuAffine::beginAffine(const GpuAffineParameters& parameters,
                                           const std::uint64_t byteBudget) {
    if (impl_ == nullptr) {
        return makeDiagnostic(GpuAffineDiagnosticCode::DeviceUnavailable,
                              "the affine pipeline is not initialized");
    }
    // Cheap gates first: reject a foreign thread, a busy pipeline, or a stale generation before any
    // O(width*height) metadata preparation.
    const auto cheap = impl_->preflightCheap();
    if (cheap.code != GpuAffineDiagnosticCode::None) {
        return cheap;
    }
    if (!parameters.transform.has_value()) {
        return makeDiagnostic(GpuAffineDiagnosticCode::InvalidArgument,
                              "the affine LayerTransform is missing");
    }
    if (parameters.source == nullptr || !parameters.source->isValid()) {
        return makeDiagnostic(GpuAffineDiagnosticCode::InvalidArgument,
                              "the affine source image is missing");
    }
    const auto sourceWindow = parameters.source->dataWindow();
    if (!sourceWindow.has_value()) {
        return makeDiagnostic(GpuAffineDiagnosticCode::InvalidArgument,
                              "the affine source image has no data window");
    }
    if (parameters.transform->sourceWindow() != *sourceWindow) {
        return makeDiagnostic(GpuAffineDiagnosticCode::InvalidArgument,
                              "the LayerTransform source window does not match the source image");
    }
    std::uint64_t metadataBytes = 0;
    if (!affineMetadataBytes(parameters.outputWindow, metadataBytes) ||
        metadataBytes > impl_->budgets.maxMetadataBytes || metadataBytes > byteBudget) {
        return makeDiagnostic(GpuAffineDiagnosticCode::OverBudget,
                              "the affine sample metadata exceeds the byte budget");
    }
    const auto samples = prepareAffineSamples(*parameters.transform, parameters.outputWindow);
    return impl_->beginPrepared(parameters.source, parameters.outputWindow, samples,
                                parameters.transform->opacity(), byteBudget);
}

GpuAffineDiagnostic GpuAffine::beginAffineMatrix(const GpuAffineMatrixParameters& parameters,
                                                 const std::uint64_t byteBudget) {
    if (impl_ == nullptr) {
        return makeDiagnostic(GpuAffineDiagnosticCode::DeviceUnavailable,
                              "the affine pipeline is not initialized");
    }
    const auto cheap = impl_->preflightCheap();
    if (cheap.code != GpuAffineDiagnosticCode::None) {
        return cheap;
    }
    if (parameters.source == nullptr || !parameters.source->isValid()) {
        return makeDiagnostic(GpuAffineDiagnosticCode::InvalidArgument,
                              "the affine source image is missing");
    }
    const auto sourceWindow = parameters.source->dataWindow();
    if (!sourceWindow.has_value()) {
        return makeDiagnostic(GpuAffineDiagnosticCode::InvalidArgument,
                              "the affine source image has no data window");
    }
    if (!std::isfinite(parameters.opacity) || parameters.opacity < 0.0F ||
        parameters.opacity > 1.0F) {
        return makeDiagnostic(GpuAffineDiagnosticCode::InvalidArgument,
                              "the affine opacity is out of domain");
    }
    std::uint64_t metadataBytes = 0;
    if (!affineMetadataBytes(parameters.outputWindow, metadataBytes) ||
        metadataBytes > impl_->budgets.maxMetadataBytes || metadataBytes > byteBudget) {
        return makeDiagnostic(GpuAffineDiagnosticCode::OverBudget,
                              "the affine sample metadata exceeds the byte budget");
    }
    const auto samples =
        prepareAffineMatrixSamples(parameters.matrix, *sourceWindow, parameters.outputWindow);
    return impl_->beginPrepared(parameters.source, parameters.outputWindow, samples,
                                parameters.opacity, byteBudget);
}

GpuAffineDiagnostic
GpuAffine::Impl::beginPrepared(const std::shared_ptr<const GpuImage>& source,
                               const ImageWindow outputWindow,
                               const std::span<const GpuAffineSample> preparedSamples,
                               const float opacity, const std::uint64_t byteBudget) {
    if (!onOwnerThread()) {
        return makeDiagnostic(GpuAffineDiagnosticCode::WrongThread,
                              "beginAffine must run on the device owner thread");
    }
    if (deviceLost) {
        return makeDiagnostic(GpuAffineDiagnosticCode::DeviceLost,
                              "the device was lost; this generation must not be reused");
    }
    if (queueSubmitted || jobState == GpuAffineJobState::Pending) {
        return makeDiagnostic(GpuAffineDiagnosticCode::Busy, "one job is already in flight");
    }
    if (source == nullptr || !source->isValid()) {
        return makeDiagnostic(GpuAffineDiagnosticCode::InvalidArgument,
                              "the affine source image is missing");
    }
    const auto sourceWindow = source->dataWindow();
    if (!sourceWindow.has_value()) {
        return makeDiagnostic(GpuAffineDiagnosticCode::InvalidArgument,
                              "the affine source image has no data window");
    }
    const std::uint32_t sourceWidth = sourceWindow->extent().width();
    const std::uint32_t sourceHeight = sourceWindow->extent().height();
    const std::uint32_t outputWidth = outputWindow.extent().width();
    const std::uint32_t outputHeight = outputWindow.extent().height();
    if (sourceWidth == 0 || sourceHeight == 0 || outputWidth == 0 || outputHeight == 0) {
        return makeDiagnostic(GpuAffineDiagnosticCode::InvalidArgument, "an extent is empty");
    }
    if (!std::isfinite(opacity) || opacity < 0.0F || opacity > 1.0F) {
        return makeDiagnostic(GpuAffineDiagnosticCode::InvalidArgument,
                              "the affine opacity is out of domain");
    }
    std::uint64_t expectedSamples = 0;
    if (compositeMultiplyOverflows(outputWidth, outputHeight, expectedSamples) ||
        expectedSamples != preparedSamples.size()) {
        return makeDiagnostic(GpuAffineDiagnosticCode::InvalidArgument,
                              "the prepared sample count does not match the output window");
    }
    // Requested byte math is overflow-checked: two uint32 extents can overflow uint64.
    std::uint64_t imageBytes = 0;
    std::uint64_t sampleBytes = 0;
    std::uint64_t metadataBytes = 0;
    if (compositeMultiplyOverflows(outputWidth, outputHeight, imageBytes) ||
        compositeMultiplyOverflows(imageBytes, sizeof(Rgba32f), imageBytes) ||
        compositeMultiplyOverflows(expectedSamples, sizeof(GpuAffineSample), sampleBytes) ||
        compositeAddOverflows(sampleBytes, sizeof(std::uint32_t), metadataBytes)) {
        return makeDiagnostic(GpuAffineDiagnosticCode::OverBudget,
                              "the affine request overflows the byte arithmetic");
    }
    const std::uint64_t allowedImage =
        byteBudget < budgets.maxImageBytes ? byteBudget : budgets.maxImageBytes;
    if (imageBytes > allowedImage || metadataBytes > budgets.maxMetadataBytes) {
        return makeDiagnostic(GpuAffineDiagnosticCode::OverBudget,
                              "the affine image or metadata exceeds the byte budget");
    }
    if (control->generation != expectedGeneration) {
        deviceLost = true;
        return makeDiagnostic(GpuAffineDiagnosticCode::DeviceLost,
                              "the device generation changed; this pipeline must not be reused");
    }
    if (!compositeImageBelongsTo(gpuImageImpl(*source), control)) {
        return makeDiagnostic(GpuAffineDiagnosticCode::InvalidArgument,
                              "the affine source image does not belong to this device");
    }
    const SolidImageSupport support = querySolidImageSupport(*control, outputWidth, outputHeight);
    if (!support.supported || imageBytes > support.maxImageBytes) {
        return makeDiagnostic(support.supported ? GpuAffineDiagnosticCode::OverBudget
                                                : GpuAffineDiagnosticCode::Unsupported,
                              support.supported ? "the output image exceeds the device limit"
                                                : support.reason);
    }

    clearJob();
    // Device buffers first so any allocation failure precedes any driver work.
    if (!createCompositeBuffer(*control, sampleBytes, false, preparedSamples.data(),
                               this->samples)) {
        return makeDiagnostic(GpuAffineDiagnosticCode::AllocationFailed,
                              "the affine sample metadata buffer could not be allocated");
    }
    const std::uint32_t zeroFlag = 0;
    if (!createCompositeBuffer(*control, sizeof(zeroFlag), true, &zeroFlag, status)) {
        return makeDiagnostic(GpuAffineDiagnosticCode::AllocationFailed,
                              "the affine status buffer could not be allocated");
    }
    VmaAllocationInfo statusInfo{};
    vmaGetAllocationInfo(control->allocator, status.allocation, &statusInfo);
    statusMapped = statusInfo.pMappedData;

    auto resident = std::make_unique<GpuImageImpl>();
    resident->state = control;
    // Data window is the requested output window; DISPLAY window and pixel aspect are preserved
    // from the source so a nonzero-origin composition keeps its geometry.
    resident->dataWindow = outputWindow;
    resident->displayWindow = source->displayWindow();
    resident->pixelAspect = source->pixelAspect();
    resident->generation = control->generation;
    if (!createResidentImage(*control, outputWidth, outputHeight, *resident)) {
        return makeDiagnostic(GpuAffineDiagnosticCode::AllocationFailed,
                              "the affine output image could not be allocated");
    }
    GpuImageImpl* const outputRaw = resident.get();

    // Enforce the budget on the ACTUAL VMA allocation sizes (allocator rounding included) plus the
    // transient staging peak for the sample upload.
    std::uint64_t retainedActual = 0;
    std::uint64_t peakActual = 0;
    {
        const std::uint64_t outputActual = compositeImageAllocationBytes(*outputRaw);
        const std::uint64_t samplesActual = compositeBufferAllocationBytes(this->samples);
        const std::uint64_t statusActual = compositeBufferAllocationBytes(status);
        std::uint64_t sum = 0;
        if (compositeAddOverflows(outputActual, samplesActual, sum) ||
            compositeAddOverflows(sum, statusActual, sum) ||
            compositeAddOverflows(sum, samplesActual, peakActual)) {
            return makeDiagnostic(GpuAffineDiagnosticCode::OverBudget,
                                  "the affine allocation sizes overflow the byte arithmetic");
        }
        retainedActual = sum;
    }
    if (retainedActual > byteBudget || peakActual > byteBudget) {
        return makeDiagnostic(GpuAffineDiagnosticCode::OverBudget,
                              "the actual affine allocations or transient peak exceed the byte "
                              "budget");
    }
    lastJobBytes = retainedActual > peakActual ? retainedActual : peakActual;

    residentImage = std::make_unique<GpuImage>(makeGpuImage(std::move(resident)));
    retainedSource = source;

    const VkDevice rawDevice = static_cast<VkDevice>(*control->device);
    const auto* dispatcher = control->device.getDispatcher();
    const VkCommandBuffer rawCommandBuffer = static_cast<VkCommandBuffer>(*commandBuffer);
    const VkFence rawFence = static_cast<VkFence>(*fence);
    if (dispatcher->vkResetFences(rawDevice, 1, &rawFence) != VK_SUCCESS ||
        dispatcher->vkResetCommandBuffer(rawCommandBuffer, 0) != VK_SUCCESS) {
        return makeDiagnostic(GpuAffineDiagnosticCode::DeviceUnavailable,
                              "the affine fence or command buffer could not be reset");
    }

    const GpuImageImpl* const sourceRaw = gpuImageImpl(*retainedSource);
    vk::DescriptorImageInfo sourceInfo{};
    sourceInfo.imageView = sourceRaw->view;
    sourceInfo.imageLayout = vk::ImageLayout::eGeneral;
    vk::DescriptorImageInfo outputInfo{};
    outputInfo.imageView = outputRaw->view;
    outputInfo.imageLayout = vk::ImageLayout::eGeneral;
    vk::DescriptorBufferInfo sampleInfo{};
    sampleInfo.buffer = this->samples.buffer;
    sampleInfo.range = VK_WHOLE_SIZE;
    vk::DescriptorBufferInfo statusInfoWrite{};
    statusInfoWrite.buffer = status.buffer;
    statusInfoWrite.range = VK_WHOLE_SIZE;
    std::array<vk::WriteDescriptorSet, kAffineBindingCount> writes{};
    writes[0] = vk::WriteDescriptorSet{};
    writes[0].dstSet = *affineSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = vk::DescriptorType::eStorageImage;
    writes[0].pImageInfo = &sourceInfo;
    writes[1] = writes[0];
    writes[1].dstBinding = 1;
    writes[1].pImageInfo = &outputInfo;
    writes[2] = vk::WriteDescriptorSet{};
    writes[2].dstSet = *affineSet;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = vk::DescriptorType::eStorageBuffer;
    writes[2].pBufferInfo = &sampleInfo;
    writes[3] = writes[2];
    writes[3].dstBinding = 3;
    writes[3].pBufferInfo = &statusInfoWrite;
    dispatcher->vkUpdateDescriptorSets(rawDevice, static_cast<std::uint32_t>(writes.size()),
                                       reinterpret_cast<const VkWriteDescriptorSet*>(writes.data()),
                                       0, nullptr);

    vk::CommandBufferBeginInfo beginInfo{};
    beginInfo.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
    if (dispatcher->vkBeginCommandBuffer(
            rawCommandBuffer, reinterpret_cast<const VkCommandBufferBeginInfo*>(&beginInfo)) !=
        VK_SUCCESS) {
        return makeDiagnostic(GpuAffineDiagnosticCode::DeviceUnavailable,
                              "the affine command buffer could not begin");
    }
    // The output is fresh (UNDEFINED); the source was produced by an earlier operation and is
    // already GENERAL.
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

    commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, *affine.pipeline);
    commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *affine.pipelineLayout, 0,
                                     {*affineSet}, {});
    const CompositeTranslationPush push{outputWidth, outputHeight, sourceWidth, sourceHeight,
                                        opacity};
    commandBuffer.pushConstants(*affine.pipelineLayout, vk::ShaderStageFlagBits::eCompute, 0,
                                sizeof(CompositeTranslationPush), &push);
    // One invocation per output pixel: X tiles the row in 256-wide groups, Y is the row.
    const auto groups = static_cast<std::uint32_t>(
        compositeDispatchGroupCount(static_cast<std::uint64_t>(outputWidth)));
    commandBuffer.dispatch(groups, outputHeight, 1);

    VkImageMemoryBarrier toRead = outputBarrier;
    toRead.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toRead.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    dispatcher->vkCmdPipelineBarrier(rawCommandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                                     nullptr, 1, &toRead);

    if (dispatcher->vkEndCommandBuffer(rawCommandBuffer) != VK_SUCCESS) {
        return makeDiagnostic(GpuAffineDiagnosticCode::DeviceUnavailable,
                              "the affine command buffer could not end");
    }
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &rawCommandBuffer;
    const VkResult submitted = dispatcher->vkQueueSubmit(
        static_cast<VkQueue>(*control->computeQueue), 1, &submit, rawFence);
    if (submitted == VK_ERROR_DEVICE_LOST) {
        deviceLost = true;
        return makeDiagnostic(GpuAffineDiagnosticCode::DeviceLost,
                              "the device was lost during submission");
    }
    if (submitted != VK_SUCCESS) {
        return makeDiagnostic(GpuAffineDiagnosticCode::DeviceUnavailable,
                              "the affine dispatch could not submit");
    }
    queueSubmitted = true;
    jobState = GpuAffineJobState::Pending;
    jobDiagnostic = GpuAffineDiagnostic{};
    return {};
}

GpuAffinePollResult GpuAffine::poll() {
    if (impl_ == nullptr) {
        return GpuAffinePollResult::Failure;
    }
    Impl& impl = *impl_;
    if (!impl.onOwnerThread()) {
        return GpuAffinePollResult::WrongThread;
    }
#ifdef BLOOM_GPU_SCENE_EXECUTOR_TEST_FAULT_INJECTION
    if (const auto fault = impl.injectedPollFault()) {
        return *fault;
    }
#endif
    if (impl.jobState == GpuAffineJobState::Ready) {
        return GpuAffinePollResult::Ready;
    }
    const bool pending = impl.jobState == GpuAffineJobState::Pending;
    if (!pending && !impl.queueSubmitted) {
        return GpuAffinePollResult::Failure;
    }
    const VkFence rawFence = static_cast<VkFence>(*impl.fence);
    const VkResult status = impl.control->device.getDispatcher()->vkGetFenceStatus(
        static_cast<VkDevice>(*impl.control->device), rawFence);
    if (status == VK_NOT_READY) {
        return pending ? GpuAffinePollResult::Pending : GpuAffinePollResult::Failure;
    }
    if (status == VK_ERROR_DEVICE_LOST) {
        impl.deviceLost = true;
        impl.queueSubmitted = false;
        if (pending) {
            impl.fail(GpuAffineDiagnosticCode::DeviceLost, "the device was lost while polling");
        }
        return GpuAffinePollResult::Failure;
    }
    if (status != VK_SUCCESS) {
        if (pending) {
            impl.fail(GpuAffineDiagnosticCode::DeviceUnavailable,
                      "the affine fence returned an unexpected status; the submission is not "
                      "retired");
        }
        return GpuAffinePollResult::Failure;
    }
    impl.queueSubmitted = false;
    if (!pending) {
        return GpuAffinePollResult::Failure;
    }
    if (impl.discardRequested.load()) {
        impl.clearJob();
        impl.fail(GpuAffineDiagnosticCode::Cancelled, "the affine job was cancelled");
        return GpuAffinePollResult::Failure;
    }
    if (impl.residentImage == nullptr) {
        impl.fail(GpuAffineDiagnosticCode::DeviceUnavailable, "the resident image is missing");
        return GpuAffinePollResult::Failure;
    }
    if (!impl.checkStatusFlag()) {
        return GpuAffinePollResult::Failure;
    }
    impl.jobState = GpuAffineJobState::Ready;
    impl.jobDiagnostic = GpuAffineDiagnostic{};
    return GpuAffinePollResult::Ready;
}

const GpuImage* GpuAffine::image() const noexcept {
    if (impl_ == nullptr || impl_->jobState != GpuAffineJobState::Ready) {
        return nullptr;
    }
    return impl_->residentImage.get();
}

GpuImage GpuAffine::takeImage() noexcept {
    if (impl_ == nullptr || impl_->residentImage == nullptr ||
        impl_->jobState != GpuAffineJobState::Ready) {
        return GpuImage{};
    }
    GpuImage taken = std::move(*impl_->residentImage);
    impl_->clearJob();
    return taken;
}

void GpuAffine::cancel() noexcept {
    if (impl_ != nullptr) {
        impl_->discardRequested.store(true);
    }
}

bool GpuAffine::teardownDrainIncomplete() noexcept { return affineTeardownIncomplete(); }

} // namespace bloom::render
