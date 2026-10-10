#include "gpu_composite_native_support.hpp"
#include "gpu_device_private.hpp"
#include "gpu_display_transfer_fault.hpp"
#include "gpu_native_buffer_allocation_private.hpp"
#include "gpu_resident_display_private.hpp"

#include <bloom/render/gpu_display_transfer.hpp>
#include <bloom/render/packed_host_retirement.hpp>

#include <algorithm>
#include <chrono>
#include <thread>

namespace {
using namespace bloom::render;
using namespace bloom::render::composite_proof;

std::shared_ptr<const GpuDisplayImage> finalImage(Expectations& checks, GpuDevice& device) {
    auto solidOp = GpuSolid::create(device);
    auto display = GpuResidentDisplay::create(device);
    checks.expect(solidOp.hasValue() && display.hasValue(), "production GPU preparation available");
    if (!solidOp || !display)
        return {};
    const auto w = window(-3, 7, 8, 4);
    auto scene = solid(*solidOp.solid, {0.25, 0.5, 1.0, 1.0}, w, w, PixelAspectRatio::square());
    checks.expect(scene.has_value(), "GPU solid prepared");
    if (!scene)
        return {};
    auto input = std::make_shared<const GpuImage>(std::move(*scene));
    checks.expect(display.display->begin(input, kBudget).code ==
                      GpuResidentDisplayDiagnosticCode::None,
                  "real final display dispatch admitted");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    auto result = display.display->poll();
    while (result == GpuResidentDisplayPollResult::Pending &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
        result = display.display->poll();
    }
    checks.expect(result == GpuResidentDisplayPollResult::Ready, "final GPU image complete");
    if (result != GpuResidentDisplayPollResult::Ready)
        return {};
    return std::make_shared<const GpuDisplayImage>(display.display->takeImage());
}

GpuDisplayTransferState complete(GpuDisplayTransfer& transfer) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    auto state = transfer.poll();
    while (state == GpuDisplayTransferState::Pending &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
        state = transfer.poll();
    }
    return state;
}

