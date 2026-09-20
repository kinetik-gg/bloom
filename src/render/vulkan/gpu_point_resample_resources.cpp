#include "gpu_point_resample_private.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

// Bounded process-global reservation accounting and native per-job resource construction for
// PointResampleV1. Mirrors the accepted combined output-colour readback: one reservation slot
// process-wide, acquired BEFORE any native allocation, with an owner-only recoverable quarantine.

namespace bloom::render {

namespace {

// The Vulkan-guaranteed minimum for maxComputeWorkGroupCount[1]. The device records only its X
// limit, so the Y dimension is planned against this conservative physical lower bound (the real
// limit is always at least this large).
constexpr std::uint64_t kPointResampleMinWorkGroupCountY = 65535;

std::atomic<std::int32_t> g_pointResampleLiveSets{0};
std::atomic<bool> g_pointResampleTeardownIncomplete{false};

} // namespace

void notePointResampleResourceSetCreated() noexcept { g_pointResampleLiveSets.fetch_add(1); }
void notePointResampleResourceSetFreed() noexcept { g_pointResampleLiveSets.fetch_sub(1); }
void notePointResampleQuarantine() noexcept { g_pointResampleTeardownIncomplete.store(true); }
bool pointResampleTeardownIncomplete() noexcept { return g_pointResampleTeardownIncomplete.load(); }

namespace point_resample_detail {

namespace {

// Bounded live-resident pool: a published output image holds one slot until it is taken
// (caller-owned), destroyed on the owner thread, or retained for owner recovery. The pool bounds
// the number of native residents process-wide, so the retained store can always hold them.
constexpr std::int32_t kMaxLiveResidents = 8;
std::atomic<std::int32_t> g_liveResidents{0};

struct RetainedSlot final {
    bool occupied = false;
    std::shared_ptr<DeviceAllocatorState> deviceState;
    std::unique_ptr<GpuImage> resident;
    std::thread::id ownerThread;
};

// Fixed-capacity, allocation-free retained store. Because every retained resident already holds one
// of the kMaxLiveResidents pool slots, the number of occupied slots can never exceed the pool, so a
// free slot always exists when a resident is retained (the fixed-capacity invariant).
std::mutex g_retainedMutex;
std::array<RetainedSlot, static_cast<std::size_t>(kMaxLiveResidents)> g_retainedSlots{};

} // namespace

bool acquireResidentSlot() noexcept {
    std::int32_t current = g_liveResidents.load(std::memory_order_relaxed);
    while (current < kMaxLiveResidents) {
        if (g_liveResidents.compare_exchange_weak(current, current + 1,
                                                  std::memory_order_acq_rel)) {
            return true;
        }
    }
    return false;
}

void releaseResidentSlot() noexcept { g_liveResidents.fetch_sub(1, std::memory_order_acq_rel); }

std::uint32_t liveResidents() noexcept {
    return static_cast<std::uint32_t>(g_liveResidents.load());
}

bool retainResident(const std::shared_ptr<DeviceAllocatorState>& deviceState,
                    std::unique_ptr<GpuImage>& resident,
                    const std::thread::id ownerThread) noexcept {
    try {
        std::lock_guard lock(g_retainedMutex);
        // Find a free fixed slot FIRST; only then take ownership of the caller's unique_ptr. A full
        // store returns false with the caller's ownership intact, so nothing is destroyed here.
        for (auto& slot : g_retainedSlots) {
            if (!slot.occupied) {
                slot.deviceState = deviceState;
                slot.ownerThread = ownerThread;
                slot.resident = std::move(resident);
                slot.occupied = true;
                return true;
            }
        }
        return false;
    } catch (...) {
        return false;
    }
}

std::uint32_t retainedResidents() noexcept {
    try {
        std::lock_guard lock(g_retainedMutex);
        std::uint32_t count = 0;
        for (const auto& slot : g_retainedSlots) {
            count += slot.occupied ? 1U : 0U;
        }
        return count;
    } catch (...) {
        return 0;
    }
}

std::uint32_t retireRetainedForOwner() noexcept {
    // Fixed-capacity staging, no allocation. Ownership is moved out under the lock and destroyed
    // outside it, on the calling (owner) thread.
    std::array<RetainedSlot, static_cast<std::size_t>(kMaxLiveResidents)> reclaim{};
    std::size_t count = 0;
    try {
        std::lock_guard lock(g_retainedMutex);
        const auto self = std::this_thread::get_id();
        for (auto& slot : g_retainedSlots) {
            if (slot.occupied && slot.ownerThread == self) {
                reclaim[count].deviceState = std::move(slot.deviceState);
                reclaim[count].resident = std::move(slot.resident);
                reclaim[count].ownerThread = slot.ownerThread;
                reclaim[count].occupied = true;
                slot.occupied = false;
                ++count;
            }
        }
    } catch (...) { // NOLINT(bugprone-empty-catch)
        // The mutex acquisition failed; nothing has been destroyed and the slots remain as they
        // were, so this fail-closed path must not terminate the noexcept owner drain.
    }
    for (std::size_t index = 0; index < count; ++index) {
        releaseResidentSlot();
        reclaim[index].resident.reset();
    }
    return static_cast<std::uint32_t>(count);
}

std::atomic<std::uint8_t>& pointResampleFault() noexcept {
    static std::atomic<std::uint8_t> value{0};
    return value;
}

std::atomic<std::uint32_t>& pointResampleForcedMaxWorkGroupCountX() noexcept {
    static std::atomic<std::uint32_t> value{0};
    return value;
}

bool pointResampleQuarantineOccupied() noexcept {
    std::lock_guard lock(reservationMutex());
    return reservation().state == ReservationState::Quarantined;
}

bool pointResampleRetireQuarantineForOwner() noexcept {
    std::lock_guard lock(reservationMutex());
    Reservation& slot = reservation();
    if (slot.state != ReservationState::Quarantined ||
        slot.ownerThread != std::this_thread::get_id()) {
        return false;
    }
    return retireQuarantinedLocked(slot);
}

std::uint32_t pointResampleLiveResourceSets() noexcept {
    return static_cast<std::uint32_t>(g_pointResampleLiveSets.load());
}

std::uint32_t pointResampleLiveResidents() noexcept { return liveResidents(); }

std::uint32_t pointResampleRetainedResidents() noexcept { return retainedResidents(); }

std::uint32_t pointResampleRetireRetainedForOwner() noexcept { return retireRetainedForOwner(); }

} // namespace point_resample_detail

GpuPointResample::Impl::~Impl() {
    // An idle instance holds no native resources and may be destroyed from any thread; a live
    // resource set is only ever destroyed on the owner thread (the foreign path retains the Impl).
    assert(!resourcesLive || owner == std::this_thread::get_id());
    if (resourcesLive) {
        notePointResampleResourceSetFreed();
        resourcesLive = false;
    }
    resources.reset();
}

void GpuPointResample::Impl::freeJobResources() noexcept {
    if (resourcesLive) {
        notePointResampleResourceSetFreed();
        resourcesLive = false;
    }
    resources.reset();
}

void GpuPointResample::Impl::releaseClaim() noexcept {
    if (slotToken == 0) {
        return;
    }
    std::lock_guard lock(point_resample_detail::reservationMutex());
    point_resample_detail::Reservation& slot = point_resample_detail::reservation();
    if (point_resample_detail::isReservationOwnerLocked(slot) && slot.token == slotToken &&
        slot.state == point_resample_detail::ReservationState::Reserved) {
        slot.state = point_resample_detail::ReservationState::Free;
        slot.ownerThread = std::thread::id{};
    }
    slotToken = 0;
}

void GpuPointResample::Impl::releaseRetired() noexcept {
    // The submission is proven retired: free the job resources (releasing the job-set accounting)
    // but keep the published resident and its bounded resident slot for take() or ownership-thread
    // destruction.
    resources.releaseJobOnly();
    if (resourcesLive) {
        notePointResampleResourceSetFreed();
        resourcesLive = false;
    }
    releaseClaim();
}

bool GpuPointResample::Impl::quarantine() noexcept {
    if (!queueSubmitted) {
        releaseRetired();
        return true;
    }
    notePointResampleQuarantine();
    std::lock_guard lock(point_resample_detail::reservationMutex());
    point_resample_detail::Reservation& slot = point_resample_detail::reservation();
    if (!point_resample_detail::isReservationOwnerLocked(slot) || slot.token != slotToken) {
        return false;
    }
    slot.resources = std::move(resources);
    slot.state = point_resample_detail::ReservationState::Quarantined;
    resources = point_resample_detail::ResourceSet{};
    // Ownership of the counted set moved into the quarantine; do not decrement here.
    resourcesLive = false;
    slotToken = 0;
    queueSubmitted = false;
    return true;
}

bool GpuPointResample::Impl::createPipelines() {
    resourcesLive = true;
    resources.deviceState = control;
    notePointResampleResourceSetCreated();
    constexpr bool kBindings[kPointResampleBindingCount] = {true, true, false};
    std::string reason;
    if (!createCompositePipeline(*control, vulkan_detail::kPointResampleSpirvCode,
                                 vulkan_detail::kPointResampleSpirvByteCount, kBindings,
                                 kPointResampleBindingCount, kPointResamplePushBytes, reason,
                                 resources.pipeline)) {
        createDiagnostic =
            pointResampleDiagnostic(GpuPointResampleDiagnosticCode::ShaderRejected, reason);
        return false;
    }

    const std::array poolSizes{vk::DescriptorPoolSize{vk::DescriptorType::eStorageImage, 2},
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
        createDiagnostic =
            pointResampleDiagnostic(GpuPointResampleDiagnosticCode::AllocationFailed,
                                    "the point-resample descriptor pool could not be created");
        return false;
    }
    resources.descriptorPool = vk::raii::DescriptorPool(control->device, rawPool);
    const vk::DescriptorSetLayout setLayout = *resources.pipeline.descriptorSetLayout;
    vk::DescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.descriptorPool = *resources.descriptorPool;
    allocateInfo.descriptorSetCount = 1;
    allocateInfo.pSetLayouts = &setLayout;
    VkDescriptorSet rawSet = VK_NULL_HANDLE;
    if (dispatcher->vkAllocateDescriptorSets(
            rawDevice, reinterpret_cast<const VkDescriptorSetAllocateInfo*>(&allocateInfo),
            &rawSet) != VK_SUCCESS) {
        createDiagnostic =
            pointResampleDiagnostic(GpuPointResampleDiagnosticCode::AllocationFailed,
                                    "the point-resample descriptor set could not be allocated");
        return false;
    }
    resources.descriptorSet =
        vk::raii::DescriptorSet(control->device, rawSet, *resources.descriptorPool);

    vk::CommandPoolCreateInfo commandPoolInfo{};
    commandPoolInfo.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
    commandPoolInfo.queueFamilyIndex = control->computeQueueFamily;
    VkCommandPool rawCommandPool = VK_NULL_HANDLE;
    if (dispatcher->vkCreateCommandPool(
            rawDevice, reinterpret_cast<const VkCommandPoolCreateInfo*>(&commandPoolInfo), nullptr,
            &rawCommandPool) != VK_SUCCESS) {
        createDiagnostic =
            pointResampleDiagnostic(GpuPointResampleDiagnosticCode::AllocationFailed,
                                    "the point-resample command pool could not be created");
        return false;
    }
    resources.commandPool = vk::raii::CommandPool(control->device, rawCommandPool);
    vk::CommandBufferAllocateInfo commandBufferInfo{};
    commandBufferInfo.commandPool = *resources.commandPool;
    commandBufferInfo.level = vk::CommandBufferLevel::ePrimary;
    commandBufferInfo.commandBufferCount = 1;
    VkCommandBuffer rawCommandBuffer = VK_NULL_HANDLE;
    if (dispatcher->vkAllocateCommandBuffers(
            rawDevice, reinterpret_cast<const VkCommandBufferAllocateInfo*>(&commandBufferInfo),
            &rawCommandBuffer) != VK_SUCCESS) {
        createDiagnostic =
            pointResampleDiagnostic(GpuPointResampleDiagnosticCode::AllocationFailed,
                                    "the point-resample command buffer could not be allocated");
        return false;
    }
    resources.commandBuffer =
        vk::raii::CommandBuffer(control->device, rawCommandBuffer, *resources.commandPool);
    vk::FenceCreateInfo fenceInfo{};
    VkFence rawFence = VK_NULL_HANDLE;
    if (dispatcher->vkCreateFence(rawDevice, reinterpret_cast<const VkFenceCreateInfo*>(&fenceInfo),
                                  nullptr, &rawFence) != VK_SUCCESS) {
        createDiagnostic = pointResampleDiagnostic(GpuPointResampleDiagnosticCode::AllocationFailed,
                                                   "the point-resample fence could not be created");
        return false;
    }
    resources.fence = vk::raii::Fence(control->device, rawFence);
    return true;
}

bool GpuPointResample::Impl::launchJob(const std::shared_ptr<const GpuImage>& source,
                                       const std::uint32_t outputWidth,
                                       const std::uint32_t outputHeight,
                                       const ImageWindow displayWindow,
                                       const core::PixelAspectRatio pixelAspect,
                                       const std::span<const std::int32_t> axisIndices,
                                       const std::uint64_t byteBudget) {
    const point_resample_detail::PointResampleFault fault =
        static_cast<point_resample_detail::PointResampleFault>(
            point_resample_detail::pointResampleFault().load());

    if (fault == point_resample_detail::PointResampleFault::FailJobAllocation) {
        jobDiagnostic = pointResampleDiagnostic(GpuPointResampleDiagnosticCode::AllocationFailed,
                                                "the point-resample job allocation was forced to "
                                                "fail");
        return false;
    }

    const std::uint64_t axisCount =
        static_cast<std::uint64_t>(outputWidth) + static_cast<std::uint64_t>(outputHeight);
    std::uint64_t metadataBytes = 0;
    if (compositeMultiplyOverflows(axisCount, sizeof(std::int32_t), metadataBytes) ||
        metadataBytes > control->maxStorageBufferRange) {
        jobDiagnostic = pointResampleDiagnostic(
            GpuPointResampleDiagnosticCode::OverBudget,
            "the point-resample axis metadata exceeds the device storage-buffer range");
        return false;
    }
    // Device buffer first so any allocation failure precedes any driver work.
    if (!createCompositeBuffer(*control, metadataBytes, false, axisIndices.data(),
                               resources.axis)) {
        jobDiagnostic = pointResampleDiagnostic(
            GpuPointResampleDiagnosticCode::AllocationFailed,
            "the point-resample axis metadata buffer could not be allocated");
        return false;
    }

    // Bound the number of live native residents process-wide before allocating the output image.
    if (!point_resample_detail::acquireResidentSlot()) {
        jobDiagnostic = pointResampleDiagnostic(
            GpuPointResampleDiagnosticCode::Busy,
            "the bounded live-resident pool is exhausted; take() published results or retire "
            "retained ones");
        return false;
    }
    resources.residentSlotHeld = true;
    const auto dataWindow = ImageWindow::create(0, 0, outputWidth, outputHeight);
    if (!dataWindow) {
        jobDiagnostic = pointResampleDiagnostic(GpuPointResampleDiagnosticCode::InvalidArgument,
                                                "the point-resample output window is invalid");
        return false;
    }
    auto resident = std::make_unique<GpuImageImpl>();
    resident->state = control;
    resident->dataWindow = *dataWindow.value();
    resident->displayWindow = displayWindow;
    resident->pixelAspect = pixelAspect;
    resident->generation = control->generation;
    if (!createResidentImage(*control, outputWidth, outputHeight, *resident)) {
        jobDiagnostic =
            pointResampleDiagnostic(GpuPointResampleDiagnosticCode::AllocationFailed,
                                    "the point-resample output image could not be allocated");
        return false;
    }
    GpuImageImpl* const outputRaw = resident.get();

    // Enforce the per-resource ceilings on the ACTUAL VMA allocations and the combined
    // image/axis/staging bytes against the per-call byteBudget.
    const std::uint64_t outputActual = compositeImageAllocationBytes(*outputRaw);
    const std::uint64_t axisActual = compositeBufferAllocationBytes(resources.axis);
    std::uint64_t retainedActual = 0;
    std::uint64_t peakActual = 0;
    if (compositeAddOverflows(outputActual, axisActual, retainedActual) ||
        compositeAddOverflows(retainedActual, axisActual, peakActual)) {
        jobDiagnostic = pointResampleDiagnostic(GpuPointResampleDiagnosticCode::OverBudget,
                                                "the point-resample allocation sizes overflow the "
                                                "byte arithmetic");
        return false;
    }
    if (outputActual > budgets.maxImageBytes) {
        jobDiagnostic = pointResampleDiagnostic(GpuPointResampleDiagnosticCode::OverBudget,
                                                "the actual output image exceeds maxImageBytes");
        return false;
    }
    if (axisActual > budgets.maxMetadataBytes) {
        jobDiagnostic =
            pointResampleDiagnostic(GpuPointResampleDiagnosticCode::OverBudget,
                                    "the actual axis metadata exceeds maxMetadataBytes");
        return false;
    }
    if (retainedActual > byteBudget || peakActual > byteBudget) {
        jobDiagnostic = pointResampleDiagnostic(
            GpuPointResampleDiagnosticCode::OverBudget,
            "the actual combined point-resample allocations or transient peak exceed the byte "
            "budget");
        return false;
    }

    resources.source = source;
    resources.resident = std::make_unique<GpuImage>(makeGpuImage(std::move(resident)));
    lastJobBytes = retainedActual > peakActual ? retainedActual : peakActual;

    const std::uint64_t totalPixels =
        static_cast<std::uint64_t>(outputWidth) * static_cast<std::uint64_t>(outputHeight);
    if (totalPixels > std::numeric_limits<std::uint32_t>::max()) {
        jobDiagnostic =
            pointResampleDiagnostic(GpuPointResampleDiagnosticCode::OverBudget,
                                    "the point-resample pixel count exceeds the flattened "
                                    "index range");
        return false;
    }
    std::uint64_t effectiveMaxX = control->maxComputeWorkGroupCountX;
    const std::uint32_t forcedMaxX =
        point_resample_detail::pointResampleForcedMaxWorkGroupCountX().load();
    if (forcedMaxX != 0) {
        effectiveMaxX = std::min<std::uint64_t>(effectiveMaxX, forcedMaxX);
    }
    PointResampleDispatch dispatch{};
    if (!planPointResampleDispatch(totalPixels, effectiveMaxX, kPointResampleMinWorkGroupCountY,
                                   dispatch)) {
        jobDiagnostic = pointResampleDiagnostic(
            GpuPointResampleDiagnosticCode::OverBudget,
            "the point-resample dispatch exceeds the device workgroup grid capacity");
        return false;
    }

    const VkDevice rawDevice = static_cast<VkDevice>(*control->device);
    const auto* dispatcher = control->device.getDispatcher();
    const VkCommandBuffer rawCommandBuffer = static_cast<VkCommandBuffer>(*resources.commandBuffer);
    const VkFence rawFence = static_cast<VkFence>(*resources.fence);
    if (dispatcher->vkResetFences(rawDevice, 1, &rawFence) != VK_SUCCESS ||
        dispatcher->vkResetCommandBuffer(rawCommandBuffer, 0) != VK_SUCCESS) {
        jobDiagnostic = pointResampleDiagnostic(
            GpuPointResampleDiagnosticCode::DeviceUnavailable,
            "the point-resample fence or command buffer could not be reset");
        return false;
    }

    const GpuImageImpl* const sourceRaw = gpuImageImpl(*resources.source);
    vk::DescriptorImageInfo sourceInfo{};
    sourceInfo.imageView = sourceRaw->view;
    sourceInfo.imageLayout = vk::ImageLayout::eGeneral;
    vk::DescriptorImageInfo outputInfo{};
    outputInfo.imageView = outputRaw->view;
    outputInfo.imageLayout = vk::ImageLayout::eGeneral;
    vk::DescriptorBufferInfo axisInfo{};
    axisInfo.buffer = resources.axis.buffer;
    axisInfo.range = VK_WHOLE_SIZE;
    std::array<vk::WriteDescriptorSet, kPointResampleBindingCount> writes{};
    writes[0] = vk::WriteDescriptorSet{};
    writes[0].dstSet = *resources.descriptorSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = vk::DescriptorType::eStorageImage;
    writes[0].pImageInfo = &sourceInfo;
    writes[1] = writes[0];
    writes[1].dstBinding = 1;
    writes[1].pImageInfo = &outputInfo;
    writes[2] = vk::WriteDescriptorSet{};
    writes[2].dstSet = *resources.descriptorSet;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = vk::DescriptorType::eStorageBuffer;
    writes[2].pBufferInfo = &axisInfo;
    dispatcher->vkUpdateDescriptorSets(rawDevice, static_cast<std::uint32_t>(writes.size()),
                                       reinterpret_cast<const VkWriteDescriptorSet*>(writes.data()),
                                       0, nullptr);

    vk::CommandBufferBeginInfo beginInfo{};
    beginInfo.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
    if (dispatcher->vkBeginCommandBuffer(
            rawCommandBuffer, reinterpret_cast<const VkCommandBufferBeginInfo*>(&beginInfo)) !=
        VK_SUCCESS) {
        jobDiagnostic =
            pointResampleDiagnostic(GpuPointResampleDiagnosticCode::DeviceUnavailable,
                                    "the point-resample command buffer could not begin");
        return false;
    }
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

    resources.commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute,
                                         *resources.pipeline.pipeline);
    resources.commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
                                               *resources.pipeline.pipelineLayout, 0,
                                               {*resources.descriptorSet}, {});
    const PointResamplePush push{outputWidth, outputHeight};
    resources.commandBuffer.pushConstants(*resources.pipeline.pipelineLayout,
                                          vk::ShaderStageFlagBits::eCompute, 0,
                                          sizeof(PointResamplePush), &push);
    resources.commandBuffer.dispatch(dispatch.groupsX, dispatch.groupsY, 1);

    VkImageMemoryBarrier toRead = outputBarrier;
    toRead.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toRead.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    dispatcher->vkCmdPipelineBarrier(rawCommandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                                     nullptr, 1, &toRead);

    if (dispatcher->vkEndCommandBuffer(rawCommandBuffer) != VK_SUCCESS) {
        jobDiagnostic = pointResampleDiagnostic(GpuPointResampleDiagnosticCode::DeviceUnavailable,
                                                "the point-resample command buffer could not end");
        return false;
    }
    if (fault == point_resample_detail::PointResampleFault::FailSubmit) {
        jobDiagnostic = pointResampleDiagnostic(GpuPointResampleDiagnosticCode::DeviceUnavailable,
                                                "the point-resample submit was forced to fail");
        return false;
    }
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &rawCommandBuffer;
    const VkResult submitted = dispatcher->vkQueueSubmit(
        static_cast<VkQueue>(*control->computeQueue), 1, &submit, rawFence);
    if (submitted == VK_ERROR_DEVICE_LOST) {
        deviceLost = true;
        jobDiagnostic = pointResampleDiagnostic(GpuPointResampleDiagnosticCode::DeviceLost,
                                                "the device was lost during submission");
        return false;
    }
    if (submitted != VK_SUCCESS) {
        jobDiagnostic = pointResampleDiagnostic(GpuPointResampleDiagnosticCode::DeviceUnavailable,
                                                "the point-resample dispatch could not submit");
        return false;
    }
    queueSubmitted = true;
    submittedAt = std::chrono::steady_clock::now();
    return true;
}

} // namespace bloom::render
