// GpuPathCoverage lifecycle, device-identity, bounded-reservation fault, and
// performance tests. The resident chain consumes the producer's device buffer
// directly; the fault tests force submit failure, fence timeout with quarantine
// and retirement, and device loss, each followed by reuse.

#include "gpu_path_coverage_fault.hpp"
#include "gpu_path_coverage_test_support.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace bloom::render::gpu_path_coverage_test {
namespace {

// One real device owner thread with its own device and one Ready producer. The producer is moved
// out so the test thread can orphan it, then the owner thread drains the resident pool.
struct OwnerThreadContext final {
    std::filesystem::path loaderPath;
    const PathRaster* raster = nullptr;
    const ImageWindow* window = nullptr;
    std::unique_ptr<GpuDevice> device;
    std::unique_ptr<GpuPathCoverage> producer;
    std::atomic<bool> ready{false};
    std::atomic<bool> drainNow{false};
    std::atomic<bool> drained{false};
    std::size_t orphanedAfter = 0;
    std::size_t inUseAfter = 0;
    std::uint64_t retiredAfter = 0;
    bool created = false;
    std::string error;
};

void runOwnerThread(OwnerThreadContext& context) {
    GpuDeviceCreationOptions options;
    options.loader_path = context.loaderPath;
    auto device = GpuDevice::create(options);
    if (!device) {
        context.error = "device: " + device.diagnostic.message;
        context.ready.store(true);
        return;
    }
    context.device = std::move(device.device);
    auto producer = GpuPathCoverage::create(*context.device);
    if (!producer) {
        context.error = "producer: " + producer.diagnostic.message;
        context.ready.store(true);
        return;
    }
    const auto geometry = context.raster->coverageGeometry(
        context.window->originX(), context.window->originY(), context.window->extent().width(),
        context.window->extent().height(), PathFillRule::NonZero, false);
    if (!geometry) {
        context.error = "geometry failed";
        context.ready.store(true);
        return;
    }
    const GpuPathCoverageParameters parameters{*context.window, *context.window,
                                               PixelAspectRatio::square()};
    bool ready = false;
    for (int attempt = 0; attempt < 2000 && !ready; ++attempt) {
        const auto began = producer.coverage->begin(parameters, *geometry.value(), 1ULL << 34ULL);
        if (began.code == GpuPathCoverageDiagnosticCode::Busy) {
            // Another device owner holds the single in-flight reservation; retry.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (began.code != GpuPathCoverageDiagnosticCode::None) {
            context.error = "begin: " + began.message;
            break;
        }
        GpuPathCoveragePollResult poll = GpuPathCoveragePollResult::Pending;
        while (poll == GpuPathCoveragePollResult::Pending) {
            poll = producer.coverage->poll();
        }
        if (poll != GpuPathCoveragePollResult::Ready) {
            context.error = "poll: " + producer.coverage->diagnostic().message;
            break;
        }
        ready = true;
    }
    if (!ready) {
        if (context.error.empty()) {
            context.error = "begin never left Busy";
        }
        context.ready.store(true);
        return;
    }
    context.producer = std::move(producer.coverage);
    context.created = true;
    context.ready.store(true);
    while (!context.drainNow.load()) {
        std::this_thread::yield();
    }
    path_coverage_detail::drainPathCoverageResidentOrphansOnOwnerThread();
    context.orphanedAfter = path_coverage_detail::pathCoverageResidentOrphaned();
    context.inUseAfter = path_coverage_detail::pathCoverageResidentInUse();
    context.retiredAfter = path_coverage_detail::pathCoverageResidentRetired();
    context.drained.store(true);
    // context.device is destroyed here, on its owner thread.
}

} // namespace

