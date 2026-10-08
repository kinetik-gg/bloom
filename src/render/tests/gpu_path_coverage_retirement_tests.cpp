// Production vector dispatch quarantine, exact allocation retention, and eventual owner reclaim.
#include "gpu_path_coverage_fault.hpp"
#include "gpu_path_coverage_test_support.hpp"

#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

namespace bloom::render::gpu_path_coverage_test {

// Bounded-reservation lifecycle: forced submit failure, forced fence timeout with quarantine and
// retirement, and forced device loss, each followed by reuse.
void testFaultLifecycle(Expectations& expectations, GpuDevice& device) {
    using namespace bloom::render::path_coverage_detail;
    auto created = GpuPathCoverage::create(device);
    expectations.expect(created.hasValue(), "the fault-lifecycle producer is created");
    if (!created) {
        return;
    }
    GpuPathCoverage& producer = *created.coverage;
    const auto window = ImageWindow::create(0, 0, 16, 8);
    const auto raster = PathRaster::create(rectanglePath(14.0, 6.0), {}, 1, 1);
    expectations.expect(static_cast<bool>(window) && static_cast<bool>(raster),
                        "the fault window and raster build");
    if (!window || !raster) {
        return;
    }
    const auto geometry = raster.value()->coverageGeometry(
        window.value()->originX(), window.value()->originY(), window.value()->extent().width(),
        window.value()->extent().height(), PathFillRule::NonZero, false);
    expectations.expect(static_cast<bool>(geometry), "the fault geometry builds");
    if (!geometry) {
        return;
    }
    const GpuPathCoverageParameters parameters{*window.value(), *window.value(),
                                               PixelAspectRatio::square()};
    std::vector<std::uint8_t> measured;

    // Forced submit failure happens before any in-flight submission: the claim is released and the
    // producer is reusable.
    setPathCoverageFaultForTest(PathCoverageFault::FailSubmit);
    expectations.expect(producer.begin(parameters, *geometry.value(), 1ULL << 34ULL).code ==
                            GpuPathCoverageDiagnosticCode::DeviceUnavailable,
                        "a forced submit failure is reported");
    expectations.expect(!pathCoverageQuarantineOccupiedForTest(),
                        "a forced submit failure does not quarantine");
    setPathCoverageFaultForTest(PathCoverageFault::None);
    expectations.expect(runCoverage(producer, *window.value(), PathFillRule::NonZero, false,
                                    *raster.value(), measured),
                        "the producer is reusable after a forced submit failure");

    // Establish a genuine allocator baseline with no resident mask from the preceding parity run.
    expectations.expect(producer.begin(parameters, *geometry.value(), 1ULL << 34ULL).code ==
                            GpuPathCoverageDiagnosticCode::None,
                        "allocator baseline begin is accepted");
    producer.cancel();
    static_cast<void>(pollToCompletion(producer));
    const auto allocatorBefore = pathCoverageAllocatorStats(device);

    // Forced fence timeout: the exact submission is quarantined, admission is refused, then the
    // owner retires it and the producer is reusable.
    setPathCoverageFaultForTest(PathCoverageFault::ForceFenceTimeout);
    expectations.expect(producer.begin(parameters, *geometry.value(), 1ULL << 34ULL).code ==
                            GpuPathCoverageDiagnosticCode::None,
                        "the fence-timeout begin is accepted");
    expectations.expect(pollToCompletion(producer) == GpuPathCoveragePollResult::Failure &&
                            producer.diagnostic().code ==
                                GpuPathCoverageDiagnosticCode::NativeTimeout,
                        "a forced fence timeout reports NativeTimeout");
    const auto retainedBytes = producer.lastJobAllocationBytes();
    const auto allocatorRetained = pathCoverageAllocatorStats(device);
    expectations.expect(allocatorRetained.allocationCount == allocatorBefore.allocationCount + 3 &&
                            allocatorRetained.allocationBytes ==
                                allocatorBefore.allocationBytes + retainedBytes,
                        "real VMA statistics retain all three actual allocations");
    expectations.expect(pathCoverageQuarantineOccupiedForTest(), "the quarantine is occupied");
    expectations.expect(
        producer.hasUnretiredSubmission() && retainedBytes != 0 &&
            pathCoverageQuarantineAllocationBytes() == retainedBytes,
        "the operation retains its exact actual geometry and mask allocation bytes");
    expectations.expect(producer.poll() == GpuPathCoveragePollResult::Failure &&
                            producer.hasUnretiredSubmission() && producer.coverageWidth() == 0,
                        "repeated failed polls retain the submission without publishing a mask");
    std::thread foreignPoll([&] {
        expectations.expect(producer.poll() == GpuPathCoveragePollResult::WrongThread &&
                                producer.hasUnretiredSubmission(),
                            "foreign polling cannot retire the quarantine");
    });
    foreignPoll.join();
    expectations.expect(producer.begin(parameters, *geometry.value(), 1ULL << 34ULL).code ==
                            GpuPathCoverageDiagnosticCode::DeviceUnavailable,
                        "admission is refused while the quarantine is occupied");
    setPathCoverageFaultForTest(PathCoverageFault::None);
    bool retired = false;
    const auto retireDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!retired && std::chrono::steady_clock::now() < retireDeadline) {
        expectations.expect(producer.poll() == GpuPathCoveragePollResult::Failure,
                            "owner retirement never publishes a failed mask");
        retired = !producer.hasUnretiredSubmission();
        if (!retired) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    expectations.expect(retired, "the original operation retires the quarantine via real polling");
    const auto allocatorAfter = pathCoverageAllocatorStats(device);
    expectations.expect(allocatorAfter.allocationCount == allocatorBefore.allocationCount &&
                            allocatorAfter.allocationBytes == allocatorBefore.allocationBytes,
                        "real VMA allocation count and bytes return to baseline after proof");
    expectations.expect(pathCoverageQuarantineAllocationBytes() == 0 &&
                            producer.poll() == GpuPathCoveragePollResult::Failure,
                        "exact retained allocations reclaim once and failure remains terminal");
    expectations.expect(!pathCoverageQuarantineOccupiedForTest(),
                        "the quarantine is free after retirement");
    expectations.expect(runCoverage(producer, *window.value(), PathFillRule::NonZero, false,
                                    *raster.value(), measured),
                        "the producer is reusable after quarantine retirement");

    // Cancellation fails publication promptly but cannot authorize releasing a pending dispatch.
    setPathCoverageFaultForTest(PathCoverageFault::ForceFenceTimeout);
    expectations.expect(producer.begin(parameters, *geometry.value(), 1ULL << 34ULL).code ==
                            GpuPathCoverageDiagnosticCode::None,
                        "cancelled timeout begin is accepted");
    producer.cancel();
    expectations.expect(producer.poll() == GpuPathCoveragePollResult::Failure &&
                            producer.diagnostic().code ==
                                GpuPathCoverageDiagnosticCode::Cancelled &&
                            producer.hasUnretiredSubmission(),
                        "cancelled timeout keeps exact native resources until proof");
    setPathCoverageFaultForTest(PathCoverageFault::None);
    const auto cancellationDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (producer.hasUnretiredSubmission() &&
           std::chrono::steady_clock::now() < cancellationDeadline) {
        static_cast<void>(producer.poll());
        std::this_thread::yield();
    }
    expectations.expect(!producer.hasUnretiredSubmission() && producer.coverageWidth() == 0 &&
                            producer.poll() == GpuPathCoveragePollResult::Failure,
                        "cancelled dispatch retires without mask publication");

    // A destroyed failed wrapper must retain its descriptor/pipeline resident together with the
    // quarantined buffers until the owner proves the real fence, then reclaim exactly once.
    {
        auto orphan = GpuPathCoverage::create(device);
        expectations.expect(orphan.hasValue(), "quarantine teardown producer creates");
        if (orphan) {
            setPathCoverageFaultForTest(PathCoverageFault::ForceFenceTimeout);
            static_cast<void>(orphan.coverage->begin(parameters, *geometry.value(), 1ULL << 34ULL));
            static_cast<void>(orphan.coverage->poll());
            const auto bytes = pathCoverageQuarantineAllocationBytes();
            const auto retiredBefore = pathCoverageResidentRetired();
            orphan.coverage.reset();
            drainPathCoverageResidentOrphansOnOwnerThread();
            expectations.expect(bytes > 0 && pathCoverageQuarantineAllocationBytes() == bytes &&
                                    pathCoverageResidentOrphaned() > 0 &&
                                    pathCoverageResidentRetired() == retiredBefore,
                                "quarantine teardown retains buffers and their pipeline resident");
            setPathCoverageFaultForTest(PathCoverageFault::None);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (pathCoverageQuarantineOccupied() &&
                   std::chrono::steady_clock::now() < deadline) {
                drainPathCoverageResidentOrphansOnOwnerThread();
                std::this_thread::yield();
            }
            drainPathCoverageResidentOrphansOnOwnerThread();
            expectations.expect(pathCoverageQuarantineAllocationBytes() == 0 &&
                                    pathCoverageResidentRetired() == retiredBefore + 1,
                                "quarantine teardown reclaims the exact resident once after proof");
        }
    }

    // Admission can retire an old quarantine and reuse the slot. The old wrapper's token must
    // never release the newly admitted job even though its owner and device match.
    {
        setPathCoverageFaultForTest(PathCoverageFault::ForceFenceTimeout);
        static_cast<void>(producer.begin(parameters, *geometry.value(), 1ULL << 34ULL));
        static_cast<void>(producer.poll());
        auto replacement = GpuPathCoverage::create(device);
        expectations.expect(replacement.hasValue(), "replacement coverage producer creates");
        setPathCoverageFaultForTest(PathCoverageFault::None);
        if (replacement) {
            GpuPathCoverageDiagnostic begun;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            do {
                begun = replacement.coverage->begin(parameters, *geometry.value(), 1ULL << 34ULL);
                std::this_thread::yield();
            } while (begun.code == GpuPathCoverageDiagnosticCode::DeviceUnavailable &&
                     std::chrono::steady_clock::now() < deadline);
            expectations.expect(begun.code == GpuPathCoverageDiagnosticCode::None &&
                                    replacement.coverage->hasUnretiredSubmission(),
                                "new admission adopts a genuine matching reservation");
            setPathCoverageFaultForTest(PathCoverageFault::ForceFenceTimeout);
            static_cast<void>(replacement.coverage->poll());
            const auto bytes = pathCoverageQuarantineAllocationBytes();
            static_cast<void>(producer.poll());
            expectations.expect(!producer.hasUnretiredSubmission() && bytes > 0 &&
                                    replacement.coverage->hasUnretiredSubmission() &&
                                    pathCoverageQuarantineAllocationBytes() == bytes,
                                "old token cannot retire another job on the same owner and device");
            setPathCoverageFaultForTest(PathCoverageFault::None);
            const auto retirementDeadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (replacement.coverage->hasUnretiredSubmission() &&
                   std::chrono::steady_clock::now() < retirementDeadline) {
                static_cast<void>(replacement.coverage->poll());
                std::this_thread::yield();
            }
            expectations.expect(!replacement.coverage->hasUnretiredSubmission(),
                                "new token retires its own exact submission");
        }
    }

    // Forced device loss: released without quarantine and latched on this instance.
    setPathCoverageFaultForTest(PathCoverageFault::ForceDeviceLost);
    expectations.expect(producer.begin(parameters, *geometry.value(), 1ULL << 34ULL).code ==
                            GpuPathCoverageDiagnosticCode::None,
                        "the device-loss begin is accepted");
    expectations.expect(pollToCompletion(producer) == GpuPathCoveragePollResult::Failure &&
                            producer.diagnostic().code == GpuPathCoverageDiagnosticCode::DeviceLost,
                        "a forced device loss is reported");
    expectations.expect(!pathCoverageQuarantineOccupiedForTest(),
                        "a device loss does not quarantine");
    setPathCoverageFaultForTest(PathCoverageFault::None);
    expectations.expect(producer.begin(parameters, *geometry.value(), 1ULL << 34ULL).code ==
                            GpuPathCoverageDiagnosticCode::DeviceLost,
                        "a lost instance refuses reuse");
    auto fresh = GpuPathCoverage::create(device);
    expectations.expect(fresh.hasValue(), "a fresh producer is created after device loss");
    if (fresh) {
        expectations.expect(runCoverage(*fresh.coverage, *window.value(), PathFillRule::NonZero,
                                        false, *raster.value(), measured),
                            "a fresh producer recovers after device loss");
    }
}

} // namespace bloom::render::gpu_path_coverage_test
