#include "gpu_composite_native_support.hpp"
#include "gpu_native_buffer_allocation_private.hpp"
#include "gpu_resident_display_private.hpp"

#include <bloom/render/gpu_solid.hpp>

#include <array>
#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace {
using namespace bloom::render;
constexpr std::uint64_t kBudget = 4ULL * 1024 * 1024;
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
std::shared_ptr<GpuProcessingPhaseLease> phase(GpuDevice& device, GpuProcessingPhaseBudget& budget,
                                               std::uint64_t bytes = kBudget) {
    auto grant =
        budget.reserve({.fixedHostBytes = 1, .deviceWorkingBytes = bytes, .sourceFree = true});
    require(grant.lease && device.retainProcessingPhase(grant.lease),
            "exact native phase retained");
    return std::move(grant.lease);
}
void retire(GpuDevice& device, GpuProcessingPhaseBudget& budget,
            std::shared_ptr<GpuProcessingPhaseLease>& lease) {
    device.retireProcessingPhase(lease->id(), true);
    lease.reset();
    const auto snapshot = budget.snapshot();
    require(snapshot.ownedNativeBuffers == 0 && snapshot.ownedNativeBufferBytes == 0 &&
                snapshot.ownedNonlocalNativeBufferHostBytes == 0 &&
                snapshot.totalDeviceClaimBytes == 0 && snapshot.totalHostClaimBytes == 0 &&
                snapshot.admittedPhases == 0,
            "actual destruction and phase retirement return all claims to zero");
}
void actualIdentity(GpuDevice& device, const ResidentBuffer& buffer) {
    const auto state = GpuRendererAccess::state(device);
    VmaAllocationInfo actual{};
    vmaGetAllocationInfo(state->allocator, buffer.allocation, &actual);
    const VkPhysicalDeviceMemoryProperties* memory = nullptr;
    vmaGetMemoryProperties(state->allocator, &memory);
    const auto heap = memory->memoryTypes[actual.memoryType].heapIndex;
    const NativeBufferStorageIdentity identity{
        device.ownershipEpoch(), actual.memoryType, heap,
        (memory->memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0};
    require(buffer.storageToken.bytes() == actual.size &&
                buffer.storageToken.identity() == identity,
            "actual backing size, memory type, heap and allocator generation match token");
}
void backingAndRefusal(GpuDevice& device) {
    auto state = GpuRendererAccess::state(device);
    GpuProcessingPhaseBudget budget(1);
    budget.setHostCapacity(kBudget, true);
    budget.setDeviceCapacity(kBudget, true);
    auto lease = phase(device, budget);
    ResidentBuffer buffer;
    require(createResidentDisplayBuffer(*state, 4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0, false,
                                        buffer),
            "unmapped processing buffer retains original allocator preferences");
    actualIdentity(device, buffer);
    const auto bytes = buffer.storageToken.bytes();
    require(budget.snapshot().ownedNativeBuffers == 1 &&
                budget.snapshot().ownedNativeBufferBytes == bytes,
            "actual unmapped backing is covered exactly once");
    destroyResidentBuffer(*state, buffer);
    retire(device, budget, lease);
    lease = phase(device, budget, bytes - 1);
    const auto attempts = state->nativeBufferBackingAttempts;
    require(!createResidentDisplayBuffer(*state, 4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0, false,
                                         buffer) &&
                buffer.buffer == VK_NULL_HANDLE && state->nativeBufferBackingAttempts == attempts,
            "one-byte-short native debit refuses before allocator backing");
    const auto refusal = device.nativeBufferStorageRefusal();
    require(refusal && refusal->status == GpuProcessingPhaseAdmission::BudgetRefused &&
                refusal->domain == GpuProcessingPhaseDomain::Device &&
                refusal->phaseId == lease->id(),
            "actual refusal retains exact phase retry provenance");
    retire(device, budget, lease);
    using Fault = vulkan_detail::NativeBufferAllocationFault;
    for (const auto fault : {Fault::Allocation, Fault::Reconcile, Fault::Bind, Fault::Map}) {
        lease = phase(device, budget);
        state->nativeBufferAllocationFault = fault;
        const auto backingAttempts = state->nativeBufferBackingAttempts;
        const auto destroyed = state->nativeBufferBackingsDestroyed;
        require(!createResidentDisplayBuffer(*state, 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                             VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                                 VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                             true, buffer),
                "injected mapped-buffer backing failure returns no resource");
        require(state->nativeBufferAllocationFault == Fault::None &&
                    state->nativeBufferBackingAttempts ==
                        backingAttempts + (fault == Fault::Allocation ? 0 : 1) &&
                    state->nativeBufferBackingsDestroyed ==
                        destroyed + (fault == Fault::Allocation ? 0 : 1) &&
                    !device.nativeBufferStorageRefusal() &&
                    budget.snapshot().deviceReservedBytes == lease->deviceBytes(),
                "fault destroys actual backing before quiet foldback, without capacity retry");
        retire(device, budget, lease);
    }
}
void nonlocalBacking(GpuDevice& device) {
    auto state = GpuRendererAccess::state(device);
    const VkPhysicalDeviceMemoryProperties* memory = nullptr;
    vmaGetMemoryProperties(state->allocator, &memory);
    std::uint32_t mask = 0;
    for (std::uint32_t type = 0; type < memory->memoryTypeCount; ++type) {
        const auto heap = memory->memoryTypes[type].heapIndex;
        if ((memory->memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0 &&
            (memory->memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0)
            mask |= std::uint32_t{1} << type;
    }
    if (mask == 0) {
        std::cout << "NotRun: device has no nonlocal host-visible processing memory type\n";
        return;
    }
    state->nativeBufferMemoryTypeMaskForTests = mask;
    GpuProcessingPhaseBudget budget(1);
    budget.setHostCapacity(kBudget, true);
    budget.setDeviceCapacity(kBudget, true);
    auto lease = phase(device, budget);
    ResidentBuffer buffer;
    constexpr auto flags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    require(createResidentDisplayBuffer(*state, 4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, flags,
                                        true, buffer),
            "actual forced-nonlocal mapped processing backing succeeds");
    actualIdentity(device, buffer);
    const auto bytes = buffer.storageToken.bytes();
    require(!buffer.storageToken.identity().deviceLocal && buffer.info.pMappedData &&
                budget.snapshot().totalHostClaimBytes == 1 + bytes &&
                budget.snapshot().hostReservedBytes == 1,
            "actual nonlocal processing adds host headroom without spending output grant");
    destroyResidentBuffer(*state, buffer);
    retire(device, budget, lease);
    for (const bool stale : {false, true}) {
        budget.setHostCapacity(kBudget, true);
        lease = phase(device, budget);
        budget.setHostCapacity(stale ? kBudget : bytes, !stale);
        const auto attempts = state->nativeBufferBackingAttempts;
        require(!createResidentDisplayBuffer(*state, 4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                             flags, true, buffer) &&
                    state->nativeBufferBackingAttempts == attempts,
                "nonlocal host refusal precedes actual backing");
        const auto refusal = device.nativeBufferStorageRefusal();
        require(refusal && refusal->domain == GpuProcessingPhaseDomain::Host &&
                    refusal->phaseId == lease->id() &&
                    refusal->status == (stale ? GpuProcessingPhaseAdmission::AwaitingCapacity
                                              : GpuProcessingPhaseAdmission::BudgetRefused),
                "stale and one-byte-short host headroom report distinct typed refusals");
        retire(device, budget, lease);
    }
    state->nativeBufferMemoryTypeMaskForTests = 0;
}
void productionCycles(GpuDevice& device) {
    // Create the input outside the rooted attempt. Only this slice's resident display processing
    // buffers and output image consume the attempt; source ownership is covered separately.
    auto solid = GpuSolid::create(device);
    auto display = GpuResidentDisplay::create(device);
    require(solid.hasValue() && display.hasValue(), "actual production pipelines created");
    const auto w = ImageWindow::create(0, 0, 64, 32);
    const auto color = solidPixelFromStraightLinearRec709Scene({0.25, 0.5, 1.0, 1.0});
    require(w.hasValue() && color.hasValue(), "finite test geometry and process color");
    require(solid.solid
                    ->begin({*color.value(), *w.value(), *w.value(),
                             bloom::core::PixelAspectRatio::square()},
                            kBudget)
                    .code == GpuSolidDiagnosticCode::None,
            "real input dispatched");
    const auto wait = [](auto& operation, auto pending) {
        auto status = operation.poll();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (status == pending && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
            status = operation.poll();
        }
        return status;
    };
    require(wait(*solid.solid, GpuSolidPollResult::Pending) == GpuSolidPollResult::Ready,
            "real input fence retires");
    auto input = std::make_shared<const GpuImage>(solid.solid->takeImage());
    const auto reference =
        composite_proof::makeImage(*w.value(), *w.value(), bloom::core::PixelAspectRatio::square(),
                                   std::vector<Rgba32f>(2048, *color.value()));
    if (!reference)
        throw std::runtime_error("independent CPU process reference creation failed");
    const auto view = reference.value().view();
    require(view.hasValue(), "independent CPU process reference readable");
    std::vector<Rgba8> expected(2048);
    for (std::int64_t y = 0; y != 32; ++y)
        require(!mapLinearRec709SceneToSrgbRow(
                    *view.value(), *w.value(), y,
                    std::span<Rgba8>(expected).subspan(static_cast<std::size_t>(y) * 64, 64)),
                "independent CPU display reference mapped");
    const auto state = GpuRendererAccess::state(device);
    VmaTotalStatistics baseline{};
    vmaCalculateStatistics(state->allocator, &baseline);
    GpuProcessingPhaseBudget budget(1);
    budget.setHostCapacity(kBudget, true);
    budget.setDeviceCapacity(kBudget, true);
    ResidentBuffer probe;
    require(createResidentDisplayBuffer(*state, 2048 * sizeof(Rgba32f),
                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                        0, false, probe),
            "actual input-buffer requirements measured before rooted attempt");
    const auto inputBackingBytes = probe.info.size;
    destroyResidentBuffer(*state, probe);
    auto failedPhase = phase(device, budget, inputBackingBytes);
    const auto attempts = state->nativeBufferBackingAttempts;
    require(display.display->begin(input, kBudget).code ==
                    GpuResidentDisplayDiagnosticCode::AllocationFailed &&
                !display.display->hasUnretiredSubmission() &&
                budget.snapshot().ownedNativeBuffers == 1 &&
                state->nativeBufferBackingAttempts == attempts + 1,
            "actual display output-buffer refusal retains the successfully allocated input prefix");
    const auto refusal = device.nativeBufferStorageRefusal();
    if (!refusal)
        throw std::runtime_error("partial production failure is missing capacity provenance");
    require(refusal->phaseId == failedPhase->id() &&
                refusal->domain == GpuProcessingPhaseDomain::Device,
            "partial production failure preserves exact capacity provenance");
    auto externalPhase = failedPhase;
    require(display.display->discardRetiredFailure(device) &&
                budget.snapshot().ownedNativeBuffers == 0 &&
                budget.snapshot().deviceReservedBytes == inputBackingBytes,
            "proven failed producer destroys partial backing before quiet token foldback");
    device.retireProcessingPhase(failedPhase->id(), true);
    failedPhase.reset();
    require(budget.snapshot().admittedPhases == 1 &&
                budget.snapshot().admissionRevision + 1 == refusal->revisionAfterOwnRelease,
            "external phase alias holds exact own R+1 without premature capacity release");
    externalPhase.reset();
    require(budget.snapshot().admittedPhases == 0 &&
                budget.snapshot().admissionRevision == refusal->revisionAfterOwnRelease,
            "final external phase release permits same-pipeline capacity-one recovery");
    for (int iteration = 0; iteration != 10; ++iteration) {
        auto lease = phase(device, budget);
        require(display.display->begin(input, kBudget).code ==
                    GpuResidentDisplayDiagnosticCode::None,
                "real resident display dispatch admitted at capacity one");
        const auto pending = budget.snapshot();
        require(pending.ownedNativeBuffers == 3 && pending.ownedNativeBufferBytes >= 40964,
                "submitted input, packed-output and mapped status backing are all charged");
        if (iteration % 2 != 0)
            display.display->cancel();
        const auto status = wait(*display.display, GpuResidentDisplayPollResult::Pending);
        if (iteration % 2 != 0) {
            require(status == GpuResidentDisplayPollResult::Failure &&
                        display.display->diagnostic().code ==
                            GpuResidentDisplayDiagnosticCode::Cancelled,
                    "cancellation retires actual submitted buffers");
        } else {
            require(status == GpuResidentDisplayPollResult::Ready, "real display fence retires");
            const auto oracle = display.display->readback();
            require(oracle.hasValue() && oracle.pixels == expected,
                    "native resident display matches independent CPU pixels");
            auto image = display.display->takeImage();
            require(image.isValid() && budget.snapshot().ownedNativeBuffers == 0,
                    "take destroys retired working buffers before returning the image");
        }
        retire(device, budget, lease);
        VmaTotalStatistics settled{};
        vmaCalculateStatistics(state->allocator, &settled);
        require(settled.total.statistics.allocationBytes ==
                        baseline.total.statistics.allocationBytes &&
                    settled.total.statistics.allocationCount ==
                        baseline.total.statistics.allocationCount,
                "ten settled iterations return actual VMA suballocations to baseline");
    }
}
} // namespace
int main(int argc, char** argv) {
    try {
        bool requireDevice = false;
        for (int i = 1; i < argc; ++i) {
            if (std::string_view(argv[i]) == "--require-device")
                requireDevice = true;
            else
                return 2;
        }
        auto device = GpuDevice::create();
        if (!device) {
            std::cout << "Unsupported: " << device.diagnostic.message << '\n';
            return requireDevice ? 1 : 77;
        }
        backingAndRefusal(*device.device);
        nonlocalBacking(*device.device);
        productionCycles(*device.device);
        std::cout << "PASS: rooted resident processing buffers, native parity and retirement\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