void testResidentConsumption(Expectations& expectations, GpuPathCoverage& producer, GpuSolid& solid,
                             GpuDevice& device) {
    const auto window = ImageWindow::create(-2, -2, 20, 14);
    expectations.expect(static_cast<bool>(window), "the resident chain window builds");
    if (!window) {
        return;
    }
    const auto& w = *window.value();
    const auto pixel =
        bloom::render::solidPixelFromStraightLinearRec709Scene(Color4d{0.2, 0.6, 0.9, 1.0});
    expectations.expect(static_cast<bool>(pixel), "the resident chain pixel builds");
    if (!pixel) {
        return;
    }
    auto raster = PathRaster::transformed(std::array{rectanglePath(18.0, 12.0)}, {},
                                          PathMatrix{1, 0, 0, 1, 0.3, 0.3}, 1, 1);
    expectations.expect(static_cast<bool>(raster), "the resident chain raster builds");
    if (!raster) {
        return;
    }
    const auto coverage = cpuCoverage(*raster.value(), w, PathFillRule::NonZero, false);
    const std::uint64_t before = GpuPathCoverage::nativeDispatchCount();
    std::vector<std::uint8_t> measured;
    expectations.expect(
        runCoverage(producer, w, PathFillRule::NonZero, false, *raster.value(), measured),
        "the resident coverage producer completes");
    expectations.expect(GpuPathCoverage::nativeDispatchCount() > before,
                        "a native coverage dispatch was counted");
    expectations.expect(producer.isBoundTo(device), "the producer is bound to its device");

    const float opacity = 0.6F;
    const GpuSolidParameters base{*pixel.value(), w, w, PixelAspectRatio::square()};
    const auto began = solid.beginCoveredResident(base, producer, opacity, 1ULL << 34ULL);
    expectations.expect(began.code == GpuSolidDiagnosticCode::None,
                        "the resident covered fill is accepted");
    if (began.code != GpuSolidDiagnosticCode::None) {
        return;
    }
    expectations.expect(pollSolidToCompletion(solid) == GpuSolidPollResult::Ready,
                        "the resident covered fill completes");
    const auto readback = solid.readback();
    expectations.expect(readback.hasValue(), "the resident covered fill reads back");
    if (!readback.hasValue()) {
        return;
    }
    // CPU oracle: coverageRow then coverageSolidRow plus the separate Float32 opacity.
    std::vector<Rgba32f> reference(
        static_cast<std::size_t>(w.extent().width()) * w.extent().height(), Rgba32f::transparent());
    const std::uint32_t width = w.extent().width();
    const std::uint32_t height = w.extent().height();
    for (std::uint32_t row = 0; row < height; ++row) {
        const auto offset = static_cast<std::size_t>(row) * width;
        const auto rowCoverage = std::span<const std::uint8_t>(coverage.data() + offset, width);
        const auto rowOutput = std::span<Rgba32f>(reference.data() + offset, width);
        expectations.expect(
            !bloom::render::coverageSolidRow(rowCoverage, *pixel.value(), rowOutput),
            "the CPU covered oracle runs");
        for (auto& value : rowOutput) {
            const auto faded =
                Rgba32f::fromPremultiplied(value.red() * opacity, value.green() * opacity,
                                           value.blue() * opacity, value.alpha() * opacity);
            value = *faded.value();
        }
    }
    expectations.expect(readback.pixels == reference,
                        "the resident coverage -> GpuSolid chain matches the CPU oracle");
}

