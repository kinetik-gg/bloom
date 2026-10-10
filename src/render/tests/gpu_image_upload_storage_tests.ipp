// Included only by the upload owner. Controls are labelled separately from driver failures.
using bloom::render::GpuProcessingPhaseBudget;
using bloom::render::GpuProcessingPhaseDomain;
using bloom::render::GpuProcessingPhaseLease;
using bloom::render::UploadStagingBuffer;
namespace upload_native = bloom::render::vulkan_detail;
void uploadRequire(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
std::shared_ptr<GpuProcessingPhaseLease> uploadPhase(GpuDevice& device,
                                                     GpuProcessingPhaseBudget& budget,
                                                     std::uint64_t uploadBytes,
                                                     std::uint64_t nativeBytes = 8ULL << 20) {
    auto grant = budget.reserve({.fixedHostBytes = 4096,
                                 .requestedSourceBytes = uploadBytes,
                                 .minimumSourceBytes = uploadBytes,
                                 .requestedExecutorBytes = nativeBytes,
                                 .minimumExecutorBytes = nativeBytes,
                                 .capacityShareDenominator = 1});
    uploadRequire(grant.lease && device.retainProcessingPhase(grant.lease),
                  "an unchanged F+3L upload grant is admitted and retained");
    return std::move(grant.lease);
}
void uploadFinish(GpuDevice& device, std::shared_ptr<GpuProcessingPhaseLease>& phase,
                  const GpuProcessingPhaseBudget& budget) {
    device.retireProcessingPhase(phase->id(), true);
    phase.reset();
    const auto c = budget.snapshot();
    uploadRequire(c.ownedNativeBufferBytes == 0 && c.ownedNativeBuffers == 0 &&
                      c.ownedNonlocalNativeBufferHostBytes == 0 && c.ownedNativeImages == 0 &&
                      c.totalHostClaimBytes == 0 && c.totalDeviceClaimBytes == 0 &&
                      c.admittedPhases == 0 && c.unprovenPhases == 0,
                  "native backing destruction precedes whole upload grant release");
}
void testUploadStorage(GpuDevice& device) {
    using upload_native::NativeBufferAllocationFault;
    using upload_native::NativeBufferAllocationOutcome;
    auto state = bloom::render::GpuRendererAccess::state(device);
    uploadRequire(bool(state), "native upload allocator exists");
    GpuProcessingPhaseBudget budget;
    budget.setHostCapacity(1ULL << 30, true);
    budget.setDeviceCapacity(1ULL << 30, true);
    auto phase = uploadPhase(device, budget, 1ULL << 20);
    UploadStagingBuffer backing;
    uploadRequire(bloom::render::createUploadStagingBuffer(*state, 240, 1ULL << 20, backing) ==
                      NativeBufferAllocationOutcome::Allocated,
                  "actual sequential-write mapped staging backing exists");
    VmaAllocationInfo actual{};
    vmaGetAllocationInfo(state->allocator, backing.allocation, &actual);
    const VkPhysicalDeviceMemoryProperties* memory = nullptr;
    vmaGetMemoryProperties(state->allocator, &memory);
    const auto identity = backing.token.identity();
    uploadRequire(identity.memoryType == actual.memoryType &&
                      identity.heap == memory->memoryTypes[actual.memoryType].heapIndex &&
                      identity.allocatorGeneration == device.ownershipEpoch() &&
                      identity.deviceLocal == ((memory->memoryHeaps[identity.heap].flags &
                                                VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) &&
                      (memory->memoryTypes[actual.memoryType].propertyFlags &
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0 &&
                      actual.pMappedData && actual.size == backing.token.bytes(),
                  "real VMA type/heap/domain/epoch/size and mapped visibility match token");
    std::memset(actual.pMappedData, 0x5a, 240);
    uploadRequire(vmaFlushAllocation(state->allocator, backing.allocation, 0, VK_WHOLE_SIZE) ==
                      VK_SUCCESS,
                  "original sequential-write flush executes");
    const auto bytes = backing.token.bytes();
    const auto beforeFree = state->nativeBufferBackingsDestroyed;
    backing.release();
    uploadRequire(state->nativeBufferBackingsDestroyed == beforeFree + 1 &&
                      budget.snapshot().ownedNativeBufferBytes == 0,
                  "actual backing free precedes quiet U restoration");
    uploadFinish(device, phase, budget);
    for (bool host : {true, false}) {
        phase = uploadPhase(device, budget, host ? bytes - 1 : 1ULL << 20,
                            host ? 8ULL << 20 : bytes - 1);
        const auto attempts = state->nativeBufferBackingAttempts;
        uploadRequire(
            bloom::render::createUploadStagingBuffer(*state, 240, 1ULL << 20, backing) ==
                    NativeBufferAllocationOutcome::PhaseRefused &&
                device.nativeBufferStorageRefusal().has_value() &&
                device.nativeBufferStorageRefusal()->domain ==
                    (host ? GpuProcessingPhaseDomain::Host : GpuProcessingPhaseDomain::Device) &&
                state->nativeBufferBackingAttempts == attempts,
            "actual U/native debit denial makes zero backing attempts");
        if (host) {
            auto producer = GpuImageUpload::create(device);
            uploadRequire(bool(producer), "metadata preflight producer exists");
            auto source = makeSource();
            uploadRequire(bool(source), "metadata source exists");
            uploadRequire(producer.upload->begin({source, phase}, 1).code ==
                                  GpuImageUploadDiagnosticCode::OverBudget &&
                              !device.nativeBufferStorageRefusal() &&
                              !device.nativeImageStorageRefusal() &&
                              state->nativeBufferBackingAttempts == attempts,
                          "new logical refusal cannot inherit previous same-phase native metadata");
        }
        uploadFinish(device, phase, budget);
    }
    for (auto fault :
         {NativeBufferAllocationFault::Allocation, NativeBufferAllocationFault::Reconcile,
          NativeBufferAllocationFault::Bind, NativeBufferAllocationFault::Map}) {
        phase = uploadPhase(device, budget, 1ULL << 20);
        const auto attempts = state->nativeBufferBackingAttempts;
        const auto destroyed = state->nativeBufferBackingsDestroyed;
        state->nativeBufferAllocationFault = fault;
        uploadRequire(bloom::render::createUploadStagingBuffer(*state, 240, 1ULL << 20, backing) ==
                          NativeBufferAllocationOutcome::Failed,
                      "scoped allocation/reconcile/bind/map control fails explicitly");
        const auto expected = fault == NativeBufferAllocationFault::Allocation ? 0U : 1U;
        uploadRequire(state->nativeBufferBackingAttempts == attempts + expected &&
                          state->nativeBufferBackingsDestroyed == destroyed + expected &&
                          budget.snapshot().ownedNativeBufferBytes == 0 &&
                          !device.nativeBufferStorageRefusal(),
                      "injected allocation branch or actual backed fault frees before foldback");
        uploadFinish(device, phase, budget);
    }
    // A compatible nonlocal domain must execute; only absent real requirement intersection skips.
    VkBuffer probe = VK_NULL_HANDLE;
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = 240;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    const auto* dispatch = state->device.getDispatcher();
    const VkDevice raw = static_cast<VkDevice>(*state->device);
    uploadRequire(dispatch->vkCreateBuffer(raw, &info, nullptr, &probe) == VK_SUCCESS,
                  "actual compatibility probe creates");
    VkMemoryRequirements requirements{};
    dispatch->vkGetBufferMemoryRequirements(raw, probe, &requirements);
    dispatch->vkDestroyBuffer(raw, probe, nullptr);
    std::uint32_t mask = 0;
    for (std::uint32_t i = 0; i < memory->memoryTypeCount; ++i) {
        const auto type = memory->memoryTypes[i];
        if ((requirements.memoryTypeBits & (std::uint32_t{1} << i)) != 0 &&
            (type.propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0 &&
            (memory->memoryHeaps[type.heapIndex].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0)
            mask |= std::uint32_t{1} << i;
    }
    if (!mask) {
        std::cout << "NotRun: actual upload buffer has no compatible nonlocal HOST_VISIBLE heap\n";
    } else {
        phase = uploadPhase(device, budget, requirements.size);
        state->nativeBufferMemoryTypeMaskForTests = mask;
        const auto outcome =
            bloom::render::createUploadStagingBuffer(*state, 240, 1ULL << 20, backing);
        state->nativeBufferMemoryTypeMaskForTests = 0;
        uploadRequire(outcome == NativeBufferAllocationOutcome::Allocated &&
                          !backing.token.identity().deviceLocal &&
                          budget.snapshot().ownedNonlocalNativeBufferHostBytes ==
                              backing.token.bytes(),
                      "compatible nonlocal U transfers the same pre-admitted host estimate once");
        std::cout << "UPLOAD_NONLOCAL bytes=" << backing.token.bytes() << '\n';
        backing.release();
        uploadFinish(device, phase, budget);
    }
    std::cout << "UPLOAD_STORAGE actualBytes=" << bytes
              << " identity/map/flush/refusal/faults=PASS\n";
}
void testRootedUploadRetirement(GpuDevice& device) {
    GpuProcessingPhaseBudget budget(1);
    budget.setHostCapacity(1ULL << 30, true);
    budget.setDeviceCapacity(1ULL << 30, true);
    auto phase = uploadPhase(device, budget, 1ULL << 20);
    auto source = makeSource();
    auto producer = GpuImageUpload::create(device);
    uploadRequire(source && producer, "rooted source/producer exist");
    const auto state = bloom::render::GpuRendererAccess::state(device);
    const auto attempts = state->nativeBufferBackingAttempts;
    uploadRequire(producer.upload->begin({source}, 1ULL << 30).code ==
                          GpuImageUploadDiagnosticCode::DeviceUnavailable &&
                      state->nativeBufferBackingAttempts == attempts &&
                      !device.imageUploadPending(),
                  "omitted rooted authority is rejected before any resources");
    GpuProcessingPhaseBudget foreign;
    foreign.setHostCapacity(1ULL << 30, true);
    foreign.setDeviceCapacity(1ULL << 30, true);
    auto wrong = foreign.reserve({.fixedHostBytes = 4096,
                                  .requestedSourceBytes = 1ULL << 20,
                                  .minimumSourceBytes = 1ULL << 20,
                                  .requestedExecutorBytes = 8ULL << 20,
                                  .minimumExecutorBytes = 8ULL << 20});
    uploadRequire(wrong.lease && wrong.lease->id() == phase->id() &&
                      producer.upload->begin({source, wrong.lease}, 1ULL << 30).code ==
                          GpuImageUploadDiagnosticCode::DeviceUnavailable &&
                      state->nativeBufferBackingAttempts == attempts,
                  "equal numeric id from another root is not native authority");
    wrong.lease.reset();
    uploadRequire(runUpload(*producer.upload, {source, phase}), "rooted upload completes");
    const auto readback = producer.upload->readback();
    uploadRequire(
        readback && readback.pixels.size() == source->pixels().size() &&
            std::memcmp(readback.pixels.data(), source->pixels().data(),
                        source->pixels().size_bytes()) == 0 &&
            budget.snapshot().ownedNativeBufferBytes == 0,
        "rooted sequential-write upload preserves every CPU source byte and frees staging");
    producer.upload.reset();
    uploadRequire(!device.imageUploadPending(), "actual producer destruction releases its slot");
    producer = GpuImageUpload::create(device);
    uploadRequire(producer &&
                      producer.upload->begin({source, phase}, 1ULL << 30).code ==
                          GpuImageUploadDiagnosticCode::None &&
                      producer.upload->hasUnretiredSubmission(),
                  "foreign control has a real submission");
    const auto held = budget.snapshot();
    uploadRequire(held.ownedNativeBuffers == 1 && held.ownedNativeBufferBytes > 0,
                  "pending upload owns exact buffer debit");
    device.reclaimReleasedImages();
    uploadRequire(budget.snapshot().ownedNativeBufferBytes == held.ownedNativeBufferBytes &&
                      producer.upload->hasUnretiredSubmission(),
                  "owner tick never polls a live reserved slot");
    std::thread release([owned = std::move(producer.upload)]() mutable { owned.reset(); });
    release.join();
    std::thread wrongOwner([&device] { device.reclaimReleasedImages(); });
    wrongOwner.join();
    uploadRequire(device.imageUploadPending() &&
                      budget.snapshot().ownedNativeBufferBytes == held.ownedNativeBufferBytes,
                  "foreign handoff/wrong-thread tick preserve charge");
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (device.imageUploadPending() && std::chrono::steady_clock::now() < until)
        device.reclaimReleasedImages();
    uploadRequire(!device.imageUploadPending() && budget.snapshot().ownedNativeBufferBytes == 0 &&
                      !phase->isUnproven(),
                  "actual native owner fence drain frees before debit return");
    producer = GpuImageUpload::create(device);
    uploadRequire(
        producer && runUpload(*producer.upload, {source, phase}),
        "recoverable foreign release permits same-generation next upload without latch clearing");
    producer.upload.reset();
    uploadFinish(device, phase, budget);
    std::cout << "UPLOAD_FOREIGN max1/same-generation/parity=PASS\n";
}
void testRootedUploadUnknown(const GpuDeviceCreationOptions& options) {
    GpuProcessingPhaseBudget budget(1);
    budget.setHostCapacity(1ULL << 30, true);
    budget.setDeviceCapacity(1ULL << 30, true);
    auto device = GpuDevice::create(options);
    uploadRequire(bool(device), "separate unknown allocator generation exists");
    auto phase = uploadPhase(*device.device, budget, 1ULL << 20);
    auto producer = GpuImageUpload::create(*device.device);
    const auto source = makeSource();
    uploadRequire(producer && source &&
                      producer.upload->begin({source, phase}, 1ULL << 30).code ==
                          GpuImageUploadDiagnosticCode::None &&
                      producer.upload->hasUnretiredSubmission(),
                  "unknown observation control follows a genuine submitted upload");
    upload_detail::setUploadRetirementFaultForTest(
        upload_detail::UploadRetirementFault::ForceFenceTimeout);
    std::thread release([owned = std::move(producer.upload)]() mutable { owned.reset(); });
    release.join();
    device.device->retireProcessingPhase(phase->id(), false);
    device.device->reclaimReleasedImages();
    const auto held = budget.snapshot();
    upload_detail::setUploadRetirementFaultForTest(upload_detail::UploadRetirementFault::None);
    uploadRequire(device.device->imageUploadPending() && held.ownedNativeBuffers == 1 &&
                      held.ownedNativeBufferBytes > 0 && held.admittedPhases == 1 &&
                      held.unprovenPhases == 1,
                  "observed unknown holds staging, phase and sticky authority");
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (device.device->imageUploadPending() && std::chrono::steady_clock::now() < until)
        device.device->reclaimReleasedImages();
    uploadRequire(!device.device->imageUploadPending() &&
                      budget.snapshot().ownedNativeBufferBytes == 0 &&
                      budget.snapshot().unprovenPhases == 1,
                  "real fence/backing drain does not clear the independently sticky phase");
    phase.reset();
    device.device.reset();
    uploadRequire(budget.snapshot().admittedPhases == 0 &&
                      budget.snapshot().totalHostClaimBytes == 0 &&
                      budget.snapshot().totalDeviceClaimBytes == 0,
                  "actual old-generation destruction releases covered sticky claims");
    std::cout << "UPLOAD_UNKNOWN submitted/observed-timeout/actual-drain/generation=PASS\n";
}
