#pragma once

#include "gpu_device_private.hpp"

#include <utility>

namespace bloom::render::vulkan_detail {

enum class NativeBufferPurpose { Output, Upload, Processing, Presentation };

enum class NativeBufferAllocationOutcome { Allocated, TransferBudgetRefused, PhaseRefused, Failed };

// One actual unbound buffer, original AUTO access/mapping preferences, then bounded compatible
// one-type attempts. The caller owns the token from the first backing call and must free first.
[[nodiscard]] inline NativeBufferAllocationOutcome allocateCoveredBuffer(
    DeviceAllocatorState& state, const VkBufferCreateInfo& bufferInfo,
    const VmaAllocationCreateInfo& original, const std::uint64_t maximumBackingBytes,
    VkBuffer& buffer, VmaAllocation& allocation, VmaAllocationInfo& allocationInfo,
    NativeBufferStorageToken& token, NativeBufferPurpose purpose = NativeBufferPurpose::Output,
    const bool restoreFailedPrefix = false) noexcept {
    const auto release = [&] {
        if (restoreFailedPrefix && purpose == NativeBufferPurpose::Processing)
            static_cast<void>(
                token.releaseFailedAttemptAfterBackingDestroyed(state.processingPhase));
        else
            token = {};
    };
    state.bufferStorageRefusal.reset();
    const bool presentation = purpose == NativeBufferPurpose::Presentation;
    if (presentation)
        state.presentationStorageRefusal.reset();
    if (presentation ? !state.processingRoot : !state.processingPhase) {
        // Explicit rootless compatibility preserves the original allocator route.
        return vmaCreateBuffer(state.allocator, &bufferInfo, &original, &buffer, &allocation,
                               &allocationInfo) == VK_SUCCESS
                   ? NativeBufferAllocationOutcome::Allocated
                   : NativeBufferAllocationOutcome::Failed;
    }
    if (!presentation && (state.processingPhaseUnproven || state.processingPhase->isUnproven()))
        return NativeBufferAllocationOutcome::Failed;
    const auto* dispatch = state.device.getDispatcher();
    const VkDevice device = static_cast<VkDevice>(*state.device);
    if (dispatch->vkCreateBuffer(device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS)
        return NativeBufferAllocationOutcome::Failed;
    VkMemoryDedicatedRequirements dedicated{};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS;
    VkMemoryRequirements2 requirements{};
    requirements.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
    requirements.pNext = &dedicated;
    VkBufferMemoryRequirementsInfo2 query{};
    query.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2;
    query.buffer = buffer;
    dispatch->vkGetBufferMemoryRequirements2(device, &query, &requirements);
    const auto destroyUnbound = [&]() {
        dispatch->vkDestroyBuffer(device, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
    };
    if (requirements.memoryRequirements.size > maximumBackingBytes) {
        destroyUnbound();
        return NativeBufferAllocationOutcome::TransferBudgetRefused;
    }
    const VkPhysicalDeviceMemoryProperties* memory = nullptr;
    vmaGetMemoryProperties(state.allocator, &memory);
    auto remaining = requirements.memoryRequirements.memoryTypeBits;
    if (original.memoryTypeBits != 0)
        remaining &= original.memoryTypeBits;
    if (state.nativeBufferMemoryTypeMaskForTests != 0)
        remaining &= state.nativeBufferMemoryTypeMaskForTests;
    bool attempted = false;
    const bool mapped = (original.flags & VMA_ALLOCATION_CREATE_MAPPED_BIT) != 0;
    const bool hostVisible =
        mapped ||
        (original.flags & (VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                           VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT)) != 0 ||
        (original.requiredFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
    while (remaining != 0) {
        VmaAllocationCreateInfo preferred = original;
        preferred.memoryTypeBits = remaining;
        std::uint32_t selected = 0;
        if (vmaFindMemoryTypeIndexForBufferInfo(state.allocator, &bufferInfo, &preferred,
                                                &selected) != VK_SUCCESS ||
            selected >= memory->memoryTypeCount || selected >= 32)
            break;
        const auto bit = std::uint32_t{1} << selected;
        if ((remaining & bit) == 0)
            break;
        remaining &= ~bit;
        // UNKNOWN ignores HOST_ACCESS_RANDOM; enforce the actual visibility chosen by AUTO.
        if (hostVisible && (memory->memoryTypes[selected].propertyFlags &
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0)
            continue;
        const auto heap = memory->memoryTypes[selected].heapIndex;
        const NativeBufferStorageIdentity identity{
            state.presentationEpoch, selected, heap,
            (memory->memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0};
        auto grant =
            presentation ? NativeBufferStorageToken::reservePresentation(
                               state.processingRoot, requirements.memoryRequirements.size, identity)
            : purpose == NativeBufferPurpose::Processing
                ? NativeBufferStorageToken::reserveProcessing(
                      state.processingPhase, requirements.memoryRequirements.size, identity)
            : purpose == NativeBufferPurpose::Upload
                ? NativeBufferStorageToken::reserveUpload(
                      state.processingPhase, requirements.memoryRequirements.size, identity)
                : NativeBufferStorageToken::reserve(state.processingPhase,
                                                    requirements.memoryRequirements.size, identity);
        if (!grant.token) {
            if (grant.status == GpuProcessingPhaseAdmission::BudgetRefused ||
                grant.status == GpuProcessingPhaseAdmission::AwaitingCapacity)
                state.bufferStorageRefusal = {grant.status, grant.domain,
                                              presentation ? 0 : state.processingPhase->id(),
                                              grant.revisionAfterOwnRelease};
            if (presentation && (grant.status == GpuProcessingPhaseAdmission::BudgetRefused ||
                                 grant.status == GpuProcessingPhaseAdmission::AwaitingCapacity))
                state.presentationStorageRefusal = {grant.status, grant.domain,
                                                    grant.revisionAfterOwnRelease, false};
            continue;
        }
        token = std::move(*grant.token);
        VmaAllocationCreateInfo pinned = preferred;
        pinned.usage = VMA_MEMORY_USAGE_UNKNOWN;
        pinned.memoryTypeBits = bit;
        if (hostVisible)
            pinned.requiredFlags |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        if (dedicated.requiresDedicatedAllocation)
            pinned.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        if (state.nativeBufferAllocationFault == NativeBufferAllocationFault::Allocation) {
            state.nativeBufferAllocationFault = NativeBufferAllocationFault::None;
            destroyUnbound();
            release();
            state.bufferStorageRefusal.reset();
            if (presentation)
                state.presentationStorageRefusal.reset();
            return NativeBufferAllocationOutcome::Failed;
        }
        attempted = true;
        if (state.nativeBufferBackingAttempts != UINT64_MAX)
            ++state.nativeBufferBackingAttempts;
        if (vmaAllocateMemoryForBuffer(state.allocator, buffer, &pinned, &allocation,
                                       &allocationInfo) != VK_SUCCESS) {
            release();
            continue;
        }
        const auto fault =
            std::exchange(state.nativeBufferAllocationFault, NativeBufferAllocationFault::None);
        if (allocationInfo.memoryType != selected ||
            !token.reconcile(fault == NativeBufferAllocationFault::Reconcile ? UINT64_MAX
                                                                             : allocationInfo.size,
                             identity) ||
            fault == NativeBufferAllocationFault::Bind ||
            vmaBindBufferMemory(state.allocator, allocation, buffer) != VK_SUCCESS ||
            fault == NativeBufferAllocationFault::Map ||
            (mapped && allocationInfo.pMappedData == nullptr)) {
            destroyUnbound();
            vmaFreeMemory(state.allocator, allocation);
            if (state.nativeBufferBackingsDestroyed != UINT64_MAX)
                ++state.nativeBufferBackingsDestroyed;
            allocation = VK_NULL_HANDLE;
            release();
            state.bufferStorageRefusal.reset();
            if (presentation)
                state.presentationStorageRefusal.reset();
            return NativeBufferAllocationOutcome::Failed;
        }
        state.bufferStorageRefusal.reset();
        if (presentation)
            state.presentationStorageRefusal.reset();
        if (presentation)
            static_cast<void>(token.promote());
        return NativeBufferAllocationOutcome::Allocated;
    }
    if (attempted)
        state.bufferStorageRefusal.reset();
    if (presentation && attempted)
        state.presentationStorageRefusal.reset();
    destroyUnbound();
    return state.bufferStorageRefusal ? NativeBufferAllocationOutcome::PhaseRefused
                                      : NativeBufferAllocationOutcome::Failed;
}
} // namespace bloom::render::vulkan_detail