void testGuards(Expectations& expectations, GpuPathCoverage& producer, GpuSolid& solid) {
    const auto window = ImageWindow::create(0, 0, 8, 4);
    expectations.expect(static_cast<bool>(window), "the guard window builds");
    if (!window) {
        return;
    }
    const auto& w = *window.value();
    const auto raster = PathRaster::create(rectanglePath(6.0, 3.0), {}, 1, 1);
    if (!raster) {
        expectations.expect(false, "the guard raster builds");
        return;
    }
    const auto geometry =
        raster.value()->coverageGeometry(w.originX(), w.originY(), w.extent().width(),
                                         w.extent().height(), PathFillRule::NonZero, false);
    expectations.expect(static_cast<bool>(geometry), "the guard geometry builds");
    if (!geometry) {
        return;
    }
    const GpuPathCoverageParameters parameters{w, w, PixelAspectRatio::square()};

    // Mismatched window geometry is refused.
    const auto mismatchWindow = ImageWindow::create(0, 0, 9, 4);
    expectations.expect(static_cast<bool>(mismatchWindow), "the mismatched window builds");
    if (mismatchWindow) {
        const auto mismatch = producer.begin(
            GpuPathCoverageParameters{*mismatchWindow.value(), w, PixelAspectRatio::square()},
            *geometry.value(), 1ULL << 34ULL);
        expectations.expect(mismatch.code == GpuPathCoverageDiagnosticCode::InvalidArgument,
                            "a mismatched data window is rejected");
    }

    // A budget below the retained mask is refused and leaves the producer reusable.
    const auto tight = producer.begin(parameters, *geometry.value(), 4);
    expectations.expect(tight.code == GpuPathCoverageDiagnosticCode::OverBudget,
                        "an under-budget coverage request is rejected");

    // Consuming a non-Ready producer is refused.
    const auto pixel =
        bloom::render::solidPixelFromStraightLinearRec709Scene(Color4d{0.5, 0.5, 0.5, 1.0});
    const auto notReady = solid.beginCoveredResident(
        GpuSolidParameters{*pixel.value(), w, w, PixelAspectRatio::square()}, producer, 0.5F,
        1ULL << 34ULL);
    expectations.expect(notReady.code == GpuSolidDiagnosticCode::InvalidArgument,
                        "a non-Ready resident coverage is rejected");

    // Cancel publishes nothing and the producer recovers for the next job.
    expectations.expect(producer.begin(parameters, *geometry.value(), 1ULL << 34ULL).code ==
                            GpuPathCoverageDiagnosticCode::None,
                        "the cancellable coverage begin is accepted");
    producer.cancel();
    expectations.expect(pollToCompletion(producer) == GpuPathCoveragePollResult::Failure &&
                            producer.diagnostic().code == GpuPathCoverageDiagnosticCode::Cancelled,
                        "a cancelled coverage job reports Cancelled");
    expectations.expect(producer.coverageWidth() == 0, "no coverage is published after cancel");

    // Reuse after cancel produces the exact mask.
    std::vector<std::uint8_t> measured;
    expectations.expect(
        runCoverage(producer, w, PathFillRule::NonZero, false, *raster.value(), measured),
        "the producer recovers after cancellation");
    expectations.expect(measured == cpuCoverage(*raster.value(), w, PathFillRule::NonZero, false),
                        "the recovered coverage is exact");

    // Wrong-thread poll fails closed.
    auto foreignPoll = GpuPathCoveragePollResult::Ready;
    std::thread worker([&producer, &foreignPoll]() { foreignPoll = producer.poll(); });
    worker.join();
    expectations.expect(foreignPoll == GpuPathCoveragePollResult::WrongThread,
                        "a poll from a joined non-owner thread is WrongThread");
}

