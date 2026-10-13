#include "gpu_ocio_program_private.hpp"
#include "ocio_gpu_program_fault.hpp"

#include <bloom/render/gpu_processing_phase_budget.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace {
using namespace bloom::render;
using namespace bloom::render::ocio_program_detail;
constexpr std::uint64_t kCapacity = 1ULL << 30;
constexpr std::uint64_t kEnvelope = 1ULL << 20;
struct SampledGuard final {
    vulkan_detail::DeviceAllocatorState& state;
    SampledResource& resource;
    ~SampledGuard() { destroySampledResource(state, resource); }
};
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
OcioGpuTextureDesc texture(OcioGpuTextureDimensions dimensions) {
    OcioGpuTextureDesc desc;
    desc.dimensions = dimensions;
    desc.channel = OcioGpuTextureChannel::Rgb;
    desc.width = 8;
    desc.height = dimensions == OcioGpuTextureDimensions::TwoD ? 4 : 1;
    desc.edgeLength = 4;
    desc.interpolation = OcioGpuInterpolation::Linear;
    const auto texels =
        dimensions == OcioGpuTextureDimensions::ThreeD ? 64U : desc.width * desc.height;
    desc.samples.resize(static_cast<std::size_t>(texels) * 3);
    for (std::size_t i = 0; i < desc.samples.size(); ++i)
        desc.samples[i] = static_cast<float>(i % 13) / 12.0F;
    return desc;
}
std::shared_ptr<GpuProcessingPhaseLease> retain(GpuDevice& device, GpuProcessingPhaseBudget& budget,
                                                std::uint64_t bytes) {
    auto granted = budget.reserve(
        {.requestedExecutorBytes = bytes, .minimumExecutorBytes = bytes, .sourceFree = true});
    require(granted.lease && device.retainProcessingPhase(granted.lease),
            "the exact declared LUT phase is retained");
    return std::move(granted.lease);
}
void finish(GpuDevice& device, std::shared_ptr<GpuProcessingPhaseLease>& phase,
            const GpuProcessingPhaseBudget& budget) {
    device.retireProcessingPhase(phase->id(), true);
    phase.reset();
    const auto counters = budget.snapshot();
    require(counters.ownedNativeImages == 0 && counters.ownedNativeImageBytes == 0 &&
                counters.ownedNativeBuffers == 0 && counters.ownedNativeBufferBytes == 0 &&
                counters.ownedNonlocalNativeBufferHostBytes == 0 &&
                counters.ownedProcessingHostBytes == 0 &&
                counters.ownedProcessingHostMetadataBytes == 0 &&
                counters.ownedProcessingHostAllocations == 0 &&
                counters.ownedNonlocalNativeHostBytes == 0 && counters.totalDeviceClaimBytes == 0 &&
                counters.totalHostClaimBytes == 0 && counters.admittedPhases == 0 &&
                counters.unprovenPhases == 0,
            "actual LUT backing retirement conserves all claims");
}
void stagingOwnership(GpuDevice& device) {
    auto state = GpuRendererAccess::state(device);
    GpuProcessingPhaseBudget budget;
    budget.setHostCapacity(kCapacity, true);
    budget.setDeviceCapacity(kCapacity, true);
    auto phase = retain(device, budget, kEnvelope);
    ocioFaultLutRgbaFormat().store(true);
    SampledResource resource;
    const SampledGuard resourceGuard{*state, resource};
    std::uint64_t stagingBytes = 0;
    std::uint64_t scratchBytes = 0;
    std::uint64_t scratchMetadata = 0;
    require(createSampledResource(
                state, texture(OcioGpuTextureDimensions::ThreeD), kEnvelope,
                [&] {
                    const auto sample = budget.snapshot();
                    stagingBytes = std::max(stagingBytes, sample.ownedNativeBufferBytes);
                    scratchBytes = std::max(scratchBytes, sample.ownedProcessingHostBytes);
                    scratchMetadata =
                        std::max(scratchMetadata, sample.ownedProcessingHostMetadataBytes);
                    require(sample.ownedNativeBuffers <= 1,
                            "LUT upload owns exactly its single live staging backing");
                    return false;
                },
                resource) == UploadOutcome::Retired &&
                stagingBytes > 0 && budget.snapshot().ownedNativeBuffers == 0,
            "real LUT transfer carries staging ownership until its proven fence");
    require(scratchBytes == 64ULL * 4 * sizeof(float) && scratchMetadata > 0 &&
                budget.snapshot().ownedProcessingHostBytes == 0 &&
                budget.snapshot().ownedProcessingHostMetadataBytes == 0,
            "RGBA padding owns exact requested CPU payload/header until actual scratch free");
    const bool localImage = resource.storage.identity().deviceLocal;
    const auto imageBytes = resource.storage.bytes();
    destroySampledResource(*state, resource);
    finish(device, phase, budget);
    phase = retain(device, budget, imageBytes + stagingBytes - 1);
    const auto attempts = state->nativeBufferBackingAttempts;
    require(createSampledResource(state, texture(OcioGpuTextureDimensions::ThreeD), kEnvelope, {},
                                  resource) == UploadOutcome::OverBudget &&
                state->nativeBufferBackingAttempts == attempts &&
                budget.snapshot().ownedNativeImages == 0 &&
                budget.snapshot().ownedNativeBuffers == 0,
            "one-byte-short LUT staging refuses before backing and destroys its image prefix");
    const auto refusal = device.nativeBufferStorageRefusal();
    if (!refusal)
        throw std::runtime_error("LUT staging denial requires typed phase provenance");
    require(refusal->domain == GpuProcessingPhaseDomain::Device &&
                refusal->status == GpuProcessingPhaseAdmission::BudgetRefused &&
                refusal->phaseId == phase->id(),
            "LUT staging denial retains exact typed phase provenance");
    const auto revision = refusal->revisionAfterOwnRelease;
    state->nativeImageAllocationFault = vulkan_detail::NativeImageAllocationFault::View;
    require(createSampledResource(state, texture(OcioGpuTextureDimensions::ThreeD), kEnvelope, {},
                                  resource) == UploadOutcome::Failed &&
                !device.nativeBufferStorageRefusal(),
            "a new pre-staging driver failure cannot reuse a previous typed buffer refusal");
    finish(device, phase, budget);
    require(budget.snapshot().admissionRevision == revision,
            "destroyed staging/image prefix contributes only the original phase release");
    using Fault = vulkan_detail::NativeBufferAllocationFault;
    for (auto fault : {Fault::Allocation, Fault::Reconcile, Fault::Bind, Fault::Map}) {
        phase = retain(device, budget, kEnvelope);
        state->nativeBufferAllocationFault = fault;
        const auto destroyed = state->nativeBufferBackingsDestroyed;
        require(createSampledResource(state, texture(OcioGpuTextureDimensions::ThreeD), kEnvelope,
                                      {}, resource) == UploadOutcome::Failed &&
                    state->nativeBufferAllocationFault == Fault::None &&
                    state->nativeBufferBackingsDestroyed ==
                        destroyed + (fault == Fault::Allocation ? 0 : 1) &&
                    budget.snapshot().ownedNativeImages == 0 &&
                    budget.snapshot().ownedNativeBuffers == 0,
                "each actual staging allocation/reconcile/bind/map fault frees before credit");
        finish(device, phase, budget);
    }
    if (localImage) {
        budget.setHostCapacity(scratchBytes + scratchMetadata - 1, true);
        phase = retain(device, budget, kEnvelope);
        const auto beforeScratch = state->nativeBufferBackingAttempts;
        require(createSampledResource(state, texture(OcioGpuTextureDimensions::ThreeD), kEnvelope,
                                      {}, resource) == UploadOutcome::OverBudget &&
                    state->nativeBufferBackingAttempts == beforeScratch &&
                    budget.snapshot().ownedNativeImages == 0 &&
                    budget.snapshot().ownedProcessingHostAllocations == 0,
                "padded host scratch refuses before CPU/staging allocation and frees image prefix");
        const auto hostRefusal = device.nativeBufferStorageRefusal();
        require(hostRefusal && hostRefusal->domain == GpuProcessingPhaseDomain::Host &&
                    hostRefusal->status == GpuProcessingPhaseAdmission::BudgetRefused,
                "scratch refusal preserves exact host capacity provenance");
        finish(device, phase, budget);
        budget.setHostCapacity(kCapacity, true);
        phase = retain(device, budget, kEnvelope);
        budget.setHostCapacity(kCapacity, true,
                               std::chrono::steady_clock::now() - std::chrono::seconds(1));
        require(createSampledResource(state, texture(OcioGpuTextureDimensions::ThreeD), kEnvelope,
                                      {}, resource) == UploadOutcome::OverBudget,
                "stale additive CPU scratch delivery fails closed");
        const auto stale = device.nativeBufferStorageRefusal();
        require(stale && stale->status == GpuProcessingPhaseAdmission::AwaitingCapacity &&
                    stale->domain == GpuProcessingPhaseDomain::Host && !budget.snapshot().hostReady,
                "scratch freshness expiry carries typed AwaitingCapacity metadata");
        finish(device, phase, budget);
    } else {
        std::cout << "NOTRUN: scratch-only host refusal requires local LUT image backing\n";
    }
    budget.setHostCapacity(kCapacity, true);
    ocioFaultLutRgbaFormat().store(false);
    std::cout << "PASS: exact LUT staging charge, pre-backing denial and native faults\n";
}

