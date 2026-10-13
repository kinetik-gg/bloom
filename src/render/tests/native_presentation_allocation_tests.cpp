#include "gpu_present_image_private.hpp"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace bloom::render;
constexpr std::uint64_t kBudget = 16ULL * 1024 * 1024;
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
void run(GpuDevice& device) {
    GpuProcessingPhaseBudget budget(1);
    budget.setHostCapacity(kBudget, true);
    budget.setDeviceCapacity(kBudget, true);
    require(device.bindProcessingRoot(budget.rootIdentity()) &&
                device.processingRootIdentity() == budget.rootIdentity(),
            "native presentation binds explicit stable root without a live phase");
    GpuProcessingPhaseBudget alien;
    alien.setHostCapacity(kBudget, true);
    alien.setDeviceCapacity(kBudget, true);
    auto alienPhase =
        alien.reserve({.fixedHostBytes = 1, .deviceWorkingBytes = 1, .sourceFree = true});
    require(!device.bindProcessingRoot(alien.rootIdentity()) &&
                !device.retainProcessingPhase(alienPhase.lease),
            "bound native device refuses root switch and foreign-root frame authority");
    const auto state = GpuRendererAccess::state(device);
    VmaTotalStatistics baseline{};
    vmaCalculateStatistics(state->allocator, &baseline);
    present_image_detail::PresentImagePipeline presenter;
    std::string message;
    require(presenter.ensureDeviceResources(state, message) &&
                presenter.paramsStorageToken.bytes() != 0 &&
                budget.snapshot().ownedPresentationBuffers == 1 &&
                budget.snapshot().admittedPhases == 0,
            "actual mapped uniform allocation owns an independent presentation charge");
    const auto paramsBytes = presenter.paramsStorageToken.bytes();
    std::array<std::uint8_t, std::size_t{64} * 64 * 4> pixels{};
    const GpuPresentOverlay firstOverlay{
        .pixels = pixels.data(), .width = 32, .height = 32, .token = 1};
    budget.setDeviceCapacity(paramsBytes, true);
    const auto imageAttempts = state->nativeImageBackingAttempts;
    require(!presenter.ensureOverlay(*state, firstOverlay, message) &&
                state->nativeImageBackingAttempts == imageAttempts &&
                budget.snapshot().ownedPresentationImages == 0 &&
                budget.snapshot().ownedPresentationBuffers == 1,
            "overlay image refusal precedes VMA backing and preserves existing uniform charge");
    const auto imageRefused = device.nativePresentationStorageRefusal();
    require(imageRefused && imageRefused->image &&
                imageRefused->domain == GpuProcessingPhaseDomain::Device,
            "overlay image has distinct independent-root capacity refusal provenance");
    budget.setDeviceCapacity(kBudget, true);
    for (auto fault : {vulkan_detail::NativeImageAllocationFault::Allocation,
                       vulkan_detail::NativeImageAllocationFault::Reconcile,
                       vulkan_detail::NativeImageAllocationFault::Bind}) {
        state->nativeImageAllocationFault = fault;
        require(!presenter.ensureOverlay(*state, firstOverlay, message) &&
                    budget.snapshot().ownedPresentationImages == 0 &&
                    !device.nativePresentationStorageRefusal(),
                "actual overlay allocation/reconcile/bind fault frees backing before independent "
                "claim");
    }
    for (std::uint64_t iteration = 1; iteration != 11; ++iteration) {
        const std::uint32_t extent = iteration % 2 == 0 ? 64 : 32;
        GpuPresentOverlay overlay{
            .pixels = pixels.data(), .width = extent, .height = extent, .token = iteration};
        require(presenter.ensureOverlay(*state, overlay, message) &&
                    budget.snapshot().ownedPresentationImages == 1 &&
                    budget.snapshot().ownedPresentationBuffers == 2 &&
                    presenter.overlayStorageToken.bytes() != 0 &&
                    presenter.stagedOverlayStorageToken.bytes() != 0,
                "actual overlay resize and mapped staging each retain exact presentation backing");
        VmaAllocationInfo actualImage{};
        VmaAllocationInfo actualStaging{};
        vmaGetAllocationInfo(state->allocator, presenter.overlayAllocation, &actualImage);
        vmaGetAllocationInfo(state->allocator, presenter.stagedOverlayAllocation, &actualStaging);
        require(
            presenter.overlayStorageToken.bytes() == actualImage.size &&
                presenter.overlayStorageToken.identity().memoryType == actualImage.memoryType &&
                presenter.stagedOverlayStorageToken.bytes() == actualStaging.size &&
                presenter.stagedOverlayStorageToken.identity().memoryType ==
                    actualStaging.memoryType,
            "overlay image/staging tokens reconcile actual native backing size and memory type");
    }
    auto phase =
        budget.reserve({.fixedHostBytes = 1, .deviceWorkingBytes = 1024, .sourceFree = true});
    require(phase.lease && device.retainProcessingPhase(phase.lease),
            "same-root frame is admitted");
    require(budget.snapshot().deviceReservedBytes == 1024,
            "existing presentation cannot borrow the same-root frame's working debit");
    device.retireProcessingPhase(phase.lease->id(), true);
    phase.lease.reset();
    require(budget.snapshot().admittedPhases == 0 && budget.snapshot().ownedPresentationImages == 1,
            "whole-frame retirement leaves independent presenter ownership intact");
    presenter.resetDeviceResources();
    budget.setDeviceCapacity(paramsBytes - 1, true);
    const auto attempts = state->nativeBufferBackingAttempts;
    require(!presenter.ensureDeviceResources(state, message) &&
                state->nativeBufferBackingAttempts == attempts,
            "one-byte-short presentation uniform refuses before actual allocator backing");
    const auto refused = device.nativePresentationStorageRefusal();
    require(refused && refused->status == GpuProcessingPhaseAdmission::BudgetRefused &&
                refused->domain == GpuProcessingPhaseDomain::Device && !refused->image,
            "native presentation refusal has its own phase-independent typed provenance");
    budget.setDeviceCapacity(kBudget, true);
    budget.setHostCapacity(kBudget, false);
    const bool hostUnavailable = !presenter.ensureDeviceResources(state, message);
    const auto awaiting = device.nativePresentationStorageRefusal();
    require(hostUnavailable && awaiting &&
                awaiting->status == GpuProcessingPhaseAdmission::AwaitingCapacity,
            "presentation closes new backing admission while host delivery is unavailable");
    budget.setHostCapacity(kBudget, true);
    for (auto fault : {vulkan_detail::NativeBufferAllocationFault::Allocation,
                       vulkan_detail::NativeBufferAllocationFault::Reconcile,
                       vulkan_detail::NativeBufferAllocationFault::Bind,
                       vulkan_detail::NativeBufferAllocationFault::Map}) {
        state->nativeBufferAllocationFault = fault;
        require(!presenter.ensureDeviceResources(state, message) &&
                    budget.snapshot().ownedPresentationBuffers == 0 &&
                    !device.nativePresentationStorageRefusal(),
                "actual uniform fault destroys backing before charge release without capacity "
                "provenance");
    }
    require(presenter.ensureDeviceResources(state, message),
            "same presenter recovers after real refusal/fault cleanup");
    presenter.resetDeviceResources();
    const auto settled = budget.snapshot();
    VmaTotalStatistics actual{};
    vmaCalculateStatistics(state->allocator, &actual);
    require(
        settled.totalDeviceClaimBytes == 0 && settled.totalHostClaimBytes == 0 &&
            settled.ownedPresentationImages == 0 && settled.ownedPresentationBuffers == 0 &&
            actual.total.statistics.allocationCount == baseline.total.statistics.allocationCount &&
            actual.total.statistics.allocationBytes == baseline.total.statistics.allocationBytes,
        "owner retirement returns exact native claims and actual VMA payload/count to baseline");
}
} // namespace
int main(int argc, char** argv) {
    try {
        bool requireDevice = false;
        for (int i = 1; i < argc; ++i) {
            if (std::string_view(argv[i]) != "--require-device")
                return 2;
            requireDevice = true;
        }
        auto device = GpuDevice::create();
        if (!device) {
            std::cout << "Unsupported: " << device.diagnostic.message << '\n';
            return requireDevice ? 1 : 77;
        }
        run(*device.device);
        std::cout
            << "PASS: actual independent presentation allocations/refusal/resize/retirement\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