void roundTrip(Expectations& checks, GpuDevice& device,
               const std::shared_ptr<const GpuDisplayImage>& source,
               const GpuDeviceCreationOptions& options,
               const std::shared_ptr<GpuProcessingPhaseLease>& phase = {}) {
    const auto oracle = readbackResidentDisplayImage(*source, kBudget);
    checks.expect(oracle.hasValue(), "debug oracle exists only in test");
    const auto w = window(-3, 7, 8, 4);
    std::vector<Rgba32f> process(32, pixel(0.25F, 0.5F, 1.0F, 1.0F));
    const auto cpuImage = makeImage(w, w, PixelAspectRatio::square(), process);
    checks.expect(cpuImage.has_value(), "CPU reference image exists");
    std::vector<Rgba8> reference(32);
    if (cpuImage) {
        const auto view = cpuImage->view();
        for (std::int64_t y = 7; y < 11; ++y)
            checks.expect(
                !mapLinearRec709SceneToSrgbRow(
                    *view.value(), w, y,
                    std::span<Rgba8>(reference).subspan(static_cast<std::size_t>(y - 7) * 8, 8)),
                "CPU reference maps each row");
    }
    GpuDisplayTransfer transfer;
    checks.expect(transfer.begin(device, source, kBudget, phase), "final RGBA8 transfer admitted");
    const auto retention = transfer.retention();
    const auto pendingBytes = retention->snapshot();
    checks.expect(pendingBytes.sourceBytes == source->allocationBytes() &&
                      pendingBytes.stagingBytes == transfer.accounting().stagingAllocationBytes &&
                      pendingBytes.hostBytes == transfer.accounting().hostStorageBytes &&
                      pendingBytes.reservedBytes >= pendingBytes.sourceBytes +
                                                        pendingBytes.stagingBytes +
                                                        pendingBytes.hostBytes,
                  "independent reservation owns every actual pending allocation");
    checks.expect(!transfer.reset() && transfer.hasUnretiredSubmission(),
                  "reset refuses pending native resource ownership");
    checks.expect(gpuDisplayImageImpl(*source)->submissionUnretired.load(),
                  "source cannot admit a competing readback until retirement");
    GpuDisplayTransfer busy;
    checks.expect(!busy.begin(device, source, kBudget, phase),
                  "second submission cannot steal live source");
    checks.expect(complete(transfer) == GpuDisplayTransferState::Ready, "transfer completes");
    checks.expect(transfer.isOwnerThread() && !transfer.hasUnretiredSubmission() &&
                      !gpuDisplayImageImpl(*source)->submissionUnretired.load(),
                  "Ready retains owner identity after releasing actual native resources");
    const auto readyBytes = retention->snapshot();
    checks.expect(readyBytes.sourceBytes == 0 && readyBytes.stagingBytes == 0 &&
                      readyBytes.hostBytes == 128 && readyBytes.reservedBytes != 0,
                  "Ready frees proved native bytes but retains charged host capacity");
    auto output = transfer.take();
    checks.expect(retention->snapshot().reservedBytes == 0 && retention->snapshot().hostBytes == 0,
                  "take transfers host ownership out of primitive reservation");
    checks.expect(output.has_value() && !transfer.take(), "Ready result is taken exactly once");
    if (!output)
        return;
    checks.expect(std::ranges::equal(output->pixels(), oracle.pixels) &&
                      std::ranges::equal(output->pixels(), reference),
                  "packed transfer matches final GPU bytes and CPU reference exactly");
    checks.expect(output->dataWindow() == w && output->displayWindow() == w &&
                      output->pixelAspect() == PixelAspectRatio::square() &&
                      output->isBoundTo(device),
                  "result preserves source geometry and exact device binding");
    auto other = GpuDevice::create(options);
    checks.expect(other.hasValue() && !output->isBoundTo(*other.device),
                  "same numeric generation is not cross-device identity");
    const auto charge = output->accounting();
    checks.expect(
        charge.submissions == 1 && charge.logicalBytes == 128 &&
            charge.stagingAllocationBytes >= charge.logicalBytes &&
            charge.hostStorageBytes >= charge.logicalBytes &&
            charge.peakTransferBytes == charge.stagingAllocationBytes + charge.hostStorageBytes &&
            charge.sourceAllocationBytes == source->allocationBytes(),
        "actual allocator-rounded staging, host capacity and separate source charges exact");
    std::cout << "TRANSFER_CHARGES logical=" << charge.logicalBytes
              << " staging=" << charge.stagingAllocationBytes << " host=" << charge.hostStorageBytes
              << " source=" << charge.sourceAllocationBytes << '\n';
    GpuDisplayTransfer refusal;
    checks.expect(!refusal.begin(device, source, charge.peakTransferBytes - 1, phase) &&
                      refusal.diagnostic().code == GpuDisplayTransferCode::OverBudget &&
                      refusal.accounting().submissions == 0 && !refusal.hasUnretiredSubmission(),
                  "real concurrent allocation budget refusal submits nothing");
    checks.expect(refusal.reset() && refusal.begin(device, source, charge.peakTransferBytes, phase),
                  "refused primitive can admit exact measured budget");
    checks.expect(complete(refusal) == GpuDisplayTransferState::Ready,
                  "exact measured budget completes");
    refusal.cancel();
    checks.expect(!refusal.take() && refusal.diagnostic().code == GpuDisplayTransferCode::Cancelled,
                  "cancelled Ready discards packed host storage");
    GpuDisplayTransfer overflow;
    checks.expect(!overflow.begin(device, source, UINT64_MAX, phase) &&
                      overflow.diagnostic().code == GpuDisplayTransferCode::OverBudget &&
                      !overflow.hasUnretiredSubmission(),
                  "maximum budget cannot wrap reservation to zero");
    checks.expect(
        overflow.reset() &&
            !overflow.begin(device, source, UINT64_MAX - source->allocationBytes() + 1, phase) &&
            overflow.diagnostic().code == GpuDisplayTransferCode::OverBudget,
        "exact-wrap budget cannot advertise an empty live reservation");
    GpuDisplayTransfer wrongDevice;
    checks.expect(!wrongDevice.begin(*other.device, source, kBudget) &&
                      wrongDevice.diagnostic().code == GpuDisplayTransferCode::DeviceUnavailable,
                  "begin refuses a source from another device even on same owner");
}

void ownerAndCancellation(Expectations& checks, GpuDevice& device,
                          const std::shared_ptr<const GpuDisplayImage>& source) {
    GpuDisplayTransfer transfer;
    checks.expect(transfer.begin(device, source, kBudget), "owner regression admitted");
    std::thread foreign([&] {
        checks.expect(!transfer.isOwnerThread() &&
                          transfer.poll() == GpuDisplayTransferState::Pending &&
                          transfer.diagnostic().code == GpuDisplayTransferCode::WrongThread &&
                          !transfer.reset(),
                      "foreign thread cannot poll/free/reset native resources");
    });
    foreign.join();
    // Same thread id, invalidated device owner lifetime: exercise exact lifetime protection without
    // depending on the OS reusing a thread id. Restore before any real retirement call.
    auto state = gpuDisplayImageImpl(*source)->state;
    const auto token = state->ownerLifetimeToken;
    state->ownerLifetimeToken = token + 1;
    checks.expect(!transfer.isOwnerThread() &&
                      transfer.poll() == GpuDisplayTransferState::Pending &&
                      transfer.diagnostic().code == GpuDisplayTransferCode::WrongThread &&
                      !transfer.reset() && transfer.hasUnretiredSubmission(),
                  "same thread id cannot retire a replaced owner lifetime");
    transfer.cancel(); // must not cancel work owned by another lifetime
    state->ownerLifetimeToken = token;
    checks.expect(complete(transfer) == GpuDisplayTransferState::Ready,
                  "real owner can complete after refused foreign lifetime");
    checks.expect(transfer.take().has_value(), "wrong lifetime cancellation changed no native job");
    checks.expect(transfer.begin(device, source, kBudget), "cancel regression admitted");
    transfer.cancel();
    checks.expect(complete(transfer) == GpuDisplayTransferState::Failure &&
                      transfer.diagnostic().code == GpuDisplayTransferCode::Cancelled &&
                      !transfer.hasUnretiredSubmission() && !transfer.take(),
                  "Pending cancellation publishes nothing and proves resource retirement");
}

