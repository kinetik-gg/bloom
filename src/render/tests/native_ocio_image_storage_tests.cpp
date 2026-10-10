#include "gpu_ocio_program_private.hpp"
#include "ocio_gpu_program_fault.hpp"

#include <bloom/render/gpu_processing_phase_budget.hpp>

#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string_view>

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
                counters.ownedNonlocalNativeHostBytes == 0 && counters.totalDeviceClaimBytes == 0 &&
                counters.totalHostClaimBytes == 0 && counters.admittedPhases == 0 &&
                counters.unprovenPhases == 0,
            "actual LUT backing retirement conserves all claims");
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
        nonlocalFreshness(*made.device);
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
