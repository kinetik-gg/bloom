#include <bloom/render/gpu_blend.hpp>

#include "gpu_blend_private.hpp"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace bloom::render {
namespace {

constexpr std::uint64_t kDrainTimeoutNanoseconds = 2ULL * 1000ULL * 1000ULL * 1000ULL;
constexpr std::uint64_t kMaxImageBytes = 256ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxMetadataBytes = 16ULL * 1024ULL * 1024ULL;

[[nodiscard]] GpuBlendDiagnostic makeDiagnostic(const GpuBlendDiagnosticCode code,
                                                std::string message) {
    return blendDiagnostic(code, std::move(message));
}

// The kernel adds a per-axis int32 source offset to an int32 destination-local coordinate. An
// int64 origin difference can be far outside int32 (even overflowing int64), and narrowing it would
// silently wrap the source position and report a false overlap. This computes either an exact
// representable offset for a genuinely overlapping axis, or a sentinel guaranteed to be out of the
// source window for a non-overlapping/unrepresentable one, and fails closed when the extents are
// too large for destLocal + offset to stay in int32.
[[nodiscard]] bool safeSourceOffset(const std::int64_t destinationOrigin,
                                    const std::int64_t sourceOrigin,
                                    const std::uint32_t outputExtent,
                                    const std::uint32_t sourceExtent, std::int32_t& out) noexcept {
    const auto outputExtent64 = static_cast<std::int64_t>(outputExtent);
    const auto sourceExtent64 = static_cast<std::int64_t>(sourceExtent);
    if (outputExtent64 + sourceExtent64 >
        static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max())) {
        return false;
    }
    std::int64_t delta = 0;
    // Overlap on this axis is exactly delta in (-outputExtent, sourceExtent); a subtraction
    // overflow or anything outside that range cannot overlap and is forced transparent.
    if (__builtin_sub_overflow(destinationOrigin, sourceOrigin, &delta) ||
        delta <= -outputExtent64 || delta >= sourceExtent64) {
        out = static_cast<std::int32_t>(-(outputExtent64 + sourceExtent64));
        return true;
    }
    out = static_cast<std::int32_t>(delta);
    return true;
}

} // namespace

void GpuBlend::Impl::fail(const GpuBlendDiagnosticCode code, std::string message) {
    jobState = GpuBlendJobState::Failure;
    jobDiagnostic = blendDiagnostic(code, std::move(message));
}

void GpuBlend::Impl::clearJob() {
    jobState = GpuBlendJobState::Idle;
    jobDiagnostic = GpuBlendDiagnostic{};
    discardRequested.store(false);
    residentImage.reset();
    retainedSource.reset();
    retainedDestination.reset();
    status.release();
    statusMapped = nullptr;
}

