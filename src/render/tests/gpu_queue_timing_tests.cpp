#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_queue_timing.hpp>
#include <bloom/render/gpu_solid.hpp>

#ifdef BLOOM_QUEUE_TIMING_NATIVE
#include "gpu_queue_timing_private.hpp"
#endif

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace {
using namespace bloom::render;
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
GpuQueueTimingSample retired(GpuQueueTiming& timer) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    auto result = timer.poll();
    while (timer.hasUnretiredSubmission() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
        result = timer.poll();
    }
    require(!timer.hasUnretiredSubmission(), "bounded timestamp retirement completes");
    return result;
}
#ifdef BLOOM_QUEUE_TIMING_NATIVE
void negativeNativeContracts(GpuDevice& device) {
    const auto control = GpuRendererAccess::state(device);
    const bool calibrated = control->calibratedTimestampsEnabled;
    const auto attempts = control->nativeQueueTimingBackingAttempts;
    control->calibratedTimestampsEnabled = false;
    auto unsupported = GpuQueueTiming::create(device);
    control->calibratedTimestampsEnabled = calibrated;
    require(
        unsupported && unsupported->begin() == GpuQueueTimingStatus::Unsupported &&
            control->nativeQueueTimingBackingAttempts == attempts &&
            !control->queueTimingOccupied.load(),
        "disabled cross-submit monotonicity capability refuses before any native timing backing");
    unsupported.reset();
    if (!calibrated)
        return;
    auto timer = GpuQueueTiming::create(device);
    require(timer && timer->begin() == GpuQueueTimingStatus::Active,
            "unknown-fence test starts actual timestamp command submission");
    timer->end();
    auto state = control->queueTiming.lock();
    if (!state)
        throw std::runtime_error("exact native timing owner exists");
    state->pollFaultForTests = queue_timing_detail::PollFault::Unknown;
    require(timer->poll().status == GpuQueueTimingStatus::Failed &&
                timer->sample().nanoseconds == 0 && timer->hasUnretiredSubmission(),
            "unknown timestamp fence never grants a result or retires actual query backing");
    auto second = GpuQueueTiming::create(device);
    require(second && second->begin() == GpuQueueTimingStatus::Busy,
            "unknown native owner keeps the only timing slot occupied");
    second.reset();
    auto* const exactOwner = state.get();
    state.reset();
    std::thread release([owned = std::move(timer)]() mutable { owned.reset(); });
    release.join();
    require(control->retiredQueueTiming.load() == exactOwner && control->queueTimingOccupied.load(),
            "foreign final release retains the exact unknown native owner allocation-free");
    auto blocked = GpuQueueTiming::create(device);
    require(blocked && blocked->sample().status == GpuQueueTimingStatus::Busy &&
                control->retiredQueueTiming.load() == exactOwner &&
                control->nativeQueueTimingBackingAttempts == attempts + 1,
            "repeated unknown owner drain cannot allocate an additional timing pool");
    exactOwner->pollFaultForTests = queue_timing_detail::PollFault::None;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        blocked = GpuQueueTiming::create(device);
        require(blocked != nullptr, "exact timing fence recovery remains bounded");
        if (blocked->sample().status == GpuQueueTimingStatus::Busy)
            std::this_thread::yield();
    } while (blocked->sample().status == GpuQueueTimingStatus::Busy &&
             std::chrono::steady_clock::now() < deadline);
    require(blocked->begin() == GpuQueueTimingStatus::Active,
            "subsequent real fence proof frees and recovers the bounded native timing owner");
    blocked->cancel();
    require(retired(*blocked).status == GpuQueueTimingStatus::Cancelled,
            "recovered unknown timing owner retires normally");
}
#endif
bool run(GpuDevice& device) {
#ifdef BLOOM_QUEUE_TIMING_NATIVE
    negativeNativeContracts(device);
#endif
    auto timer = GpuQueueTiming::create(device);
    auto second = GpuQueueTiming::create(device);
    require(timer && second, "optional timing facades create");
    if (timer->sample().status == GpuQueueTimingStatus::Unsupported) {
        require(timer->begin() == GpuQueueTimingStatus::Unsupported &&
                    !timer->hasUnretiredSubmission() && timer->sample().nanoseconds == 0,
                "unsupported device reports no fabricated measurement or submission");
        std::cout << "Unsupported: compute queue exposes no timestamp capability\n";
        return false;
    }
    auto producer = GpuSolid::create(device);
    require(static_cast<bool>(producer), "actual production solid pipeline creates");
    const auto window = ImageWindow::create(-3, 5, 257, 19);
    if (!window)
        throw std::runtime_error("production test extent creates");
    const GpuSolidParameters params{Rgba32f::transparent(), *window.value(), *window.value(),
                                    bloom::core::PixelAspectRatio::square()};
    for (int cycle = 0; cycle < 10; ++cycle) {
        require(timer->begin() == GpuQueueTimingStatus::Active,
                "one serial timing owner starts and is reusable");
        require(second->begin() == GpuQueueTimingStatus::Busy,
                "overlapping facade cannot allocate or reset active query owner");
        require(timer->poll().status == GpuQueueTimingStatus::Active &&
                    timer->hasUnretiredSubmission(),
                "opening marker completion never retires an unclosed production span");
        require(producer.solid->begin(params, 16ULL * 1024 * 1024).code ==
                    GpuSolidDiagnosticCode::None,
                "actual native production submit is accepted");
        auto solidStatus = GpuSolidPollResult::Pending;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (solidStatus == GpuSolidPollResult::Pending &&
               std::chrono::steady_clock::now() < deadline)
            solidStatus = producer.solid->poll();
        require(solidStatus == GpuSolidPollResult::Ready,
                "pixels publish before optional timestamp end is submitted");
        timer->end();
        const auto measured = retired(*timer);
        require(measured.status == GpuQueueTimingStatus::Available && measured.nanoseconds > 0 &&
                    measured.timestampValidBits != 0 && measured.timestampPeriodNanoseconds > 0,
                "two real available query values produce genuine device-clock nanoseconds");
        require(producer.solid->takeImage().isValid(), "native output remains usable");
        require(second->begin() == GpuQueueTimingStatus::Active,
                "capacity-one owner transfers to another facade after actual fence proof");
        second->cancel();
        const auto cancelled = retired(*second);
        require(cancelled.status == GpuQueueTimingStatus::Cancelled && cancelled.nanoseconds == 0,
                "cancellation retires query commands without publishing a duration");
    }
    require(timer->begin() == GpuQueueTimingStatus::Active, "result handoff span starts");
    timer->end();
    const auto handoffDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    auto acquired = second->begin();
    while (acquired == GpuQueueTimingStatus::Busy &&
           std::chrono::steady_clock::now() < handoffDeadline) {
        std::this_thread::yield();
        acquired = second->begin();
    }
    require(acquired == GpuQueueTimingStatus::Active &&
                timer->poll().status == GpuQueueTimingStatus::Available &&
                timer->sample().nanoseconds > 0,
            "next facade may consume completed fence and reuse pool without losing prior result");
    second->cancel();
    static_cast<void>(retired(*second));
    GpuQueueTimingStatus foreign = GpuQueueTimingStatus::Idle;
    std::thread wrong([&] { foreign = timer->poll().status; });
    wrong.join();
    require(foreign == GpuQueueTimingStatus::WrongThread,
            "foreign diagnostic polling cannot touch Vulkan");
    second.reset();
    require(timer->begin() == GpuQueueTimingStatus::Active, "foreign retirement span starts");
    std::thread release([owned = std::move(timer)]() mutable { owned.reset(); });
    release.join();
    // A final foreign release retains the same native owner and generation. Owner-thread create
    // closes/discards it and polls its exact fence; repeated creates cannot grow query owners.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        timer = GpuQueueTiming::create(device);
        require(timer != nullptr, "foreign-retired owner drain remains bounded");
        if (timer->sample().status == GpuQueueTimingStatus::Busy)
            std::this_thread::yield();
    } while (timer->sample().status == GpuQueueTimingStatus::Busy &&
             std::chrono::steady_clock::now() < deadline);
    require(timer->begin() == GpuQueueTimingStatus::Active,
            "exact owner fence retirement recovers the single timing slot");
    timer->cancel();
    require(retired(*timer).status == GpuQueueTimingStatus::Cancelled,
            "recovered owner retires normally");
    return true;
}
} // namespace
int main(int argc, char** argv) {
    try {
        bool requireDevice = false;
        for (int index = 1; index < argc; ++index) {
            if (std::string_view(argv[index]) != "--require-device")
                return 2;
            requireDevice = true;
        }
        auto device = GpuDevice::create();
        if (!device) {
            std::cout << "Unsupported: " << device.diagnostic.message << '\n';
            return requireDevice ? 1 : 77;
        }
        if (!run(*device.device))
            return requireDevice ? 1 : 77;
        std::cout << "PASS: optional real queue timestamps, capacity-one reuse/cancel/retirement\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