// Device-identity and wrong-thread hardening for the resident consume path.
void testDeviceIdentity(Expectations& expectations, GpuPathCoverage& producer, GpuSolid& solid,
                        GpuDevice& device, const std::filesystem::path& loaderPath) {
    const auto window = ImageWindow::create(-2, -2, 8, 4);
    const auto raster = PathRaster::create(rectanglePath(6.0, 3.0), {}, 1, 1);
    expectations.expect(static_cast<bool>(window) && static_cast<bool>(raster),
                        "the identity window and raster build");
    if (!window || !raster) {
        return;
    }
    std::vector<std::uint8_t> measured;
    expectations.expect(producer.isBoundTo(device), "the identity producer is bound to its device");
    expectations.expect(runCoverage(producer, *window.value(), PathFillRule::NonZero, false,
                                    *raster.value(), measured),
                        "the identity coverage producer runs");
    const auto pixel =
        bloom::render::solidPixelFromStraightLinearRec709Scene(Color4d{0.4, 0.5, 0.6, 1.0});
    expectations.expect(static_cast<bool>(pixel), "the identity pixel builds");
    if (!pixel) {
        return;
    }
    const GpuSolidParameters base{*pixel.value(), *window.value(), *window.value(),
                                  PixelAspectRatio::square()};

    // Wrong-thread consume fails closed before reading the producer's state.
    auto foreign = GpuSolidDiagnosticCode::None;
    std::thread worker([&solid, &base, &producer, &foreign]() {
        foreign = solid.beginCoveredResident(base, producer, 0.5F, 1ULL << 34ULL).code;
    });
    worker.join();
    expectations.expect(foreign == GpuSolidDiagnosticCode::WrongThread,
                        "a wrong-thread resident consume is WrongThread");

    // A foreign device's resident coverage must be rejected by identity, not dimensions.
    GpuDeviceCreationOptions secondOptions;
    secondOptions.loader_path = loaderPath;
    auto second = GpuDevice::create(secondOptions);
    if (!second) {
        return;
    }
    auto secondProducer = GpuPathCoverage::create(*second.device);
    expectations.expect(secondProducer.hasValue(), "the second-device producer is created");
    if (!secondProducer) {
        return;
    }
    std::vector<std::uint8_t> secondMask;
    expectations.expect(runCoverage(*secondProducer.coverage, *window.value(),
                                    PathFillRule::NonZero, false, *raster.value(), secondMask),
                        "the second-device coverage runs");
    const auto foreignBegin =
        solid.beginCoveredResident(base, *secondProducer.coverage, 0.5F, 1ULL << 34ULL);
    expectations.expect(foreignBegin.code == GpuSolidDiagnosticCode::InvalidArgument,
                        "a foreign-device resident coverage is rejected by identity");
}

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
    expectations.expect(pathCoverageQuarantineOccupiedForTest(), "the quarantine is occupied");
    expectations.expect(producer.begin(parameters, *geometry.value(), 1ULL << 34ULL).code ==
                            GpuPathCoverageDiagnosticCode::DeviceUnavailable,
                        "admission is refused while the quarantine is occupied");
    setPathCoverageFaultForTest(PathCoverageFault::None);
    bool retired = false;
    const auto retireDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!retired && std::chrono::steady_clock::now() < retireDeadline) {
        retired = retirePathCoverageQuarantineForTest();
        if (!retired) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    expectations.expect(retired, "the owner retires the quarantine");
    expectations.expect(!pathCoverageQuarantineOccupiedForTest(),
                        "the quarantine is free after retirement");
    expectations.expect(runCoverage(producer, *window.value(), PathFillRule::NonZero, false,
                                    *raster.value(), measured),
                        "the producer is reusable after quarantine retirement");

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

// Injected small X workgroup limit: the flattened 2D grid must still plan and execute a
// capacity-valid geometry that a 1D grid would have refused.
void testTwoDimensionalPlan(Expectations& expectations, GpuPathCoverage& producer) {
    using namespace bloom::render::path_coverage_detail;
    const auto window = ImageWindow::create(0, 0, 80, 64);
    const auto raster = PathRaster::create(rectanglePath(70.0, 54.0), {}, 1, 1);
    expectations.expect(static_cast<bool>(window) && static_cast<bool>(raster),
                        "the 2D plan window and raster build");
    if (!window || !raster) {
        return;
    }
    const auto reference =
        cpuCoverage(*raster.value(), *window.value(), PathFillRule::NonZero, false);
    setPathCoverageMaxWorkGroupCountXForTest(3);
    setPathCoverageMaxWorkGroupCountYForTest(4);
    std::vector<std::uint8_t> measured;
    const bool ran = runCoverage(producer, *window.value(), PathFillRule::NonZero, false,
                                 *raster.value(), measured);
    setPathCoverageMaxWorkGroupCountXForTest(0);
    setPathCoverageMaxWorkGroupCountYForTest(0);
    expectations.expect(ran, "the 2D-flattened coverage plan runs under an injected small maxX");
    expectations.expect(measured == reference, "the 2D-flattened coverage is byte-exact");
}

// Capacity invariant: at most `capacity` producers own native resources (pipeline + resident mask)
// at once. Many idle producers allocate zero and hold no slot; the next begin beyond capacity is
// refused cleanly before any allocation; foreign destruction preserves the already-owned slots; the
// owner drain retires every orphaned resident (allocation-free) and returns the slots so admission
// recovers.
void testResidentPoolBound(Expectations& expectations, GpuDevice& device) {
    using namespace bloom::render::path_coverage_detail;
    const auto window = ImageWindow::create(0, 0, 8, 4);
    const auto raster = PathRaster::create(rectanglePath(6.0, 3.0), {}, 1, 1);
    expectations.expect(static_cast<bool>(window) && static_cast<bool>(raster),
                        "the resident-pool window and raster build");
    if (!window || !raster) {
        return;
    }
    const std::size_t capacity = pathCoverageResidentCapacity();
    expectations.expect(capacity > 0, "the resident pool has a positive bounded capacity");
    const std::size_t inUseBefore = pathCoverageResidentInUse();
    expectations.expect(inUseBefore <= capacity,
                        "the resident pool invariant holds before the test");
    const std::size_t available = capacity - inUseBefore;
    expectations.expect(available > 0, "the resident pool has at least one free slot");
    if (available == 0) {
        return;
    }

    // 32 pre-created idle producers allocate no native resources and hold no slot.
    std::vector<std::unique_ptr<GpuPathCoverage>> producers;
    for (int index = 0; index < 32; ++index) {
        auto created = GpuPathCoverage::create(device);
        if (!created) {
            break;
        }
        producers.push_back(std::move(created.coverage));
    }
    expectations.expect(producers.size() == 32, "32 idle producers are created");
    expectations.expect(pathCoverageResidentInUse() == inUseBefore,
                        "idle producers allocate no resident slot");

    const auto geometry = raster.value()->coverageGeometry(
        window.value()->originX(), window.value()->originY(), window.value()->extent().width(),
        window.value()->extent().height(), PathFillRule::NonZero, false);
    expectations.expect(static_cast<bool>(geometry), "the resident-pool geometry builds");
    if (!geometry) {
        return;
    }
    const GpuPathCoverageParameters parameters{*window.value(), *window.value(),
                                               PixelAspectRatio::square()};

    // Sequential Ready fills exactly the available slots; the next begin is refused before any
    // native allocation.
    std::size_t ready = 0;
    for (auto& producer : producers) {
        const auto began = producer->begin(parameters, *geometry.value(), 1ULL << 34ULL);
        if (began.code != GpuPathCoverageDiagnosticCode::None) {
            break;
        }
        if (pollToCompletion(*producer) != GpuPathCoveragePollResult::Ready) {
            break;
        }
        ++ready;
    }
    expectations.expect(ready == available, "exactly the available resident slots become Ready");
    expectations.expect(pathCoverageResidentInUse() == capacity, "the resident pool is full");

    const std::uint64_t refusalsBefore = pathCoverageResidentRefusals();
    auto extra = GpuPathCoverage::create(device);
    expectations.expect(extra.hasValue(),
                        "an idle producer is still creatable when the pool is full");
    if (extra) {
        const auto refused = extra.coverage->begin(parameters, *geometry.value(), 1ULL << 34ULL);
        expectations.expect(refused.code == GpuPathCoverageDiagnosticCode::DeviceUnavailable,
                            "a full resident pool refuses a begin cleanly");
        expectations.expect(pathCoverageResidentRefusals() == refusalsBefore + 1,
                            "the refusal is counted");
        expectations.expect(pathCoverageResidentInUse() == capacity,
                            "a refused begin allocates no resident slot");
    }

    // Foreign-destroy all 32 producers: the resident holders are orphaned in their own slots, the
    // idle ones own no native state.
    const std::uint64_t retiredBefore = pathCoverageResidentRetired();
    std::thread worker([&producers]() { producers.clear(); });
    worker.join();
    expectations.expect(pathCoverageResidentInUse() == capacity,
                        "foreign destruction preserves the resident pool bound");
    expectations.expect(pathCoverageResidentOrphaned() == available,
                        "every resident holder is orphaned in its own slot");

    // Owner drain retires the orphaned residents without allocating and returns their slots.
    drainPathCoverageResidentOrphansOnOwnerThread();
    expectations.expect(pathCoverageResidentOrphaned() == 0,
                        "the owner drain empties the orphaned set");
    expectations.expect(pathCoverageResidentInUse() == inUseBefore,
                        "the drain returns every orphaned resident slot");
    expectations.expect(pathCoverageResidentRetired() == retiredBefore + available,
                        "every orphaned resident was retired on the owner thread");

    // Admission recovers.
    auto recovered = GpuPathCoverage::create(device);
    expectations.expect(recovered.hasValue(), "a producer is creatable after recovery");
    if (recovered) {
        std::vector<std::uint8_t> measured;
        expectations.expect(runCoverage(*recovered.coverage, *window.value(), PathFillRule::NonZero,
                                        false, *raster.value(), measured),
                            "a begin works after recovery");
    }
}

// Two real device owner threads. B's owner drain must skip A's orphan (wrong owner thread) and
// leave it untouched; A's owner drain then frees it. This is the exact-owner guard that prevents
// one device thread from querying a fence or tearing down another device's Vulkan resources.
void testResidentPoolOwnerIsolation(Expectations& expectations,
                                    const std::filesystem::path& loaderPath) {
    using namespace bloom::render::path_coverage_detail;
    const auto window = ImageWindow::create(0, 0, 8, 4);
    const auto raster = PathRaster::create(rectanglePath(6.0, 3.0), {}, 1, 1);
    expectations.expect(static_cast<bool>(window) && static_cast<bool>(raster),
                        "the owner-isolation window and raster build");
    if (!window || !raster) {
        return;
    }
    const std::size_t inUseBefore = pathCoverageResidentInUse();
    const std::size_t orphanedBefore = pathCoverageResidentOrphaned();
    const std::uint64_t retiredBefore = pathCoverageResidentRetired();

    OwnerThreadContext a;
    a.loaderPath = loaderPath;
    a.raster = raster.value();
    a.window = window.value();
    OwnerThreadContext b;
    b.loaderPath = loaderPath;
    b.raster = raster.value();
    b.window = window.value();
    std::thread threadA([&a] { runOwnerThread(a); });
    std::thread threadB([&b] { runOwnerThread(b); });
    while (!a.ready.load() || !b.ready.load()) {
        std::this_thread::yield();
    }
    expectations.expect(a.created && b.created, "both owner-thread producers become Ready");
    if (!a.created || !b.created) {
        std::cerr << "owner-thread A: " << a.error << " ; B: " << b.error << '\n';
    }
    if (a.created && b.created) {
        // Orphan both producers from this foreign thread.
        a.producer.reset();
        b.producer.reset();
        expectations.expect(pathCoverageResidentOrphaned() == orphanedBefore + 2,
                            "both owner-thread residents are orphaned");
        expectations.expect(pathCoverageResidentInUse() == inUseBefore + 2,
                            "both owner-thread residents hold slots");

        // B drains first: it owns B's orphan but not A's, so A's must be left untouched.
        b.drainNow.store(true);
        while (!b.drained.load()) {
            std::this_thread::yield();
        }
        expectations.expect(b.orphanedAfter == orphanedBefore + 1,
                            "B's drain leaves A's orphan untouched");
        expectations.expect(pathCoverageResidentOrphaned() == orphanedBefore + 1,
                            "a wrong-owner drain does not touch another owner's orphan");
        expectations.expect(pathCoverageResidentRetired() == retiredBefore + 1,
                            "B retires exactly its own orphan");

        // A drains its own orphan.
        a.drainNow.store(true);
        while (!a.drained.load()) {
            std::this_thread::yield();
        }
        expectations.expect(pathCoverageResidentOrphaned() == orphanedBefore,
                            "A's drain frees A's orphan");
        expectations.expect(pathCoverageResidentInUse() == inUseBefore,
                            "all owner-thread slots are returned");
        expectations.expect(pathCoverageResidentRetired() == retiredBefore + 2,
                            "both owners retire their own orphans");
    } else {
        a.drainNow.store(true);
        b.drainNow.store(true);
    }
    threadA.join();
    threadB.join();
}

void testPerformance(Expectations& expectations, GpuPathCoverage& producer) {
    struct Size final {
        std::uint32_t width;
        std::uint32_t height;
    };
    const std::array<Size, 3> sizes{{{1280, 720}, {1920, 1080}, {3840, 2160}}};
    auto raster = PathRaster::create(starPath(900.0, 500.0, 12, 0.45), {}, 1, 1);
    expectations.expect(static_cast<bool>(raster), "the performance raster builds");
    if (!raster) {
        return;
    }
    std::cout << "path coverage performance (ms; CPU coverageRow vs GPU geometry + dispatch + "
                 "readback)\n";
    for (const auto size : sizes) {
        const auto window = ImageWindow::create(0, 0, size.width, size.height);
        if (!window) {
            expectations.expect(false, "the performance window builds");
            continue;
        }
        const auto& w = *window.value();
        std::vector<std::uint8_t> cpuBytes(static_cast<std::size_t>(size.width) * size.height, 0);
        const auto cpuRun = [&]() {
            for (std::uint32_t row = 0; row < size.height; ++row) {
                const auto offset = static_cast<std::size_t>(row) * size.width;
                (void)raster.value()->coverageRow(
                    w.originX(), w.originY() + row,
                    std::span<std::uint8_t>(cpuBytes.data() + offset, size.width),
                    PathFillRule::NonZero, false);
            }
        };
        const auto gpuRun = [&]() {
            std::vector<std::uint8_t> measured;
            if (!runCoverage(producer, w, PathFillRule::NonZero, false, *raster.value(),
                             measured)) {
                expectations.expect(false, "the performance coverage job runs");
            }
        };
        for (int warmup = 0; warmup < 3; ++warmup) {
            cpuRun();
            gpuRun();
        }
        std::vector<double> cpuSamples;
        std::vector<double> gpuSamples;
        for (int sample = 0; sample < 7; ++sample) {
            const auto cpuStart = std::chrono::steady_clock::now();
            cpuRun();
            const auto cpuStop = std::chrono::steady_clock::now();
            const auto gpuStart = std::chrono::steady_clock::now();
            gpuRun();
            const auto gpuStop = std::chrono::steady_clock::now();
            cpuSamples.push_back(
                std::chrono::duration<double, std::milli>(cpuStop - cpuStart).count());
            gpuSamples.push_back(
                std::chrono::duration<double, std::milli>(gpuStop - gpuStart).count());
        }
        std::sort(cpuSamples.begin(), cpuSamples.end());
        std::sort(gpuSamples.begin(), gpuSamples.end());
        std::cout << "  " << size.width << 'x' << size.height << ": cpu " << cpuSamples[3]
                  << " ms, gpu " << gpuSamples[3] << " ms\n";
    }
}

} // namespace bloom::render::gpu_path_coverage_test