GpuBlend::GpuBlend(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
GpuBlend::GpuBlend(GpuBlend&& other) noexcept = default;
GpuBlend& GpuBlend::operator=(GpuBlend&& other) noexcept {
    if (this != &other) {
        releaseImpl();
        impl_ = std::move(other.impl_);
    }
    return *this;
}
GpuBlend::~GpuBlend() { releaseImpl(); }

void GpuBlend::releaseImpl() noexcept {
    if (impl_ == nullptr) {
        return;
    }
    if (!impl_->onOwnerThread() || !impl_->drainAndRetire()) {
        noteCompositeQuarantine();
        [[maybe_unused]] const auto* const quarantined = impl_.release();
        return;
    }
    impl_.reset();
}

GpuBlendJobState GpuBlend::state() const noexcept {
    return impl_ != nullptr ? impl_->jobState : GpuBlendJobState::Idle;
}
const GpuBlendDiagnostic& GpuBlend::diagnostic() const noexcept {
    static const GpuBlendDiagnostic none{};
    if (impl_ == nullptr) {
        return none;
    }
    return impl_->jobDiagnostic;
}
bool GpuBlend::isBoundTo(GpuDevice& device) const noexcept {
    if (impl_ == nullptr || !impl_->onOwnerThread()) {
        return false;
    }
    const auto deviceState = GpuRendererAccess::state(device);
    return deviceState != nullptr && deviceState == impl_->control;
}

bool GpuBlend::hasUnretiredSubmission() const noexcept {
    return impl_ != nullptr && impl_->queueSubmitted;
}

std::uint64_t GpuBlend::lastJobAllocationBytes() const noexcept {
    return impl_ != nullptr ? impl_->lastJobBytes : 0;
}

bool GpuBlend::Impl::drainAndRetire() noexcept {
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

GpuBlend::Impl::~Impl() {
    assert(owner == std::this_thread::get_id());
    retainedSource.reset();
    retainedDestination.reset();
    residentImage.reset();
}

bool GpuBlend::Impl::createPipeline() {
    constexpr bool kBlendBindings[kBlendBindingCount] = {true, true, true, false};
    std::string reason;
    if (!createCompositePipeline(*control, vulkan_detail::kBlendSpirvCode,
                                 vulkan_detail::kBlendSpirvByteCount, kBlendBindings,
                                 kBlendBindingCount, kBlendPushBytes, reason, pipeline)) {
        createDiagnostic = makeDiagnostic(GpuBlendDiagnosticCode::ShaderRejected, reason);
        return false;
    }
    // The exact Float64 companion is built only when the device advertised and enabled the core
    // shaderFloat64 feature. The SPIR-V requires that capability, so it must not be created
    // otherwise. If the device claims support but rejects the pipeline, creation fails closed
    // rather than silently losing the exactness the general modes need.
    if (control->shaderFloat64) {
        if (!createCompositePipeline(*control, vulkan_detail::kBlendF64SpirvCode,
                                     vulkan_detail::kBlendF64SpirvByteCount, kBlendBindings,
                                     kBlendBindingCount, kBlendPushBytes, reason, pipelineF64)) {
            createDiagnostic = makeDiagnostic(GpuBlendDiagnosticCode::ShaderRejected, reason);
            return false;
        }
        f64Available = true;
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
        createDiagnostic = makeDiagnostic(GpuBlendDiagnosticCode::AllocationFailed,
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
        createDiagnostic = makeDiagnostic(GpuBlendDiagnosticCode::AllocationFailed,
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
        createDiagnostic = makeDiagnostic(GpuBlendDiagnosticCode::AllocationFailed,
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
        createDiagnostic = makeDiagnostic(GpuBlendDiagnosticCode::AllocationFailed,
                                          "the blend command buffer could not be allocated");
        return false;
    }
    commandBuffer = vk::raii::CommandBuffer(control->device, rawCommandBuffer, *commandPool);
    vk::FenceCreateInfo fenceInfo{};
    VkFence rawFence = VK_NULL_HANDLE;
    if (dispatcher->vkCreateFence(rawDevice, reinterpret_cast<const VkFenceCreateInfo*>(&fenceInfo),
                                  nullptr, &rawFence) != VK_SUCCESS) {
        createDiagnostic = makeDiagnostic(GpuBlendDiagnosticCode::AllocationFailed,
                                          "the blend fence could not be created");
        return false;
    }
    fence = vk::raii::Fence(control->device, rawFence);
    return true;
}

GpuBlendCreateResult GpuBlend::create(GpuDevice& device, const GpuBlendBudgets& budgets) {
    if (budgets.maxImageBytes == 0 || budgets.maxImageBytes > kMaxImageBytes ||
        budgets.maxMetadataBytes == 0 || budgets.maxMetadataBytes > kMaxMetadataBytes) {
        return {nullptr, makeDiagnostic(GpuBlendDiagnosticCode::InvalidArgument,
                                        "the blend budget is out of range")};
    }
    if (device.state() != GpuDeviceState::Ready) {
        return {nullptr, makeDiagnostic(GpuBlendDiagnosticCode::DeviceUnavailable,
                                        "the GPU device is not Ready")};
    }
    if (GpuRendererAccess::owner(device) != std::this_thread::get_id()) {
        return {nullptr, makeDiagnostic(GpuBlendDiagnosticCode::WrongThread,
                                        "the blend pipeline must be created on the device owner "
                                        "thread")};
    }
    if (!compositeQuarantineAllowed()) {
        return {nullptr, makeDiagnostic(GpuBlendDiagnosticCode::DeviceUnavailable,
                                        "too many undrained GPU generations are quarantined")};
    }
    auto control = GpuRendererAccess::state(device);
    if (control == nullptr) {
        return {nullptr, makeDiagnostic(GpuBlendDiagnosticCode::DeviceUnavailable,
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
    return {std::unique_ptr<GpuBlend>(new GpuBlend(std::move(impl))), GpuBlendDiagnostic{}};
}

bool GpuBlend::Impl::checkStatusFlag() {
    if (statusMapped == nullptr) {
        return true;
    }
    std::uint32_t flag = 0;
    std::memcpy(&flag, statusMapped, sizeof(flag));
    if (flag != 0) {
        fail(GpuBlendDiagnosticCode::StatusFlagRejected,
             "the blend kernel rejected the frame (non-finite or subnormal input); use the CPU "
             "reference path");
        return false;
    }
    return true;
}

GpuBlendDiagnostic GpuBlend::beginBlend(const GpuBlendParameters& parameters,
                                        const std::uint64_t byteBudget) {
    if (impl_ == nullptr) {
        return makeDiagnostic(GpuBlendDiagnosticCode::DeviceUnavailable,
                              "the blend pipeline is not initialized");
    }
    Impl& impl = *impl_;
    if (!impl.onOwnerThread()) {
        return makeDiagnostic(GpuBlendDiagnosticCode::WrongThread,
                              "beginBlend must run on the device owner thread");
    }
    if (impl.deviceLost) {
        return makeDiagnostic(GpuBlendDiagnosticCode::DeviceLost,
                              "the device was lost; this generation must not be reused");
    }
    if (impl.queueSubmitted || impl.jobState == GpuBlendJobState::Pending) {
        return makeDiagnostic(GpuBlendDiagnosticCode::Busy, "one job is already in flight");
    }
    if (parameters.source == nullptr || !parameters.source->isValid() ||
        parameters.destination == nullptr || !parameters.destination->isValid()) {
        return makeDiagnostic(GpuBlendDiagnosticCode::InvalidArgument,
                              "the blend inputs are missing");
    }
    const auto sourceWindow = parameters.source->dataWindow();
    const auto destinationWindow = parameters.destination->dataWindow();
    if (!sourceWindow.has_value() || !destinationWindow.has_value()) {
        return makeDiagnostic(GpuBlendDiagnosticCode::InvalidArgument,
                              "a blend input has no data window");
    }
    const std::uint32_t destWidth = destinationWindow->extent().width();
    const std::uint32_t destHeight = destinationWindow->extent().height();
    const std::uint32_t sourceWidth = sourceWindow->extent().width();
    const std::uint32_t sourceHeight = sourceWindow->extent().height();
    if (destWidth == 0 || destHeight == 0 || sourceWidth == 0 || sourceHeight == 0) {
        return makeDiagnostic(GpuBlendDiagnosticCode::InvalidArgument, "an extent is empty");
    }
    // A BlendMode that does not round-trip through the durable stored mapping is a caller bug, not
    // a request to render Normal. Reject it here: the shader's separable helper falls through to
    // the source value for an unknown mode, so it must never receive one.
    const std::int64_t storedMode = core::blendModeStoredValue(parameters.mode);
    if (!core::blendModeFromStoredValue(storedMode).has_value()) {
        return makeDiagnostic(GpuBlendDiagnosticCode::InvalidArgument,
                              "the blend mode is not a known core::BlendMode value");
    }
    // Resolve the int32 shader offsets safely BEFORE any driver resource is created. A huge int64
    // origin difference is either represented exactly (overlap) or forced transparent (no overlap),
    // never narrowed and wrapped.
    std::int32_t sourceOffsetX = 0;
    std::int32_t sourceOffsetY = 0;
    if (!safeSourceOffset(destinationWindow->originX(), sourceWindow->originX(), destWidth,
                          sourceWidth, sourceOffsetX) ||
        !safeSourceOffset(destinationWindow->originY(), sourceWindow->originY(), destHeight,
                          sourceHeight, sourceOffsetY)) {
        return makeDiagnostic(GpuBlendDiagnosticCode::Unsupported,
                              "the source/destination extents are too large for a safe int32 "
                              "source offset");
    }
    std::uint64_t imageBytes = 0;
    if (compositeMultiplyOverflows(destWidth, destHeight, imageBytes) ||
        compositeMultiplyOverflows(imageBytes, sizeof(Rgba32f), imageBytes)) {
        return makeDiagnostic(GpuBlendDiagnosticCode::OverBudget,
                              "the blend request overflows the byte arithmetic");
    }
    const std::uint64_t allowedImage =
        byteBudget < impl.budgets.maxImageBytes ? byteBudget : impl.budgets.maxImageBytes;
    if (imageBytes > allowedImage || sizeof(std::uint32_t) > impl.budgets.maxMetadataBytes) {
        return makeDiagnostic(GpuBlendDiagnosticCode::OverBudget,
                              "the blend image or metadata exceeds the byte budget");
    }
    if (impl.control->generation != impl.expectedGeneration) {
        impl.deviceLost = true;
        return makeDiagnostic(GpuBlendDiagnosticCode::DeviceLost,
                              "the device generation changed; this pipeline must not be reused");
    }
    // Both inputs must belong to exactly this device generation BEFORE any driver resource is
    // created or bound.
    if (!compositeImageBelongsTo(gpuImageImpl(*parameters.source), impl.control) ||
        !compositeImageBelongsTo(gpuImageImpl(*parameters.destination), impl.control)) {
        return makeDiagnostic(GpuBlendDiagnosticCode::InvalidArgument,
                              "a blend input image does not belong to this device");
    }
    const SolidImageSupport support = querySolidImageSupport(*impl.control, destWidth, destHeight);
    if (!support.supported || imageBytes > support.maxImageBytes) {
        return makeDiagnostic(support.supported ? GpuBlendDiagnosticCode::OverBudget
                                                : GpuBlendDiagnosticCode::Unsupported,
                              support.supported ? "the destination image exceeds the device limit"
                                                : support.reason);
    }

    // Normal is the exact retained fma source-over and Add is the exact premultiplied sum, so both
    // stay on the Float32 kernel. The six general separable modes divide by alpha and can cancel
    // large HDR operands to a near-zero result, which needs the Float64 kernel to hold the 2e-6
    // gate. Without shaderFloat64 those modes report Unsupported and the caller keeps the CPU
    // reference path rather than publishing Float32 pixels outside tolerance.
    const bool needsF64 =
        parameters.mode != core::BlendMode::Normal && parameters.mode != core::BlendMode::Add;
    if (needsF64 && !impl.f64Available) {
        return makeDiagnostic(GpuBlendDiagnosticCode::Unsupported,
                              "this device does not support shaderFloat64; the general blend modes "
                              "need the CPU reference path");
    }
    const CompositePipeline& selected = needsF64 ? impl.pipelineF64 : impl.pipeline;

    impl.clearJob();
    const std::uint32_t zeroFlag = 0;
    if (!createCompositeBuffer(*impl.control, sizeof(zeroFlag), true, &zeroFlag, impl.status)) {
        return makeDiagnostic(GpuBlendDiagnosticCode::AllocationFailed,
                              "the status buffer could not be allocated");
    }
    VmaAllocationInfo statusInfo{};
    vmaGetAllocationInfo(impl.control->allocator, impl.status.allocation, &statusInfo);
    impl.statusMapped = statusInfo.pMappedData;

    // The destination is the read-only BACKDROP; the operation writes its own resident OUTPUT
    // image, so neither input is mutated and takeImage() returns a real result.
    const GpuImageImpl* const destRaw = gpuImageImpl(*parameters.destination);
    auto resident = std::make_unique<GpuImageImpl>();
    resident->state = impl.control;
    resident->dataWindow = parameters.destination->dataWindow();
    resident->displayWindow = parameters.destination->displayWindow();
    resident->pixelAspect = parameters.destination->pixelAspect();
    resident->generation = impl.control->generation;
    if (!createResidentImage(*impl.control, destWidth, destHeight, *resident)) {
        return makeDiagnostic(GpuBlendDiagnosticCode::AllocationFailed,
                              "the blend output image could not be allocated");
    }
    GpuImageImpl* const outputRaw = resident.get();

    // Enforce the budget on the ACTUAL VMA allocation sizes (allocator rounding included). The
    // inputs' retained allocations belong to the caller/cache and are deliberately not counted.
    std::uint64_t retainedActual = 0;
    {
        const std::uint64_t outputActual = compositeImageAllocationBytes(*outputRaw);
        const std::uint64_t statusActual = compositeBufferAllocationBytes(impl.status);
        if (compositeAddOverflows(outputActual, statusActual, retainedActual)) {
            return makeDiagnostic(GpuBlendDiagnosticCode::OverBudget,
                                  "the blend allocation sizes overflow the byte arithmetic");
        }
    }
    if (retainedActual > byteBudget) {
        return makeDiagnostic(GpuBlendDiagnosticCode::OverBudget,
                              "the actual blend allocations exceed the byte budget");
    }
    impl.lastJobBytes = retainedActual;

    impl.residentImage = std::make_unique<GpuImage>(makeGpuImage(std::move(resident)));
    impl.retainedSource = parameters.source;
    impl.retainedDestination = parameters.destination;

    const VkDevice rawDevice = static_cast<VkDevice>(*impl.control->device);
    const auto* dispatcher = impl.control->device.getDispatcher();
    const VkCommandBuffer rawCommandBuffer = static_cast<VkCommandBuffer>(*impl.commandBuffer);
    const VkFence rawFence = static_cast<VkFence>(*impl.fence);
    if (dispatcher->vkResetFences(rawDevice, 1, &rawFence) != VK_SUCCESS ||
        dispatcher->vkResetCommandBuffer(rawCommandBuffer, 0) != VK_SUCCESS) {
        return makeDiagnostic(GpuBlendDiagnosticCode::DeviceUnavailable,
                              "the blend fence or command buffer could not be reset");
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
    std::array<vk::WriteDescriptorSet, kBlendBindingCount> writes{};
    writes[0] = vk::WriteDescriptorSet{};
    writes[0].dstSet = *impl.descriptorSet;
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
    writes[3].dstSet = *impl.descriptorSet;
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
        return makeDiagnostic(GpuBlendDiagnosticCode::DeviceUnavailable,
                              "the blend command buffer could not begin");
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

    impl.commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, *selected.pipeline);
    impl.commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *selected.pipelineLayout,
                                          0, {*impl.descriptorSet}, {});
    const BlendPush push{destWidth,
                         destHeight,
                         sourceWidth,
                         sourceHeight,
                         sourceOffsetX,
                         sourceOffsetY,
                         static_cast<std::uint32_t>(storedMode)};
    impl.commandBuffer.pushConstants(*selected.pipelineLayout, vk::ShaderStageFlagBits::eCompute, 0,
                                     sizeof(BlendPush), &push);
    // One invocation per destination pixel: X tiles the row in 256-wide groups, Y is the row.
    const auto groups = static_cast<std::uint32_t>(
        compositeDispatchGroupCount(static_cast<std::uint64_t>(destWidth)));
    impl.commandBuffer.dispatch(groups, destHeight, 1);

    VkImageMemoryBarrier toRead = outputBarrier;
    toRead.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    // The output was transitioned to GENERAL by the first barrier and the compute shader has since
    // written it. This ordering barrier is GENERAL -> GENERAL: inheriting outputBarrier's
    // UNDEFINED old layout would discard the just-written contents and rely on driver retention.
    toRead.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    dispatcher->vkCmdPipelineBarrier(rawCommandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                                     nullptr, 1, &toRead);

    if (dispatcher->vkEndCommandBuffer(rawCommandBuffer) != VK_SUCCESS) {
        return makeDiagnostic(GpuBlendDiagnosticCode::DeviceUnavailable,
                              "the blend command buffer could not end");
    }
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &rawCommandBuffer;
    const VkResult submitted = dispatcher->vkQueueSubmit(
        static_cast<VkQueue>(*impl.control->computeQueue), 1, &submit, rawFence);
    if (submitted == VK_ERROR_DEVICE_LOST) {
        impl.deviceLost = true;
        return makeDiagnostic(GpuBlendDiagnosticCode::DeviceLost,
                              "the device was lost during submission");
    }
    if (submitted != VK_SUCCESS) {
        return makeDiagnostic(GpuBlendDiagnosticCode::DeviceUnavailable,
                              "the blend dispatch could not submit");
    }
    impl.queueSubmitted = true;
    impl.jobState = GpuBlendJobState::Pending;
    impl.jobDiagnostic = GpuBlendDiagnostic{};
    return {};
}

GpuBlendPollResult GpuBlend::poll() {
    if (impl_ == nullptr) {
        return GpuBlendPollResult::Failure;
    }
    Impl& impl = *impl_;
    if (!impl.onOwnerThread()) {
        return GpuBlendPollResult::WrongThread;
    }
#ifdef BLOOM_GPU_SCENE_EXECUTOR_TEST_FAULT_INJECTION
    if (const auto fault = impl.injectedPollFault()) {
        return *fault;
    }
#endif
    if (impl.jobState == GpuBlendJobState::Ready) {
        return GpuBlendPollResult::Ready;
    }
    const bool pending = impl.jobState == GpuBlendJobState::Pending;
    if (!pending && !impl.queueSubmitted) {
        return GpuBlendPollResult::Failure;
    }
    const VkFence rawFence = static_cast<VkFence>(*impl.fence);
    const VkResult status = impl.control->device.getDispatcher()->vkGetFenceStatus(
        static_cast<VkDevice>(*impl.control->device), rawFence);
    if (status == VK_NOT_READY) {
        return pending ? GpuBlendPollResult::Pending : GpuBlendPollResult::Failure;
    }
    if (status == VK_ERROR_DEVICE_LOST) {
        impl.deviceLost = true;
        impl.queueSubmitted = false;
        if (pending) {
            impl.fail(GpuBlendDiagnosticCode::DeviceLost, "the device was lost while polling");
        }
        return GpuBlendPollResult::Failure;
    }
    if (status != VK_SUCCESS) {
        if (pending) {
            impl.fail(GpuBlendDiagnosticCode::DeviceUnavailable,
                      "the blend fence returned an unexpected status; the submission is not "
                      "retired");
        }
        return GpuBlendPollResult::Failure;
    }
    impl.queueSubmitted = false;
    if (!pending) {
        return GpuBlendPollResult::Failure;
    }
    if (impl.discardRequested.load()) {
        impl.clearJob();
        impl.fail(GpuBlendDiagnosticCode::Cancelled, "the blend job was cancelled");
        return GpuBlendPollResult::Failure;
    }
    if (impl.residentImage == nullptr) {
        impl.fail(GpuBlendDiagnosticCode::DeviceUnavailable, "the resident image is missing");
        return GpuBlendPollResult::Failure;
    }
    // The status flag is only meaningful after the fence; a nonzero flag rejects the frame.
    if (!impl.checkStatusFlag()) {
        return GpuBlendPollResult::Failure;
    }
    impl.jobState = GpuBlendJobState::Ready;
    impl.jobDiagnostic = GpuBlendDiagnostic{};
    return GpuBlendPollResult::Ready;
}

const GpuImage* GpuBlend::image() const noexcept {
    if (impl_ == nullptr || impl_->jobState != GpuBlendJobState::Ready) {
        return nullptr;
    }
    return impl_->residentImage.get();
}

GpuImage GpuBlend::takeImage() noexcept {
    if (impl_ == nullptr || impl_->residentImage == nullptr ||
        impl_->jobState != GpuBlendJobState::Ready) {
        return GpuImage{};
    }
    GpuImage taken = std::move(*impl_->residentImage);
    impl_->clearJob();
    return taken;
}

void GpuBlend::cancel() noexcept {
    if (impl_ != nullptr) {
        impl_->discardRequested.store(true);
    }
}

bool GpuBlend::teardownDrainIncomplete() noexcept { return compositeTeardownIncomplete(); }

} // namespace bloom::render
