// Included in the transfer owner's native fixture; no other buffer creator is adopted here.
struct BufferTestRetirement final {
    std::shared_ptr<PackedHostRetirementOwner> owner = PackedHostRetirementOwner::create();
    ~BufferTestRetirement() {
        owner->beginShutdown();
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!owner->exitAcknowledged() && std::chrono::steady_clock::now() < until)
            std::this_thread::yield();
        if (!owner->exitAcknowledged() || !owner->collect())
            std::terminate();
    }
};
void bufferRequire(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
std::shared_ptr<GpuProcessingPhaseLease> bufferPhase(GpuDevice& device,
                                                     GpuProcessingPhaseBudget& budget,
                                                     std::uint64_t native, std::uint64_t host) {
    auto grant = budget.reserve({.fixedHostBytes = host,
                                 .requestedExecutorBytes = native,
                                 .minimumExecutorBytes = native,
                                 .sourceFree = true});
    bufferRequire(grant.lease && device.retainProcessingPhase(grant.lease),
                  "the exact paired staging phase is admitted and retained");
    return std::move(grant.lease);
}
void bufferFinish(GpuDevice& device, std::shared_ptr<GpuProcessingPhaseLease>& phase,
                  const GpuProcessingPhaseBudget& budget) {
    device.retireProcessingPhase(phase->id(), true);
    phase.reset();
    const auto c = budget.snapshot();
    bufferRequire(c.ownedNativeBufferBytes == 0 && c.ownedNativeBuffers == 0 &&
                      c.ownedNonlocalNativeBufferHostBytes == 0 && c.ownedNativeImages == 0 &&
                      c.ownedNativeImageBytes == 0 && c.ownedNonlocalNativeHostBytes == 0 &&
                      c.ownedPackedHostBytes == 0 && c.ownedPackedHostMetadataBytes == 0 &&
                      c.ownedPackedHostAllocations == 0 && c.totalHostClaimBytes == 0 &&
                      c.totalDeviceClaimBytes == 0 && c.admittedPhases == 0 &&
                      c.unprovenPhases == 0,
                  "actual staging/image/host destruction precedes final phase conservation");
}
struct BufferBackingGuard final {
    explicit BufferBackingGuard(std::shared_ptr<vulkan_detail::DeviceAllocatorState> allocator)
        : state(std::move(allocator)) {}
    std::shared_ptr<vulkan_detail::DeviceAllocatorState> state;
    NativeBufferStorageToken token;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    ~BufferBackingGuard() { clear(); }
    void clear() noexcept {
        if (allocation != VK_NULL_HANDLE) {
            vmaDestroyBuffer(state->allocator, buffer, allocation);
            ++state->nativeBufferBackingsDestroyed;
        }
        allocation = VK_NULL_HANDLE;
        buffer = VK_NULL_HANDLE;
        token = {};
    }
};
VkBufferCreateInfo stagingDescription() {
    return {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .size = 128,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr};
}
VmaAllocationCreateInfo stagingPreferences() {
    VmaAllocationCreateInfo info{};
    info.usage = VMA_MEMORY_USAGE_AUTO;
    info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    return info;
}
void actualBufferStorage(GpuDevice& device) {
    using namespace vulkan_detail;
    auto state = GpuRendererAccess::state(device);
    bufferRequire(bool(state), "actual staging allocator exists");
    GpuProcessingPhaseBudget budget;
    budget.setHostCapacity(1ULL << 30, true);
    budget.setDeviceCapacity(1ULL << 30, true);
    auto phase = bufferPhase(device, budget, 1ULL << 20, 1ULL << 20);
    BufferBackingGuard backing{state};
    VmaAllocationInfo actual{};
    auto info = stagingDescription();
    auto preferences = stagingPreferences();
    bufferRequire(allocateCoveredBuffer(*state, info, preferences, 1ULL << 20, backing.buffer,
                                        backing.allocation, actual,
                                        backing.token) == NativeBufferAllocationOutcome::Allocated,
                  "same-buffer AUTO-ranked UNKNOWN-pinned mapped backing exists");
    const VkPhysicalDeviceMemoryProperties* memory = nullptr;
    vmaGetMemoryProperties(state->allocator, &memory);
    const auto heap = memory->memoryTypes[actual.memoryType].heapIndex;
    const auto identity = backing.token.identity();
    const auto charged = budget.snapshot();
    bufferRequire(
        backing.token.bytes() == actual.size && identity.memoryType == actual.memoryType &&
            identity.heap == heap && identity.allocatorGeneration == device.ownershipEpoch() &&
            identity.deviceLocal ==
                ((memory->memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) &&
            (memory->memoryTypes[actual.memoryType].propertyFlags &
             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0 &&
            actual.pMappedData != nullptr && charged.ownedNativeBufferBytes == actual.size &&
            charged.ownedNativeBuffers == 1 &&
            charged.ownedNonlocalNativeBufferHostBytes == (identity.deviceLocal ? 0 : actual.size),
        "buffer token equals actual VMA size/type/heap/domain/epoch and mapped HOST_VISIBLE");
    const auto bytes = actual.size;
    // An explicit original mask is preserved, even when a private domain control is absent.
    preferences.memoryTypeBits = std::uint32_t{1} << actual.memoryType;
    backing.clear();
    bufferFinish(device, phase, budget);
    phase = bufferPhase(device, budget, bytes - 1, 1ULL << 20);
    const auto attempts = state->nativeBufferBackingAttempts;
    bufferRequire(allocateCoveredBuffer(*state, info, preferences, 1ULL << 20, backing.buffer,
                                        backing.allocation, actual, backing.token) ==
                          NativeBufferAllocationOutcome::PhaseRefused &&
                      state->nativeBufferBackingAttempts == attempts &&
                      device.nativeBufferStorageRefusal().has_value() &&
                      budget.snapshot().ownedNativeBuffers == 0,
                  "one-byte-short native debit refuses before any actual backing attempt");
    bufferFinish(device, phase, budget);
    phase = bufferPhase(device, budget, 1ULL << 20, 1ULL << 20);
    bufferRequire(!device.nativeBufferStorageRefusal(), "new phase clears old root-local refusal");
    bufferRequire(allocateCoveredBuffer(*state, info, preferences, bytes - 1, backing.buffer,
                                        backing.allocation, actual, backing.token) ==
                          NativeBufferAllocationOutcome::TransferBudgetRefused &&
                      state->nativeBufferBackingAttempts == attempts,
                  "one-byte-short transfer allowance refuses before backing too");
    bufferFinish(device, phase, budget);
    for (auto fault :
         {NativeBufferAllocationFault::Allocation, NativeBufferAllocationFault::Reconcile,
          NativeBufferAllocationFault::Bind, NativeBufferAllocationFault::Map}) {
        phase = bufferPhase(device, budget, 1ULL << 20, 1ULL << 20);
        const auto beforeAttempts = state->nativeBufferBackingAttempts;
        const auto beforeDestroyed = state->nativeBufferBackingsDestroyed;
        state->nativeBufferAllocationFault = fault;
        bufferRequire(allocateCoveredBuffer(*state, info, preferences, 1ULL << 20, backing.buffer,
                                            backing.allocation, actual,
                                            backing.token) == NativeBufferAllocationOutcome::Failed,
                      "controlled allocation/reconcile/bind/map rejection is an honest failure");
        const auto backed = fault == NativeBufferAllocationFault::Allocation ? 0U : 1U;
        bufferRequire(
            state->nativeBufferBackingAttempts == beforeAttempts + backed &&
                state->nativeBufferBackingsDestroyed == beforeDestroyed + backed &&
                budget.snapshot().ownedNativeBufferBytes == 0 &&
                budget.snapshot().ownedNonlocalNativeBufferHostBytes == 0 &&
                !device.nativeBufferStorageRefusal(),
            "injected branch versus actual backing failures destroy before debit foldback");
        bufferFinish(device, phase, budget);
    }
    // Determine actual compatible nonlocal HOST_VISIBLE types on the same real buffer description.
    VkBuffer probe = VK_NULL_HANDLE;
    const auto* dispatch = state->device.getDispatcher();
    const VkDevice rawDevice = static_cast<VkDevice>(*state->device);
    bufferRequire(dispatch->vkCreateBuffer(rawDevice, &info, nullptr, &probe) == VK_SUCCESS,
                  "actual nonlocal compatibility probe exists");
    VkMemoryRequirements requirements{};
    dispatch->vkGetBufferMemoryRequirements(rawDevice, probe, &requirements);
    dispatch->vkDestroyBuffer(rawDevice, probe, nullptr);
    std::uint32_t nonlocal = 0;
    for (std::uint32_t i = 0; i < memory->memoryTypeCount; ++i) {
        const auto type = memory->memoryTypes[i];
        if ((requirements.memoryTypeBits & (std::uint32_t{1} << i)) != 0 &&
            (type.propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0 &&
            (memory->memoryHeaps[type.heapIndex].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0)
            nonlocal |= std::uint32_t{1} << i;
    }
    if (nonlocal == 0) {
        std::cout << "NotRun: nonlocal buffer domain control has no compatible HOST_VISIBLE heap\n";
    } else {
        preferences.memoryTypeBits = nonlocal;
        phase = bufferPhase(device, budget, 1ULL << 20, requirements.size - 1);
        const auto before = state->nativeBufferBackingAttempts;
        bufferRequire(
            allocateCoveredBuffer(*state, info, preferences, 1ULL << 20, backing.buffer,
                                  backing.allocation, actual,
                                  backing.token) == NativeBufferAllocationOutcome::PhaseRefused &&
                state->nativeBufferBackingAttempts == before &&
                budget.snapshot().ownedNativeBuffers == 0,
            "one-byte-short planned nonlocal host debit changes neither domain before backing");
        bufferFinish(device, phase, budget);
        phase = bufferPhase(device, budget, 1ULL << 20, 1ULL << 20);
        const auto beforeClaims = budget.snapshot();
        budget.setHostCapacity(0, false, std::chrono::steady_clock::time_point::min());
        budget.setDeviceCapacity(0, false);
        bufferRequire(
            allocateCoveredBuffer(*state, info, preferences, 1ULL << 20, backing.buffer,
                                  backing.allocation, actual,
                                  backing.token) == NativeBufferAllocationOutcome::Allocated,
            "already-admitted paired staging conversion is grandfathered under stale shrink");
        const auto c = budget.snapshot();
        bufferRequire(
            !backing.token.identity().deviceLocal &&
                c.ownedNonlocalNativeBufferHostBytes == actual.size &&
                c.totalHostClaimBytes == beforeClaims.totalHostClaimBytes &&
                c.totalDeviceClaimBytes == beforeClaims.totalDeviceClaimBytes,
            "nonlocal host staging transfers once without an additive or duplicate claim");
        backing.clear();
        bufferFinish(device, phase, budget);
        budget.setHostCapacity(1ULL << 30, true);
        budget.setDeviceCapacity(1ULL << 30, true);
        std::cout << "BUFFER_NONLOCAL actualBytes=" << actual.size << " hostTransferred=1\n";
    }
    std::cout << "PASS: actual staging identity/mapping, paired denial zero attempts and backing "
                 "faults\n";
}
void rootedBufferTransfers(Expectations& checks, GpuDevice& device,
                           const GpuDeviceCreationOptions& options) {
    BufferTestRetirement retirement;
    bufferRequire(retirement.owner->prepare(), "packed host retirement starts off the UI thread");
    GpuProcessingPhaseBudget budget(32, retirement.owner);
    budget.setHostCapacity(1ULL << 30, true);
    budget.setDeviceCapacity(1ULL << 30, true);
    auto phase = bufferPhase(device, budget, 64ULL << 20, 1ULL << 20);
    auto source = finalImage(checks, device);
    bufferRequire(bool(source), "rooted final GPU source exists");
    roundTrip(checks, device, source, options, phase);
    bufferRequire(budget.snapshot().ownedNativeBuffers == 0,
                  "packed result retains host pixels but no transient staging buffer");
    auto state = GpuRendererAccess::state(device);
    using display_transfer_detail::TransferFault;
    for (auto fault :
         {TransferFault::CommandSetup, TransferFault::Submit, TransferFault::Invalidate}) {
        GpuDisplayTransfer rejected;
        const auto attempts = state->nativeBufferBackingAttempts;
        const auto destroyedBefore = state->nativeBufferBackingsDestroyed;
        display_transfer_detail::setTransferFaultForTest(fault);
        const bool admitted = rejected.begin(device, source, 1ULL << 20, phase);
        const bool afterSubmit = fault == TransferFault::Invalidate;
        bufferRequire(
            admitted == afterSubmit &&
                (!afterSubmit || complete(rejected) == GpuDisplayTransferState::Failure) &&
                rejected.diagnostic().code == GpuDisplayTransferCode::TransferFailed &&
                !rejected.hasUnretiredSubmission() && rejected.reset() &&
                state->nativeBufferBackingAttempts == attempts + 1 &&
                state->nativeBufferBackingsDestroyed == destroyedBefore + 1 &&
                budget.snapshot().ownedNativeBufferBytes == 0 &&
                budget.snapshot().ownedPackedHostBytes == 0,
            "controlled command/submit/invalidate failure frees actual backing before foldback");
        display_transfer_detail::setTransferFaultForTest(TransferFault::None);
    }
    GpuDisplayTransfer cancelled;
    bufferRequire(cancelled.begin(device, source, 1ULL << 20, phase),
                  "rooted Pending cancellation starts after actual submission");
    cancelled.cancel();
    bufferRequire(
        complete(cancelled) == GpuDisplayTransferState::Failure &&
            cancelled.diagnostic().code == GpuDisplayTransferCode::Cancelled &&
            !cancelled.hasUnretiredSubmission() && cancelled.reset() &&
            budget.snapshot().ownedNativeBufferBytes == 0 &&
            budget.snapshot().ownedPackedHostBytes == 0,
        "actual fence-proven Pending cancellation frees staging/pixels and publishes nothing");
    auto transfer = std::make_unique<GpuDisplayTransfer>();
    bufferRequire(transfer->begin(device, source, 1ULL << 20, phase),
                  "rooted real submitted foreign final-release control admitted");
    const auto retention = transfer->retention();
    const auto pending = budget.snapshot();
    const auto destroyed = state->nativeBufferBackingsDestroyed;
    device.reclaimReleasedImages();
    bufferRequire(device.displayTransferPending() &&
                      state->nativeBufferBackingsDestroyed == destroyed &&
                      budget.snapshot().ownedNativeBufferBytes == pending.ownedNativeBufferBytes,
                  "native owner tick cannot poll or consume a live Reserved Impl");
    std::thread foreign([&] {
        transfer.reset();
        device.reclaimReleasedImages(); // wrong owner: no native call
    });
    foreign.join();
    const auto handedOff = budget.snapshot();
    bufferRequire(
        device.displayTransferPending() && pending.ownedNativeBuffers == 1 &&
            handedOff.ownedNativeBufferBytes == pending.ownedNativeBufferBytes &&
            handedOff.ownedNonlocalNativeBufferHostBytes ==
                pending.ownedNonlocalNativeBufferHostBytes &&
            handedOff.ownedPackedHostBytes == pending.ownedPackedHostBytes &&
            state->nativeBufferBackingsDestroyed == destroyed &&
            retention->snapshot().hostBytes > 0,
        "foreign Pending final drop hands off buffer and pixels without free or false gauge clear");
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (device.displayTransferPending() && std::chrono::steady_clock::now() < until) {
        device.reclaimReleasedImages();
        std::this_thread::yield();
    }
    bufferRequire(
        !device.displayTransferPending() && state->nativeBufferBackingsDestroyed == destroyed + 1 &&
            budget.snapshot().ownedNativeBufferBytes == 0 &&
            budget.snapshot().ownedNonlocalNativeBufferHostBytes == 0 &&
            budget.snapshot().ownedPackedHostBytes == 0 && retention->snapshot().reservedBytes == 0,
        "real native owner fence drain frees backing/token/pixels before pending becomes zero");
    GpuDisplayTransfer recovered;
    bufferRequire(recovered.begin(device, source, 1ULL << 20, phase) &&
                      complete(recovered) == GpuDisplayTransferState::Ready &&
                      recovered.take().has_value() && !device.displayTransferPending(),
                  "recoverable foreign handoff allows same-generation reuse without clearing any "
                  "fuse or latch");
    GpuDisplayTransfer unknown;
    bufferRequire(unknown.begin(device, source, 1ULL << 20, phase),
                  "real rooted submission before observed timeout");
    display_transfer_detail::setTransferFaultForTest(
        display_transfer_detail::TransferFault::ForceFenceTimeout);
    bufferRequire(
        unknown.poll() == GpuDisplayTransferState::Failure && unknown.hasUnretiredSubmission() &&
            budget.snapshot().ownedNativeBufferBytes > 0 &&
            budget.snapshot().ownedNativeBuffers == 1 && device.displayTransferPending(),
        "fault-overridden observation after actual submit retains staging and exact phase");
    display_transfer_detail::setTransferFaultForTest(display_transfer_detail::TransferFault::None);
    while (unknown.hasUnretiredSubmission() &&
           std::chrono::steady_clock::now() < until + std::chrono::seconds(5)) {
        device.reclaimReleasedImages();
        std::this_thread::yield();
    }
    bufferRequire(!unknown.hasUnretiredSubmission() && unknown.reset() &&
                      budget.snapshot().ownedNativeBufferBytes == 0 &&
                      !device.displayTransferPending(),
                  "removing only observed-status override permits actual fence owner retirement");
    source.reset();
    device.reclaimReleasedImages();
    state.reset();
    bufferFinish(device, phase, budget);
    std::cout << "PASS: rooted staging pixels/foreign Reserved handoff/submitted observed-timeout "
                 "owner retirement\n";
}