void stagingQuarantine(const GpuDeviceCreationOptions& options) {
    GpuProcessingPhaseBudget budget;
    budget.setHostCapacity(kCapacity, true);
    budget.setDeviceCapacity(kCapacity, true);
    auto made = GpuDevice::create(options);
    require(bool(made), "the isolated LUT quarantine owner is ready");
    auto state = GpuRendererAccess::state(*made.device);
    auto phase = retain(*made.device, budget, kEnvelope);
    SampledResource resource;
    ocioFaultCancelAfterSubmit().store(true);
    const auto outcome = createSampledResource(state, texture(OcioGpuTextureDimensions::ThreeD),
                                               kEnvelope, {}, resource);
    ocioFaultCancelAfterSubmit().store(false);
    require(outcome == UploadOutcome::Cancelled && resource.image == VK_NULL_HANDLE &&
                uploadQuarantineOccupied() && budget.snapshot().ownedNativeImages == 1 &&
                budget.snapshot().ownedNativeBuffers == 1 &&
                budget.snapshot().ownedProcessingHostAllocations == 0 &&
                budget.snapshot().unprovenPhases == 1,
            "cancel after genuine submit retains image, staging and exact unproven phase");
    made.device->retireProcessingPhase(phase->id(), true);
    require(made.device->processingPhaseOccupied(),
            "a caller retirement claim cannot clear the unknown native generation");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (uploadQuarantineOccupied() && std::chrono::steady_clock::now() < deadline) {
        retireUploadQuarantine();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(!uploadQuarantineOccupied() && budget.snapshot().ownedNativeImages == 0 &&
                budget.snapshot().ownedNativeBuffers == 0 && budget.snapshot().unprovenPhases == 1,
            "real fence proof frees staging/image but leaves generation quarantine sticky");
    phase.reset();
    state.reset();
    made.device.reset();
    require(budget.snapshot().totalDeviceClaimBytes == 0 &&
                budget.snapshot().totalHostClaimBytes == 0 && budget.snapshot().admittedPhases == 0,
            "exact native owner teardown releases the retained quarantine phase");
    std::cout << "PASS: LUT staging quarantine owns charges through real native retirement\n";
}
void dimensionalAndFaults(GpuDevice& device) {
    auto state = GpuRendererAccess::state(device);
    require(bool(state), "the real LUT allocator exists");
    GpuProcessingPhaseBudget budget;
    budget.setHostCapacity(kCapacity, true);
    budget.setDeviceCapacity(kCapacity, true);
    for (auto dimensions : {OcioGpuTextureDimensions::OneD, OcioGpuTextureDimensions::TwoD,
                            OcioGpuTextureDimensions::ThreeD}) {
        for (bool forceRgba : {false, true}) {
            auto phase = retain(device, budget, kEnvelope);
            ocioFaultLutRgbaFormat().store(forceRgba);
            SampledResource resource;
            const SampledGuard resourceGuard{*state, resource};
            require(createSampledResource(state, texture(dimensions), kEnvelope, {}, resource) ==
                        UploadOutcome::Retired,
                    "actual dimensional LUT upload retires");
            VmaAllocationInfo actual{};
            vmaGetAllocationInfo(state->allocator, resource.allocation, &actual);
            const VkPhysicalDeviceMemoryProperties* memory = nullptr;
            vmaGetMemoryProperties(state->allocator, &memory);
            const auto heap = memory->memoryTypes[actual.memoryType].heapIndex;
            const auto identity = resource.storage.identity();
            require(resource.storage.bytes() == actual.size &&
                        identity.memoryType == actual.memoryType && identity.heap == heap &&
                        identity.allocatorGeneration == device.ownershipEpoch() &&
                        identity.deviceLocal == ((memory->memoryHeaps[heap].flags &
                                                  VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0),
                    "LUT token matches actual VMA size/type/heap/domain/generation");
            require(!forceRgba || resource.format == VK_FORMAT_R32G32B32A32_SFLOAT,
                    "the controlled native RGBA format fallback is actually used");
            require(budget.snapshot().ownedNativeImages == 1 &&
                        budget.snapshot().ownedNativeImageBytes == actual.size,
                    "one sampled backing owns one exact image charge");
            const auto bytes = actual.size;
            destroySampledResource(*state, resource);
            finish(device, phase, budget);
            phase = retain(device, budget, bytes - 1);
            const auto attempts = state->nativeImageBackingAttempts;
            SampledResource denied;
            const SampledGuard deniedGuard{*state, denied};
            require(createSampledResource(state, texture(dimensions), kEnvelope, {}, denied) ==
                            UploadOutcome::OverBudget &&
                        denied.image == VK_NULL_HANDLE &&
                        state->nativeImageBackingAttempts == attempts &&
                        device.nativeImageStorageRefusal().has_value(),
                    "one-byte-short actual LUT quota refuses before any backing attempt");
            finish(device, phase, budget);
        }
    }
    ocioFaultLutRgbaFormat().store(false);
    using Fault = vulkan_detail::NativeImageAllocationFault;
    for (auto fault : {Fault::Allocation, Fault::Reconcile, Fault::Bind, Fault::View}) {
        auto phase = retain(device, budget, kEnvelope);
        const auto attempts = state->nativeImageBackingAttempts;
        const auto destroyed = state->nativeImageBackingsDestroyed;
        state->nativeImageAllocationFault = fault;
        SampledResource failed;
        const SampledGuard failedGuard{*state, failed};
        require(createSampledResource(state, texture(OcioGpuTextureDimensions::ThreeD), kEnvelope,
                                      {}, failed) == UploadOutcome::Failed,
                "the scoped LUT allocation/reconcile/bind/view fault refuses honestly");
        const bool backed = fault != Fault::Allocation;
        require(state->nativeImageBackingAttempts == attempts + (backed ? 1 : 0) &&
                    state->nativeImageBackingsDestroyed == destroyed + (backed ? 1 : 0) &&
                    budget.snapshot().ownedNativeImageBytes == 0,
                "post-backing LUT faults destroy backing before folding the exact debit");
        finish(device, phase, budget);
    }
    std::cout
        << "PASS: actual LUT 1D/2D/3D identity, RGBA fallback, pre-backing denial and faults\n";
}
void nonlocalFreshness(GpuDevice& device) {
    auto state = GpuRendererAccess::state(device);
    const VkPhysicalDeviceMemoryProperties* memory = nullptr;
    vmaGetMemoryProperties(state->allocator, &memory);
    std::uint32_t mask = 0;
    for (std::uint32_t type = 0; type < memory->memoryTypeCount; ++type)
        if ((memory->memoryHeaps[memory->memoryTypes[type].heapIndex].flags &
             VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0)
            mask |= std::uint32_t{1} << type;
    if (mask == 0) {
        std::cout << "NOTRUN: actual device exposes no nonlocal LUT candidate domain\n";
        return;
    }
    // Prove actual compatibility before permitting a domain-only NOTRUN. A VMA/view/driver
    // failure after this point is a required-fixture failure, never an unavailable-domain pass.
    VkImageCreateInfo probeInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_3D,
        .format = VK_FORMAT_R32G32B32A32_SFLOAT,
        .extent = {4, 4, 4},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    const auto* dispatch = state->device.getDispatcher();
    const VkDevice nativeDevice = static_cast<VkDevice>(*state->device);
    VkImage probe = VK_NULL_HANDLE;
    require(dispatch->vkCreateImage(nativeDevice, &probeInfo, nullptr, &probe) == VK_SUCCESS,
            "the actual nonlocal compatibility image is created without backing");
    VkMemoryRequirements requirements{};
    dispatch->vkGetImageMemoryRequirements(nativeDevice, probe, &requirements);
    dispatch->vkDestroyImage(nativeDevice, probe, nullptr);
    mask &= requirements.memoryTypeBits;
    if (mask == 0) {
        std::cout << "NOTRUN: actual LUT requirements have no compatible nonlocal candidate\n";
        return;
    }
    ocioFaultLutRgbaFormat().store(true);
    state->nativeImageMemoryTypeMaskForTests = mask;
    GpuProcessingPhaseBudget budget;
    budget.setHostCapacity(kCapacity, true);
    budget.setDeviceCapacity(kCapacity, true);
    auto phase = retain(device, budget, kEnvelope);
    SampledResource resource;
    const SampledGuard resourceGuard{*state, resource};
    const auto outcome = createSampledResource(state, texture(OcioGpuTextureDimensions::ThreeD),
                                               kEnvelope, {}, resource);
    require(outcome == UploadOutcome::Retired,
            "an actual compatible nonlocal LUT must allocate/upload successfully");
    require(!resource.storage.identity().deviceLocal &&
                budget.snapshot().ownedNonlocalNativeHostBytes == resource.storage.bytes(),
            "actual nonlocal LUT backing has the overlapping additive host claim");
    destroySampledResource(*state, resource);
    budget.setHostCapacity(kCapacity, true,
                           std::chrono::steady_clock::now() - std::chrono::seconds(1));
    const auto attempts = state->nativeImageBackingAttempts;
    SampledResource expired;
    const SampledGuard expiredGuard{*state, expired};
    require(createSampledResource(state, texture(OcioGpuTextureDimensions::ThreeD), kEnvelope, {},
                                  expired) == UploadOutcome::OverBudget &&
                state->nativeImageBackingAttempts == attempts,
            "expired new nonlocal host claim refuses before actual backing");
    const auto refused = device.nativeImageStorageRefusal();
    require(refused && refused->status == GpuProcessingPhaseAdmission::AwaitingCapacity &&
                !budget.snapshot().hostReady,
            "actual LUT additive-host expiry emits typed awaiting and closes readiness");
    finish(device, phase, budget);
    budget.setHostCapacity(kCapacity, true);
    phase = retain(device, budget, kEnvelope);
    require(createSampledResource(state, texture(OcioGpuTextureDimensions::ThreeD), kEnvelope, {},
                                  resource) == UploadOutcome::Retired,
            "fresh same-byte host delivery restores actual nonlocal LUT admission");
    destroySampledResource(*state, resource);
    finish(device, phase, budget);
    state->nativeImageMemoryTypeMaskForTests = 0;
    ocioFaultLutRgbaFormat().store(false);
    std::cout << "PASS: actual nonlocal LUT overlapping host claim and freshness recovery\n";
}
} // namespace
int main(int argc, char** argv) {
    GpuDeviceCreationOptions options;
    bool required = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--require-device")
            required = true;
        if (std::string_view(argv[i]) == "--loader" && i + 1 < argc)
            options.loader_path = argv[++i];
    }
    auto made = GpuDevice::create(options);
    if (!made) {
        std::cout << (required ? "FAIL: " : "SKIP: ") << made.diagnostic.message << '\n';
        return required ? 1 : 77;
    }
    try {
        dimensionalAndFaults(*made.device);
        stagingOwnership(*made.device);
        nonlocalFreshness(*made.device);
        stagingQuarantine(options);
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
