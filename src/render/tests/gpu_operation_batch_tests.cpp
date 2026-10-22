#include "gpu_operation_batch_private.hpp"
#include <bloom/render/processing_host_storage.hpp>

#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace {
using namespace bloom::render;
using Fault = operation_batch_detail::PollFault;
GpuProcessingPhaseBudget* scratchRoot = nullptr;
bool observedAxes = false;
VkResult observeAxes(const bool invalidate, VmaAllocator allocator, VmaAllocation allocation,
                     const VkDeviceSize offset, const VkDeviceSize bytes) noexcept {
    if (!invalidate && scratchRoot) {
        const auto claims = scratchRoot->snapshot();
        observedAxes = observedAxes ||
                       (claims.ownedProcessingHostBytes == 20 * sizeof(GpuAxisSample) &&
                        claims.ownedProcessingHostMetadataBytes >=
                            2 * ProcessingHostAllocator<GpuAxisSample>::allocationHeaderBytes() &&
                        claims.ownedProcessingHostAllocations == 2);
    }
    return invalidate ? vmaInvalidateAllocation(allocator, allocation, offset, bytes)
                      : vmaFlushAllocation(allocator, allocation, offset, bytes);
}
void require(const bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
GpuOperationBatchState complete(GpuOperationBatch& batch) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    auto status = batch.poll();
    while (status == GpuOperationBatchState::Pending &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
        status = batch.poll();
    }
    return status;
}
void run(GpuDevice& device) {
    constexpr std::uint64_t capacity = 64ULL * 1024 * 1024;
    GpuProcessingPhaseBudget root;
    root.setHostCapacity(capacity, true);
    root.setDeviceCapacity(capacity, true);

    require(device.bindProcessingRoot(root.rootIdentity()),
            "batch binds one stable allocation root");
    auto grant = root.reserve({.fixedHostBytes = 1,
                               .requestedExecutorBytes = capacity / 2,
                               .minimumExecutorBytes = 1,
                               .sourceFree = true});
    require(grant.lease && device.retainProcessingPhase(grant.lease),
            "batch retains actual phase grant");
    const auto control = GpuRendererAccess::state(device);
    const auto madeWindow = ImageWindow::create(-3, 5, 13, 7);
    const auto window = *madeWindow.value();
    const auto madePixel = Rgba32f::fromPremultiplied(0.25F, 0.125F, 0.0F, 0.5F);
    const auto pixel = *madePixel.value();
    const auto base =
        GpuBatchSolid{{pixel, window, window, bloom::core::PixelAspectRatio::square()}};
    const std::array<GpuBatchOperation, 4> ops{
        base, GpuBatchAffine{{nullptr, 0}, window, {1, 0, -2.75, 0, 1, 5.5}, 0.75F},
        GpuBatchSolid{
            {Rgba32f::transparent(), window, window, bloom::core::PixelAspectRatio::square()}},
        GpuBatchSourceOver{{nullptr, 1}, {nullptr, 2}}};
    auto batch = GpuOperationBatch::create(device);
    require(batch != nullptr, "bounded batch facade creates");
    // A generator with no image input still dispatches a genuine native kernel. The one-operation
    // case is reserved for GradientV1; it must retain the same descriptor and publish only after
    // a fence. The interpolated alpha is independent of RGB until final premultiplication.
    const auto madeGradientWindow = ImageWindow::create(0, 0, 4, 1);
    const auto gradientWindow = *madeGradientWindow.value();
    GpuBatchGradient gradient{gradientWindow, gradientWindow,
                              bloom::core::PixelAspectRatio::square()};
    gradient.first = {-1.0F, 0.0F, 2.0F, 1.0F};
    gradient.second = {3.0F, 0.0F, -2.0F, 0.0F};
    gradient.endpoints = {0.0F, 0.0F, 4.0F, 0.0F};
    gradient.exactWidth = 4.0F;
    gradient.exactHeight = 1.0F;
    const std::array<GpuBatchOperation, 1> gradientOps{gradient};
    require(batch->begin(gradientOps, capacity).code == GpuOperationBatchCode::None &&
                complete(*batch) == GpuOperationBatchState::Ready,
            "native gradient generator submits and retires");
    auto gradientImages = batch->takeImages();
    require(gradientImages.size() == 1, "gradient retains one native image");
    auto gradientReadback = readbackResidentImage(gradientImages.front(), capacity);
    require(gradientReadback.code == GpuImageReadbackCode::None &&
                gradientReadback.pixels.size() == 4,
            "gradient native image has expected extent");
    if (gradientReadback.pixels.size() == 4) {
        const auto& first = gradientReadback.pixels.front();
        const auto& last = gradientReadback.pixels.back();
        require(std::fabs(first.alpha() - 0.875F) < 0.0002F &&
                    std::fabs(first.red() + 0.4375F) < 0.0002F &&
                    std::fabs(last.alpha() - 0.125F) < 0.0002F &&
                    std::fabs(last.red() - 0.3125F) < 0.0002F,
                "native gradient interpolates HDR/negative straight RGB and alpha then premultiplies");
    }
    gradientImages.clear();
    device.reclaimReleasedImages();
    const auto before = root.snapshot();
    const auto baseline = device.allocatorMemorySnapshot();
    {
        const auto madeOutput = ImageWindow::create(0, 0, 16, 12);
        const auto output = *madeOutput.value();
        const auto par = *bloom::core::PixelAspectRatio::create(4, 3);
        const auto madeDescriptor = Rgba32fImageDescriptor::create(output, output, par);
        const auto descriptor = *madeDescriptor.value();
        const std::array<GpuBatchOperation, 2> resized{
            base, GpuBatchAffine{{nullptr, 0}, output, {1.25, 0, 0, 0, 1.5, 0}, 1.0F, descriptor}};
        require(batch->begin(resized, capacity).code == GpuOperationBatchCode::None &&
                    complete(*batch) == GpuOperationBatchState::Ready,
                "Resize/Fit affine output descriptor executes in the actual batch path");
        const auto images = batch->takeImages();
        require(images.size() == 2 && images[1].dataWindow() == output &&
                    images[1].displayWindow() == output && images[1].pixelAspect() == par,
                "batched affine publication retains the authored output display window and PAR");
    }
    // A malformed topological source is refused before any pixel backing allocation.
    auto invalid = ops;
    std::get<GpuBatchAffine>(invalid[1]).source.operation = 1;
    const auto attempts = control->nativeImageBackingAttempts;
    require(batch->begin(invalid, capacity).code == GpuOperationBatchCode::InvalidRequest &&
                control->nativeImageBackingAttempts == attempts + 1,
            "forward/self reference never allocates its own output and earlier output is cleaned");
    require(root.snapshot().totalDeviceClaimBytes == before.totalDeviceClaimBytes &&
                root.snapshot().ownedNativeImageBytes == 0,
            "malformed partial batch restores consumed origin prefix after destruction");
    // Failure inside the allocation helper must restore its own token and earlier jobs too.
    for (const auto fault : {vulkan_detail::NativeImageAllocationFault::Allocation,
                             vulkan_detail::NativeImageAllocationFault::Reconcile,
                             vulkan_detail::NativeImageAllocationFault::Bind,
                             vulkan_detail::NativeImageAllocationFault::View}) {
        control->nativeImageAllocationFault = fault;
        require(batch->begin(ops, capacity).code == GpuOperationBatchCode::AllocationFailed &&
                    !batch->hasUnretiredSubmission() && batch->statistics().submissions == 0 &&
                    root.snapshot().ownedNativeImageBytes == 0 &&
                    root.snapshot().ownedNativeBufferBytes == 0 &&
                    root.snapshot().totalDeviceClaimBytes == before.totalDeviceClaimBytes,
                "every image allocation/reconcile/bind/view failure restores exact original prefix "
                "after backing destruction");
    }
    for (const auto fault : {vulkan_detail::NativeBufferAllocationFault::Allocation,
                             vulkan_detail::NativeBufferAllocationFault::Bind,
                             vulkan_detail::NativeBufferAllocationFault::Map}) {
        control->nativeBufferAllocationFault = fault;
        require(batch->begin(ops, capacity).code == GpuOperationBatchCode::AllocationFailed,
                "native backing refusal occurs before submission");
        require(
            !batch->hasUnretiredSubmission() && batch->statistics().submissions == 0 &&
                root.snapshot().ownedNativeImageBytes == 0 &&
                root.snapshot().ownedNativeBufferBytes == 0 &&
                root.snapshot().totalDeviceClaimBytes == before.totalDeviceClaimBytes &&
                root.snapshot().totalHostClaimBytes == before.totalHostClaimBytes,
            "helper and earlier actual backing free before exact same-phase prefix restoration");
    }
    auto nonfinite = ops;
    std::get<GpuBatchAffine>(nonfinite[1]).matrix.a = std::numeric_limits<double>::infinity();
    require(batch->begin(nonfinite, capacity).code == GpuOperationBatchCode::InvalidRequest &&
                batch->statistics().submissions == 0,
            "nonfinite affine fields refuse before any submission");
    const auto farWindow = ImageWindow::create(INT64_MIN, 0, 1, 1);
    const auto nearWindow = ImageWindow::create(INT64_MAX - 1, 0, 1, 1);
    const std::array<GpuBatchOperation, 3> overflowingOffset{
        GpuBatchSolid{{pixel, *farWindow.value(), window, bloom::core::PixelAspectRatio::square()}},
        GpuBatchSolid{
            {pixel, *nearWindow.value(), window, bloom::core::PixelAspectRatio::square()}},
        GpuBatchSourceOver{{nullptr, 0}, {nullptr, 1}}};
    require(batch->begin(overflowingOffset, capacity).code ==
                    GpuOperationBatchCode::InvalidRequest &&
                batch->statistics().submissions == 0,
            "source-over checked origin subtraction rejects int64 overflow before submit");
    require(batch->begin(ops, 1).code == GpuOperationBatchCode::OverBudget &&
                batch->statistics().submissions == 0,
            "whole-batch requested headroom refusal never submits");
    {
        struct Hook final {
            composite_detail::MappedMemoryHook previous;
            ~Hook() {
                composite_detail::mappedMemoryHook.store(previous);
                scratchRoot = nullptr;
            }
        } hook{composite_detail::mappedMemoryHook.exchange(observeAxes)};
        scratchRoot = &root;
        const auto translatedDisplay = ImageWindow::create(-2, -1, 17, 13);
        const std::array<GpuBatchOperation, 2> translation{
            base, GpuBatchTranslation{{nullptr, 0}, window, 0.25, -0.3, 0.75F,
                                      *translatedDisplay.value()}};
        require(batch->begin(translation, capacity).code == GpuOperationBatchCode::None &&
                    observedAxes && root.snapshot().ownedProcessingHostBytes == 0 &&
                    root.snapshot().ownedProcessingHostMetadataBytes == 0 &&
                    complete(*batch) == GpuOperationBatchState::Ready,
                "axis allocator admits exact additive host payload/headers during real staging and "
                "frees before GPU publication");
        auto translated = batch->takeImages();
        require(translated.size() == 2, "tracked-axis native batch retires actual outputs");
        require(translated[0].displayWindow() == window &&
                    translated[1].displayWindow() == *translatedDisplay.value() &&
                    translated[1].dataWindow() == window,
                "translation batch preserves its display override without changing source geometry");
    }
    for (int cycle = 0; cycle < 10; ++cycle) {
        require(batch->begin(ops, capacity).code == GpuOperationBatchCode::None,
                "actual four-kernel batch starts");
        require(batch->statistics().submissions == 1 && batch->statistics().dispatches == 4 &&
                    batch->statistics().peakAllocationBytes > batch->statistics().imageBytes,
                "one actual queue submission owns four real dispatches and metadata");
        require(batch->takeImages().empty(), "pending internal images never escape");
        if (cycle % 2 != 0)
            batch->cancel();
        const auto result = complete(*batch);
        require(result == (cycle % 2 == 0 ? GpuOperationBatchState::Ready
                                          : GpuOperationBatchState::Failure),
                "capacity-one completion/cancellation retires real native work");
        if (cycle % 2 == 0) {
            require(batch->begin(ops, capacity).code == GpuOperationBatchCode::Busy,
                    "ready but untaken outputs retain exclusive batch ownership");
            auto outputs = batch->takeImages();
            require(outputs.size() == 4, "every proven output is retained in topological order");
            auto read = readbackResidentImage(outputs.back(), capacity);
            require(read.code == GpuImageReadbackCode::None && read.pixels.size() == 91,
                    "final batch pixels read through real native oracle");
            require(outputs.back().dataWindow() == window &&
                        outputs.back().displayWindow() == window,
                    "composite output preserves descriptor geometry");
        }
        device.reclaimReleasedImages();
    }
    // Late status rejection discards every output, even those from earlier successful kernels.
    const auto madeHuge = Rgba32f::fromPremultiplied(std::numeric_limits<float>::max(), 0, 0, 0.5F);
    const auto huge = *madeHuge.value();
    const std::array<GpuBatchOperation, 3> rejected{
        GpuBatchSolid{{huge, window, window, bloom::core::PixelAspectRatio::square()}},
        GpuBatchSolid{{huge, window, window, bloom::core::PixelAspectRatio::square()}},
        GpuBatchSourceOver{{nullptr, 0}, {nullptr, 1}}};
    require(batch->begin(rejected, capacity).code == GpuOperationBatchCode::None &&
                complete(*batch) == GpuOperationBatchState::Failure &&
                batch->diagnostic().code == GpuOperationBatchCode::StatusRejected &&
                batch->takeImages().empty(),
            "genuine overflow status in the last kernel rejects the entire batch");
    require(batch->begin(ops, capacity).code == GpuOperationBatchCode::None,
            "unknown-fence fixture really submits");
    // Private negative-only seam: never simulates a successful fence or pixel result.
    std::thread quarantine([owned = std::move(batch)]() mutable { owned.reset(); });
    quarantine.join();
    auto* const retired = control->retiredOperationBatch.load(std::memory_order_acquire);
    require(retired != nullptr, "foreign final release retains exact unknown-fence fixture owner");
    if (retired) {
        retired->fault = Fault::Unknown;
        const auto owned = root.snapshot().ownedNativeImageBytes;
        operation_batch_detail::drainRetired(*control);
        require(control->retiredOperationBatch.load() == retired &&
                    root.snapshot().ownedNativeImageBytes == owned &&
                    control->operationBatchOccupied.load(),
                "unknown fence retains exact entire native owner and occupied bounded slot");
        retired->fault = Fault::None;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        device.reclaimReleasedImages();
        std::this_thread::yield();
    } while (control->operationBatchOccupied.load() && std::chrono::steady_clock::now() < deadline);
    require(!control->operationBatchOccupied.load(), "actual fence proof recovers bounded slot");
    batch = GpuOperationBatch::create(device);
    require(batch->begin(ops, capacity).code == GpuOperationBatchCode::None,
            "foreign release fixture really submits");
    GpuOperationBatchState foreign = GpuOperationBatchState::Idle;
    bool foreignGetters = false;
    std::thread release([&foreign, &foreignGetters, owned = std::move(batch)]() mutable {
        foreignGetters = owned->state() == GpuOperationBatchState::WrongThread &&
                         owned->diagnostic().code == GpuOperationBatchCode::WrongThread &&
                         owned->statistics().submissions == 0 && owned->hasUnretiredSubmission();
        foreign = owned->poll();
        owned.reset();
    });
    release.join();
    require(foreignGetters && foreign == GpuOperationBatchState::WrongThread &&
                control->retiredOperationBatch.load() != nullptr,
            "foreign polling/release does not touch Vulkan and retains exact owner");
    const auto foreignDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        device.reclaimReleasedImages();
        std::this_thread::yield();
    } while (control->operationBatchOccupied.load() &&
             std::chrono::steady_clock::now() < foreignDeadline);
    require(!control->operationBatchOccupied.load() && root.snapshot().ownedNativeImageBytes == 0 &&
                root.snapshot().ownedNativeBufferBytes == 0 &&
                device.allocatorMemorySnapshot().allocationBytes == baseline.allocationBytes,
            "settled native payload and exact root charges return to baseline");
    batch = GpuOperationBatch::create(device);
    require(batch->begin(ops, capacity).code == GpuOperationBatchCode::None,
            "device-loss retirement fixture genuinely submits");
    std::thread lostRelease([owned = std::move(batch)]() mutable { owned.reset(); });
    lostRelease.join();
    auto* const lostOwner = control->retiredOperationBatch.load();
    require(lostOwner != nullptr, "device-loss fixture retains exact native owner");
    lostOwner->fault = Fault::Pending;
    operation_batch_detail::drainRetired(*control);
    require(control->retiredOperationBatch.load() == lostOwner &&
                root.snapshot().ownedNativeImageBytes > 0,
            "negative pending fault cannot create retirement proof");
    lostOwner->fault = Fault::LostAfterProof;
    const auto lostDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        device.reclaimReleasedImages();
        std::this_thread::yield();
    } while (control->operationBatchOccupied.load() &&
             std::chrono::steady_clock::now() < lostDeadline);
    require(!control->operationBatchOccupied.load() && root.snapshot().ownedNativeImageBytes == 0 &&
                root.snapshot().ownedNativeBufferBytes == 0,
            "injected loss only after genuine fence proof frees all retained native owners");
    device.retireProcessingPhase(grant.lease->id(), true);
    grant.lease.reset();
}
} // namespace
int main(int argc, char** argv) {
    try {
        bool required = false;
        GpuDeviceCreationOptions options;
        for (int index = 1; index < argc; ++index) {
            const std::string_view arg = argv[index];
            if (arg == "--require-device")
                required = true;
            else if (arg == "--loader" && index + 1 < argc)
                options.loader_path = argv[++index];
            else
                return 2;
        }
        auto device = GpuDevice::create(options);
        if (!device) {
            std::cout << "Unsupported: " << device.diagnostic.message << '\n';
            return required ? 1 : 77;
        }
        run(*device.device);
        std::cout
            << "PASS: bounded actual multi-kernel command batch, status/admission/retirement\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
