#include "gpu_path_coverage_private.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

// GpuPathCoverage: the native vector coverage producer. The host uploads the bounded
// PathRasterCoverageGeometry (integer sample spans, no CPU pixel mask) and one compute dispatch
// turns it into the exact packed R8 coverage mask the CPU PathRaster::coverageRow produces. The
// mask stays resident for a downstream GpuSolid covered fill; only readback() ever brings it to the
// host. In-flight submissions are serialized by a bounded process-wide reservation so at most one
// unproven submission can ever be retained.

namespace bloom::render {
namespace {

using vulkan_detail::DeviceAllocatorState;
using GpuRendererAccess = bloom::render::GpuRendererAccess;

constexpr std::uint32_t kWorkgroupSizeX = 256;

static_assert(sizeof(PathRasterCoverageRange) == sizeof(std::uint32_t) * 2);
static_assert(sizeof(PathRasterCoverageSpan) == sizeof(std::uint32_t) * 2);
static_assert(std::is_trivially_copyable_v<PathRasterCoverageRange>);
static_assert(std::is_trivially_copyable_v<PathRasterCoverageSpan>);

std::atomic<std::uint64_t> g_nativeDispatchCount{0};

[[nodiscard]] std::uint64_t actualAllocationBytes(DeviceAllocatorState& state,
                                                  const VmaAllocation allocation) noexcept {
    if (allocation == VK_NULL_HANDLE) {
        return 0;
    }
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(state.allocator, allocation, &info);
    return static_cast<std::uint64_t>(info.size);
}

[[nodiscard]] bool createHostStorageBuffer(DeviceAllocatorState& state, const std::uint64_t bytes,
                                           const bool hostRandomAccess,
                                           GpuPathCoverageHostBuffer& out) noexcept {
    if (bytes == 0) {
        return false;
    }
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = bytes;
    bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocationInfo.flags =
        (hostRandomAccess ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
                          : VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT) |
        VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    if (vmaCreateBuffer(state.allocator, &bufferInfo, &allocationInfo, &buffer, &allocation,
                        nullptr) != VK_SUCCESS) {
        return false;
    }
    out.state = &state;
    out.buffer = buffer;
    out.allocation = allocation;
    out.bytes = bytes;
    out.armed = true;
    return true;
}

// The resident result buffer is host-visible (for the test oracle only) and shared, so a consuming
// GpuSolid job can co-own it across the producer's lifetime.
[[nodiscard]] std::shared_ptr<GpuPathCoverageMaskOwner>
createMaskBuffer(const std::shared_ptr<DeviceAllocatorState>& state, const std::uint64_t bytes,
                 const std::uint32_t width, const std::uint32_t height) {
    auto owner = std::make_shared<GpuPathCoverageMaskOwner>();
    GpuPathCoverageHostBuffer buffer;
    if (!createHostStorageBuffer(*state, bytes, true, buffer)) {
        return nullptr;
    }
    owner->state = state;
    owner->buffer = buffer.buffer;
    owner->allocation = buffer.allocation;
    owner->bytes = buffer.bytes;
    owner->width = width;
    owner->height = height;
    owner->generation = state->generation;
    buffer.armed = false;
    return owner;
}

template <typename Dispatcher>
void bufferBarrier(const Dispatcher& dispatcher, VkCommandBuffer commandBuffer, VkBuffer buffer,
                   const VkAccessFlags src, const VkAccessFlags dst,
                   const VkPipelineStageFlags srcStage, const VkPipelineStageFlags dstStage) {
    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = src;
    barrier.dstAccessMask = dst;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = buffer;
    barrier.offset = 0;
    barrier.size = VK_WHOLE_SIZE;
    dispatcher.vkCmdPipelineBarrier(commandBuffer, srcStage, dstStage, 0, 0, nullptr, 1, &barrier, 0,
                                    nullptr);
}

[[nodiscard]] std::uint64_t checkedDivide(const std::uint64_t numerator,
                                          const std::uint64_t denominator) noexcept {
    return denominator == 0 ? 0 : (numerator + denominator - 1) / denominator;
}

} // namespace


GpuPathCoverageCreateResult GpuPathCoverage::create(GpuDevice& device,
                                                    const GpuPathCoverageBudgets& budgets) {
    if (device.state() != GpuDeviceState::Ready) {
        return {nullptr, {GpuPathCoverageDiagnosticCode::DeviceUnavailable,
                          "the GPU device is not Ready"}};
    }
    if (GpuRendererAccess::owner(device) != std::this_thread::get_id()) {
        return {nullptr, {GpuPathCoverageDiagnosticCode::WrongThread,
                          "the GpuPathCoverage pipeline must be created on the device "
                          "owner thread"}};
    }
    // Retire orphaned foreign-released residents on the owner thread so admission recovers.
    path_coverage_detail::drainPathCoverageResidentOrphansOnOwnerThread();
    auto control = GpuRendererAccess::state(device);
    if (control == nullptr) {
        return {nullptr, {GpuPathCoverageDiagnosticCode::DeviceUnavailable,
                          "the GPU device exposes no renderer state"}};
    }
    // Lazy creation: an idle producer allocates no native resources and holds no resident slot.
    auto impl = std::make_unique<GpuPathCoverageImpl>();
    impl->owner = std::this_thread::get_id();
    impl->control = std::move(control);
    impl->budgets = budgets;
    impl->expectedGeneration = impl->control->generation;
    impl->maxWorkGroupCountY =
        impl->control->physicalDevice.getProperties().limits.maxComputeWorkGroupCount[1];
    return {std::unique_ptr<GpuPathCoverage>(new GpuPathCoverage(std::move(impl))),
            GpuPathCoverageDiagnostic{}};
}

GpuPathCoverageJobState GpuPathCoverage::state() const noexcept {
    return impl_ != nullptr ? impl_->jobState : GpuPathCoverageJobState::Idle;
}
const GpuPathCoverageDiagnostic& GpuPathCoverage::diagnostic() const noexcept {
    static const GpuPathCoverageDiagnostic none{};
    return impl_ != nullptr ? impl_->jobDiagnostic : none;
}
bool GpuPathCoverage::isBoundTo(GpuDevice& device) const noexcept {
    if (impl_ == nullptr || !impl_->onOwnerThread()) {
        return false;
    }
    const auto state = GpuRendererAccess::state(device);
    return state != nullptr && state == impl_->control;
}
bool GpuPathCoverage::hasUnretiredSubmission() const noexcept {
    return impl_ != nullptr && impl_->submitted;
}
std::uint64_t GpuPathCoverage::lastJobAllocationBytes() const noexcept {
    return impl_ != nullptr ? impl_->lastJobBytes : 0;
}
std::uint32_t GpuPathCoverage::coverageWidth() const noexcept {
    return impl_ != nullptr && impl_->jobState == GpuPathCoverageJobState::Ready && impl_->mask
               ? impl_->mask->width
               : 0;
}
std::uint32_t GpuPathCoverage::coverageHeight() const noexcept {
    return impl_ != nullptr && impl_->jobState == GpuPathCoverageJobState::Ready && impl_->mask
               ? impl_->mask->height
               : 0;
}
std::uint64_t GpuPathCoverage::nativeDispatchCount() noexcept {
    return g_nativeDispatchCount.load();
}

GpuPathCoverageDiagnostic GpuPathCoverage::begin(const GpuPathCoverageParameters& parameters,
                                                 const PathRasterCoverageGeometry& geometry,
                                                 const std::uint64_t byteBudget) {
    if (impl_ == nullptr) {
        return {GpuPathCoverageDiagnosticCode::DeviceUnavailable,
                "the GpuPathCoverage pipeline is not initialized"};
    }
    GpuPathCoverageImpl& impl = *impl_;
    if (!impl.onOwnerThread()) {
        return {GpuPathCoverageDiagnosticCode::WrongThread,
                "begin must run on the device owner thread"};
    }
    // Opportunistic, non-blocking retirement of orphaned foreign-released residents.
    path_coverage_detail::drainPathCoverageResidentOrphansOnOwnerThread();
    if (impl.deviceLost) {
        return {GpuPathCoverageDiagnosticCode::DeviceLost,
                "the device was lost; this generation must not be reused"};
    }
    if (impl.submitted || impl.jobState == GpuPathCoverageJobState::Pending) {
        return {GpuPathCoverageDiagnosticCode::Busy, "one job is already in flight"};
    }
    const std::uint32_t width = parameters.dataWindow.extent().width();
    const std::uint32_t height = parameters.dataWindow.extent().height();
    if (width == 0 || height == 0 || geometry.width != width || geometry.height != height ||
        geometry.rows.size() != static_cast<std::size_t>(height) * 4ULL) {
        return {GpuPathCoverageDiagnosticCode::InvalidArgument,
                "the coverage geometry does not match the data window"};
    }
    if (geometry.rows.size() > std::numeric_limits<std::uint32_t>::max() ||
        geometry.spans.size() > std::numeric_limits<std::uint32_t>::max()) {
        return {GpuPathCoverageDiagnosticCode::InvalidArgument,
                "the coverage geometry entry count exceeds the 32-bit index bound"};
    }
    const std::uint64_t pixels = static_cast<std::uint64_t>(width) * height;
    const std::uint64_t rangeBytes = static_cast<std::uint64_t>(geometry.rows.size()) * 8ULL;
    const std::uint64_t spanBytes =
        std::max<std::uint64_t>(static_cast<std::uint64_t>(geometry.spans.size()) * 8ULL, 8ULL);
    const std::uint64_t maskBytes = ((pixels + 3ULL) / 4ULL) * 4ULL;
    const std::uint64_t wordCount = (pixels + 3ULL) / 4ULL;
    if (wordCount > std::numeric_limits<std::uint32_t>::max()) {
        return {GpuPathCoverageDiagnosticCode::OverBudget,
                "the coverage mask word count exceeds the 32-bit index bound"};
    }
    // Every range must address spans inside the uploaded array.
    for (const auto& range : geometry.rows) {
        if (static_cast<std::uint64_t>(range.offset) + range.count > geometry.spans.size()) {
            return {GpuPathCoverageDiagnosticCode::InvalidArgument,
                    "a coverage span range escapes the span array"};
        }
    }
    // Explicit caller budgets (no fixed artificial ceiling) plus the genuine device storage range.
    const std::uint64_t geometryAllowance = std::min(impl.budgets.maxGeometryBytes, byteBudget);
    const std::uint64_t coverageAllowance = std::min(impl.budgets.maxCoverageBytes, byteBudget);
    const std::uint64_t retained = rangeBytes + spanBytes + maskBytes;
    if (rangeBytes + spanBytes > geometryAllowance || maskBytes > coverageAllowance ||
        retained < maskBytes || retained > byteBudget) {
        return {GpuPathCoverageDiagnosticCode::OverBudget,
                "the coverage geometry or mask exceeds the configured or requested byte budget"};
    }
    if (rangeBytes > impl.control->maxStorageBufferRange ||
        spanBytes > impl.control->maxStorageBufferRange ||
        maskBytes > impl.control->maxStorageBufferRange) {
        return {GpuPathCoverageDiagnosticCode::OverBudget,
                "a coverage buffer exceeds the device storage buffer range"};
    }
    if (impl.control->generation != impl.expectedGeneration) {
        impl.deviceLost = true;
        return {GpuPathCoverageDiagnosticCode::DeviceLost,
                "the device generation changed; this pipeline must not be reused"};
    }
    // Flattened 2D grid plan: a capacity-valid large geometry is split across Y rather than refused.
    const std::uint64_t totalGroups = checkedDivide(wordCount, kWorkgroupSizeX);
    const std::uint32_t maxX = path_coverage_detail::pathCoverageMaxWorkGroupCountXOverride().load() !=
                                       0
                                   ? path_coverage_detail::pathCoverageMaxWorkGroupCountXOverride()
                                         .load()
                                   : impl.control->maxComputeWorkGroupCountX;
    const std::uint32_t maxY = path_coverage_detail::pathCoverageMaxWorkGroupCountYOverride().load() !=
                                       0
                                   ? path_coverage_detail::pathCoverageMaxWorkGroupCountYOverride()
                                         .load()
                                   : impl.maxWorkGroupCountY;
    if (maxX == 0 || maxY == 0) {
        return {GpuPathCoverageDiagnosticCode::Unsupported,
                "the device reports no usable compute workgroup grid limit"};
    }
    const std::uint64_t groupsX = std::min<std::uint64_t>(totalGroups, maxX);
    const std::uint64_t groupsY = checkedDivide(totalGroups, groupsX);
    if (groupsX == 0 || groupsY > maxY) {
        return {GpuPathCoverageDiagnosticCode::OverBudget,
                "the coverage dispatch exceeds the device workgroup grid limits"};
    }

    // Acquire the bounded resident slot BEFORE the first native allocation, and create the pipeline
    // lazily under it. A full pool refuses cleanly without allocating anything.
    if (!impl.pipelineReady) {
        if (!path_coverage_detail::acquireResidentSlot(&impl)) {
            return {GpuPathCoverageDiagnosticCode::DeviceUnavailable,
                    "the bounded resident pool is full; no coverage native resources were "
                    "allocated"};
        }
        if (!impl.createPipeline()) {
            path_coverage_detail::releaseResidentSlot(&impl);
            return impl.createDiagnostic;
        }
        impl.pipelineReady = true;
    }

    impl.clearJob();
    // Claim the bounded reservation BEFORE any native allocation or submit.
    {
        std::lock_guard lock(path_coverage_detail::reservationMutex());
        path_coverage_detail::Reservation& slot = path_coverage_detail::reservation();
        if (slot.state == path_coverage_detail::ReservationState::Reserved) {
            return {GpuPathCoverageDiagnosticCode::Busy,
                    "the coverage reservation is held by a live submission"};
        }
        if (slot.state == path_coverage_detail::ReservationState::Quarantined) {
            if (slot.ownerThread != std::this_thread::get_id()) {
                return {GpuPathCoverageDiagnosticCode::DeviceUnavailable,
                        "a foreign owner thread holds the coverage quarantine"};
            }
            if (!path_coverage_detail::retireQuarantinedLocked(slot)) {
                return {GpuPathCoverageDiagnosticCode::DeviceUnavailable,
                        "a prior coverage submission is not retired"};
            }
        }
        slot.state = path_coverage_detail::ReservationState::Reserved;
        slot.ownerThread = std::this_thread::get_id();
        ++slot.token;
        if (slot.token == 0) {
            ++slot.token;
        }
        impl.slotToken = slot.token;
    }

    const auto failJob = [&](const GpuPathCoverageDiagnosticCode code,
                             std::string message) -> GpuPathCoverageDiagnostic {
        impl.ranges.release();
        impl.spans.release();
        impl.mask.reset();
        impl.jobBuffer = vk::raii::CommandBuffer{nullptr};
        impl.jobPool = vk::raii::CommandPool{nullptr};
        impl.jobFence = vk::raii::Fence{nullptr};
        impl.releaseClaim();
        impl.fail(code, std::move(message));
        return impl.jobDiagnostic;
    };

    if (!createHostStorageBuffer(*impl.control, rangeBytes, false, impl.ranges)) {
        return failJob(GpuPathCoverageDiagnosticCode::AllocationFailed,
                       "the coverage range buffer could not be allocated");
    }
    if (!createHostStorageBuffer(*impl.control, spanBytes, false, impl.spans)) {
        return failJob(GpuPathCoverageDiagnosticCode::AllocationFailed,
                       "the coverage span buffer could not be allocated");
    }
    impl.mask = createMaskBuffer(impl.control, maskBytes, width, height);
    if (impl.mask == nullptr) {
        return failJob(GpuPathCoverageDiagnosticCode::AllocationFailed,
                       "the resident coverage mask could not be allocated");
    }

    VmaAllocationInfo rangeInfo{};
    VmaAllocationInfo spanInfo{};
    vmaGetAllocationInfo(impl.control->allocator, impl.ranges.allocation, &rangeInfo);
    vmaGetAllocationInfo(impl.control->allocator, impl.spans.allocation, &spanInfo);
    if (rangeInfo.pMappedData == nullptr || spanInfo.pMappedData == nullptr) {
        return failJob(GpuPathCoverageDiagnosticCode::AllocationFailed,
                       "a coverage geometry buffer could not be mapped");
    }
    std::memcpy(rangeInfo.pMappedData, geometry.rows.data(), static_cast<std::size_t>(rangeBytes));
    std::memset(spanInfo.pMappedData, 0, static_cast<std::size_t>(spanBytes));
    if (!geometry.spans.empty()) {
        std::memcpy(spanInfo.pMappedData, geometry.spans.data(),
                    geometry.spans.size() * sizeof(PathRasterCoverageSpan));
    }
    if (vmaFlushAllocation(impl.control->allocator, impl.ranges.allocation, 0, VK_WHOLE_SIZE) !=
            VK_SUCCESS ||
        vmaFlushAllocation(impl.control->allocator, impl.spans.allocation, 0, VK_WHOLE_SIZE) !=
            VK_SUCCESS) {
        return failJob(GpuPathCoverageDiagnosticCode::AllocationFailed,
                       "a coverage geometry buffer could not be flushed");
    }

    const std::uint64_t actualRanges =
        actualAllocationBytes(*impl.control, impl.ranges.allocation);
    const std::uint64_t actualSpans = actualAllocationBytes(*impl.control, impl.spans.allocation);
    const std::uint64_t actualMask = actualAllocationBytes(*impl.control, impl.mask->allocation);
    const std::uint64_t actualGeometry = actualRanges + actualSpans;
    const std::uint64_t actualRetained = actualGeometry + actualMask;
    if (actualGeometry > geometryAllowance || actualMask > coverageAllowance ||
        actualRetained < actualMask || actualRetained > byteBudget) {
        return failJob(GpuPathCoverageDiagnosticCode::OverBudget,
                       "actual VMA allocation sizes exceed the configured or requested byte budget");
    }
    impl.lastJobBytes = actualRetained;

    const VkDevice rawDevice = static_cast<VkDevice>(*impl.control->device);
    const auto* dispatcher = impl.control->device.getDispatcher();
    {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolInfo.queueFamilyIndex = impl.control->computeQueueFamily;
        VkCommandPool rawPool = VK_NULL_HANDLE;
        if (dispatcher->vkCreateCommandPool(rawDevice, &poolInfo, nullptr, &rawPool) != VK_SUCCESS) {
            return failJob(GpuPathCoverageDiagnosticCode::AllocationFailed,
                           "the coverage command pool could not be created");
        }
        impl.jobPool = vk::raii::CommandPool(impl.control->device, rawPool);
    }
    {
        VkCommandBufferAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocateInfo.commandPool = *impl.jobPool;
        allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateInfo.commandBufferCount = 1;
        VkCommandBuffer raw = VK_NULL_HANDLE;
        if (dispatcher->vkAllocateCommandBuffers(rawDevice, &allocateInfo, &raw) != VK_SUCCESS) {
            return failJob(GpuPathCoverageDiagnosticCode::AllocationFailed,
                           "the coverage command buffer could not be allocated");
        }
        impl.jobBuffer = vk::raii::CommandBuffer(impl.control->device, raw, *impl.jobPool);
    }
    {
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence raw = VK_NULL_HANDLE;
        if (dispatcher->vkCreateFence(rawDevice, &fenceInfo, nullptr, &raw) != VK_SUCCESS) {
            return failJob(GpuPathCoverageDiagnosticCode::AllocationFailed,
                           "the coverage fence could not be created");
        }
        impl.jobFence = vk::raii::Fence(impl.control->device, raw);
    }

    vk::DescriptorBufferInfo rangeDescriptor{};
    rangeDescriptor.buffer = impl.ranges.buffer;
    rangeDescriptor.offset = 0;
    rangeDescriptor.range = rangeBytes;
    vk::DescriptorBufferInfo spanDescriptor{};
    spanDescriptor.buffer = impl.spans.buffer;
    spanDescriptor.offset = 0;
    spanDescriptor.range = spanBytes;
    vk::DescriptorBufferInfo maskDescriptor{};
    maskDescriptor.buffer = impl.mask->buffer;
    maskDescriptor.offset = 0;
    maskDescriptor.range = maskBytes;
    std::array<vk::WriteDescriptorSet, 3> writes{};
    writes[0].dstSet = *impl.descriptorSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = vk::DescriptorType::eStorageBuffer;
    writes[0].pBufferInfo = &rangeDescriptor;
    writes[1].dstSet = *impl.descriptorSet;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = vk::DescriptorType::eStorageBuffer;
    writes[1].pBufferInfo = &spanDescriptor;
    writes[2].dstSet = *impl.descriptorSet;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = vk::DescriptorType::eStorageBuffer;
    writes[2].pBufferInfo = &maskDescriptor;
    dispatcher->vkUpdateDescriptorSets(rawDevice, static_cast<std::uint32_t>(writes.size()),
                                       reinterpret_cast<const VkWriteDescriptorSet*>(writes.data()),
                                       0, nullptr);

    const VkCommandBuffer rawCommandBuffer = static_cast<VkCommandBuffer>(*impl.jobBuffer);
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (dispatcher->vkBeginCommandBuffer(rawCommandBuffer, &beginInfo) != VK_SUCCESS) {
        return failJob(GpuPathCoverageDiagnosticCode::DeviceUnavailable,
                       "the coverage command buffer could not begin");
    }

    bufferBarrier(*dispatcher, rawCommandBuffer, impl.ranges.buffer, VK_ACCESS_HOST_WRITE_BIT,
                  VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    bufferBarrier(*dispatcher, rawCommandBuffer, impl.spans.buffer, VK_ACCESS_HOST_WRITE_BIT,
                  VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    impl.jobBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, *impl.pipeline);
    impl.jobBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *impl.pipelineLayout, 0,
                                      {*impl.descriptorSet}, {});
    GpuPathCoveragePushConstants push{};
    push.width = width;
    push.height = height;
    push.groupsX = static_cast<std::uint32_t>(groupsX);
    impl.jobBuffer.pushConstants(*impl.pipelineLayout, vk::ShaderStageFlagBits::eCompute, 0,
                                 static_cast<std::uint32_t>(sizeof(GpuPathCoveragePushConstants)),
                                 &push);
    impl.jobBuffer.dispatch(static_cast<std::uint32_t>(groupsX),
                            static_cast<std::uint32_t>(groupsY), 1);

    // Make the produced mask available to a later consumer submission.
    bufferBarrier(*dispatcher, rawCommandBuffer, impl.mask->buffer, VK_ACCESS_SHADER_WRITE_BIT,
                  VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    if (dispatcher->vkEndCommandBuffer(rawCommandBuffer) != VK_SUCCESS) {
        return failJob(GpuPathCoverageDiagnosticCode::DeviceUnavailable,
                       "the coverage command buffer could not end");
    }
    if (static_cast<path_coverage_detail::PathCoverageFault>(
            path_coverage_detail::pathCoverageFault().load()) ==
        path_coverage_detail::PathCoverageFault::FailSubmit) {
        return failJob(GpuPathCoverageDiagnosticCode::DeviceUnavailable,
                       "the coverage submission was forced to fail");
    }
    const VkFence rawFence = static_cast<VkFence>(*impl.jobFence);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &rawCommandBuffer;
    const VkResult submitted = dispatcher->vkQueueSubmit(
        static_cast<VkQueue>(*impl.control->computeQueue), 1, &submit, rawFence);
    if (submitted == VK_ERROR_DEVICE_LOST) {
        impl.deviceLost = true;
        return failJob(GpuPathCoverageDiagnosticCode::DeviceLost,
                       "the device was lost during submission");
    }
    if (submitted != VK_SUCCESS) {
        return failJob(GpuPathCoverageDiagnosticCode::DeviceUnavailable,
                       "the coverage dispatch could not submit");
    }
    g_nativeDispatchCount.fetch_add(1);
    impl.submitted = true;
    impl.jobState = GpuPathCoverageJobState::Pending;
    impl.submittedAt = std::chrono::steady_clock::now();
    impl.jobDiagnostic = {};
    return {};
}


} // namespace bloom::render