void quarantine(Expectations& checks, GpuDevice& device,
                const std::shared_ptr<const GpuDisplayImage>& source) {
    using namespace display_transfer_detail;
    GpuDisplayTransfer transfer;
    checks.expect(transfer.begin(device, source, kBudget), "quarantine regression admitted");
    setTransferFaultForTest(TransferFault::ForceFenceTimeout);
    transfer.cancel();
    checks.expect(transfer.poll() == GpuDisplayTransferState::Failure &&
                      transfer.hasUnretiredSubmission() && !transfer.reset() && !transfer.take(),
                  "logical timeout/cancel retains unproved native resources and source");
    setTransferFaultForTest(TransferFault::None);
    auto state = gpuDisplayImageImpl(*source)->state;
    const auto token = state->ownerLifetimeToken;
    state->ownerLifetimeToken = token + 1;
    static_cast<void>(transfer.poll());
    checks.expect(transfer.hasUnretiredSubmission() && !transfer.reset(),
                  "quarantine cannot reclaim resources in a replaced owner lifetime");
    state->ownerLifetimeToken = token;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (transfer.hasUnretiredSubmission() && std::chrono::steady_clock::now() < deadline) {
        static_cast<void>(transfer.poll());
        std::this_thread::yield();
    }
    checks.expect(!transfer.hasUnretiredSubmission() && transfer.reset() &&
                      !gpuDisplayImageImpl(*source)->submissionUnretired.load(),
                  "real fence observation clears quarantine and source marker");
    checks.expect(transfer.begin(device, source, kBudget) &&
                      complete(transfer) == GpuDisplayTransferState::Ready &&
                      transfer.take().has_value(),
                  "retired slot can be reused without scene reevaluation");
    std::shared_ptr<const GpuDisplayTransferRetention> destroyedReservation;
    {
        GpuDisplayTransfer destroyed;
        checks.expect(destroyed.begin(device, source, kBudget), "destructor timeout admitted");
        destroyedReservation = destroyed.retention();
        setTransferFaultForTest(TransferFault::ForceFenceTimeout);
        checks.expect(destroyed.poll() == GpuDisplayTransferState::Failure &&
                          destroyed.hasUnretiredSubmission(),
                      "destructor owns real quarantine");
        setTransferFaultForTest(TransferFault::None);
    }
    const auto orphaned = destroyedReservation->snapshot();
    checks.expect(orphaned.reservedBytes != 0 &&
                      orphaned.sourceBytes == source->allocationBytes() &&
                      orphaned.stagingBytes != 0 && orphaned.hostBytes == 0,
                  "destruction frees host storage but quarantined native bytes remain charged");
    GpuDisplayTransfer recreated;
    bool admitted = false;
    const auto recoveryDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!admitted && std::chrono::steady_clock::now() < recoveryDeadline) {
        admitted = recreated.begin(device, source, kBudget);
        if (!admitted) {
            checks.expect(recreated.reset(), "unsubmitted recovery refusal stays reusable");
            std::this_thread::yield();
        }
    }
    checks.expect(admitted && complete(recreated) == GpuDisplayTransferState::Ready &&
                      recreated.take().has_value(),
                  "new primitive retires destroyed quarantine before checking same source marker");
    checks.expect(destroyedReservation->snapshot().reservedBytes == 0 &&
                      destroyedReservation->snapshot().sourceBytes == 0 &&
                      destroyedReservation->snapshot().stagingBytes == 0,
                  "only genuine fence retirement releases orphaned reservation");
}
#include "gpu_display_transfer_storage_tests.ipp"
} // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parseOptions(argc, argv);
        if (!options.valid)
            return 2;
        GpuDeviceCreationOptions creation;
        creation.loader_path = options.loader_path;
        auto device = GpuDevice::create(creation);
        if (!device) {
            std::cout << "Unsupported: GPU device unavailable: " << device.diagnostic.message
                      << '\n';
            return options.require_device ? 1 : 77;
        }
        Expectations checks;
        const auto source = finalImage(checks, *device.device);
        if (source) {
            roundTrip(checks, *device.device, source, creation);
            ownerAndCancellation(checks, *device.device, source);
            quarantine(checks, *device.device, source);
        }
        actualBufferStorage(*device.device);
        rootedBufferTransfers(checks, *device.device, creation);
        if (checks.failures())
            return 1;
        std::cout
            << "PASS: final RGBA8 display transfer parity/accounting/cancellation/owner lifetime\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
