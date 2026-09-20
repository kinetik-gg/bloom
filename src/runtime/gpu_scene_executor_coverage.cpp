#include "gpu_scene_executor_private.hpp"

#include <bloom/render/gpu_path_coverage.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace bloom::runtime {
namespace {

using gpu_scene_executor_detail::diagnosticFromSolid;
using gpu_scene_executor_detail::makeDiagnostic;
using gpu_scene_executor_detail::NativePoll;

[[nodiscard]] NativePoll mapPathCoverage(const render::GpuPathCoveragePollResult result) noexcept {
    switch (result) {
    case render::GpuPathCoveragePollResult::Pending:
        return NativePoll::Pending;
    case render::GpuPathCoveragePollResult::Ready:
        return NativePoll::Ready;
    case render::GpuPathCoveragePollResult::WrongThread:
        return NativePoll::WrongThread;
    case render::GpuPathCoveragePollResult::Failure:
        break;
    }
    return NativePoll::Failure;
}

[[nodiscard]] GpuSceneExecutorDiagnostic
diagnosticFromPathCoverage(const render::GpuPathCoverageDiagnostic& diagnostic) {
    switch (diagnostic.code) {
    case render::GpuPathCoverageDiagnosticCode::OverBudget:
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::OverBudget, diagnostic.message);
    case render::GpuPathCoverageDiagnosticCode::DeviceLost:
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::DeviceLost, diagnostic.message);
    case render::GpuPathCoverageDiagnosticCode::WrongThread:
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::WrongThread, diagnostic.message);
    case render::GpuPathCoverageDiagnosticCode::DeviceUnavailable:
    case render::GpuPathCoverageDiagnosticCode::AllocationFailed:
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::DeviceUnavailable,
                              diagnostic.message);
    case render::GpuPathCoverageDiagnosticCode::Unsupported:
    case render::GpuPathCoverageDiagnosticCode::ShaderRejected:
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::Unsupported, diagnostic.message);
    default:
        break;
    }
    return makeDiagnostic(GpuSceneExecutorDiagnosticCode::DispatchRefused, diagnostic.message);
}

[[nodiscard]] bool
pathCoverageDeviceLost(const render::GpuPathCoverageDiagnostic& diagnostic) noexcept {
    return diagnostic.code == render::GpuPathCoverageDiagnosticCode::DeviceLost;
}

[[nodiscard]] bool
pathCoverageCancelled(const render::GpuPathCoverageDiagnostic& diagnostic) noexcept {
    return diagnostic.code == render::GpuPathCoverageDiagnosticCode::Cancelled;
}

} // namespace

// Lazily creates the retained native vector-coverage producer on the device owner thread. A create
// failure fails the covered step closed (the caller takes the CPU path) rather than fabricating a
// host mask: there is no CPU coverage-mask opt-out.
GpuSceneExecutorDiagnostic GpuSceneExecutor::Impl::ensurePathCoverage() {
    if (pathCoverage != nullptr) {
        return {};
    }
    if (device == nullptr) {
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::DeviceUnavailable,
                              "the vector coverage producer needs a bound device");
    }
    const render::GpuPathCoverageBudgets coverageBudgets{64ULL * 1024ULL * 1024ULL,
                                                         budgets.maxCoverageBytes};
    auto created = render::GpuPathCoverage::create(*device, coverageBudgets);
    if (!created) {
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::Unsupported,
                              created.diagnostic.message);
    }
    pathCoverage = std::move(created.coverage);
    return {};
}

// Dispatches one covered step. A PathRaster vector source begins the native GpuPathCoverage
// producer (PathCoverage); the integer-grid FreeType text leaf begins the ordinary host-bitmap
// covered fill (Solid). The windowing/pixel/opacity fields are shared.
GpuSceneExecutorDiagnostic
GpuSceneExecutor::Impl::startCoveredStep(const GpuSceneExecutorStep& step) {
    const std::uint64_t remaining = remainingBudget();
    const render::GpuSolidParameters parameters{step.solidPixel, *step.solidDataWindow,
                                                *step.solidDisplayWindow, step.solidPixelAspect};
    if (step.coverageGeometry != nullptr) {
        if (const auto error = ensurePathCoverage();
            error.code != GpuSceneExecutorDiagnosticCode::None) {
            return error;
        }
        const render::GpuPathCoverageParameters coverageParameters{
            *step.solidDataWindow, *step.solidDisplayWindow, step.solidPixelAspect};
        const auto native =
            pathCoverage->begin(coverageParameters, *step.coverageGeometry, remaining);
        if (native.code != render::GpuPathCoverageDiagnosticCode::None) {
            // A refused begin has no unretired submission, so the step fails without a drain and
            // the caller takes the CPU path.
            return diagnosticFromPathCoverage(native);
        }
        ++counters.coverageDispatches;
        nativeKind = NativeKind::PathCoverage;
        return {};
    }
    if (step.hostCoverage == nullptr) {
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::InternalInvariant,
                              "a covered step has neither geometry nor a host bitmap");
    }
    const auto native =
        solid->beginCovered(parameters, *step.hostCoverage, step.coveredOpacity, remaining);
    if (native.code != render::GpuSolidDiagnosticCode::None) {
        return diagnosticFromSolid(native);
    }
    ++counters.coveredSolidDispatches;
    nativeKind = NativeKind::Solid;
    return {};
}

// Advances the producer. On Ready it starts the resident covered fill from the mask; the producer
// is retained (the executor member) until that consuming fill is proved retired.
GpuSceneExecutorPollResult GpuSceneExecutor::Impl::pollPathCoverage() {
    if (pathCoverage == nullptr) {
        fail(GpuSceneExecutorDiagnosticCode::InternalInvariant,
             "the vector coverage producer disappeared while in flight", false);
        return GpuSceneExecutorPollResult::Failure;
    }
    const NativePoll status = mapPathCoverage(pathCoverage->poll());
    if (status == NativePoll::WrongThread) {
        fail(GpuSceneExecutorDiagnosticCode::WrongThread,
             "the vector coverage producer rejected the calling thread", true);
        return GpuSceneExecutorPollResult::Failure;
    }
    if (status == NativePoll::Pending) {
        const auto deadline = std::chrono::milliseconds(budgets.jobDeadlineMilliseconds);
        if (!timeoutRequested && std::chrono::steady_clock::now() - nativeStart >= deadline) {
            timeoutRequested = true;
            ++counters.nativeJobTimeouts;
            pathCoverage->cancel();
            fail(GpuSceneExecutorDiagnosticCode::NativeTimeout,
                 "the vector coverage job exceeded its deadline; the submission is retained until "
                 "it retires",
                 true);
            return GpuSceneExecutorPollResult::Failure;
        }
        return GpuSceneExecutorPollResult::Pending;
    }
    if (status == NativePoll::Failure) {
        const auto& coverageDiagnostic = pathCoverage->diagnostic();
        if (pathCoverageDeviceLost(coverageDiagnostic)) {
            deviceLost = true;
            nativeInFlight = false;
            nativeKind = NativeKind::None;
            ++counters.nativeJobFailures;
            fail(GpuSceneExecutorDiagnosticCode::DeviceLost,
                 "the device generation was lost while polling the coverage producer", false);
            return GpuSceneExecutorPollResult::Failure;
        }
        if (!pathCoverage->hasUnretiredSubmission()) {
            nativeInFlight = false;
            nativeKind = NativeKind::None;
            if (pathCoverageCancelled(coverageDiagnostic)) {
                ++counters.nativeJobCancellations;
                fail(GpuSceneExecutorDiagnosticCode::Cancelled,
                     "the vector coverage job was cancelled after a proven retirement", false);
                return GpuSceneExecutorPollResult::Failure;
            }
            ++counters.nativeJobFailures;
            fail(GpuSceneExecutorDiagnosticCode::DispatchRefused,
                 "the vector coverage job failed after a proven retirement", false);
            return GpuSceneExecutorPollResult::Failure;
        }
        ++counters.nativeJobFailures;
        fail(GpuSceneExecutorDiagnosticCode::NativeUnproven,
             "a vector coverage failure did not prove fence retirement; owner drain is required",
             true);
        return GpuSceneExecutorPollResult::Failure;
    }

    // Ready: the producer's mask is resident. A cancellation discards it without a fill.
    if (cancelRequested) {
        nativeInFlight = false;
        nativeKind = NativeKind::None;
        ++counters.nativeJobCancellations;
        fail(GpuSceneExecutorDiagnosticCode::Cancelled,
             "the scene execution was cancelled; no image was published", false);
        return GpuSceneExecutorPollResult::Failure;
    }
    if (cursor >= steps.size()) {
        fail(GpuSceneExecutorDiagnosticCode::InternalInvariant,
             "the vector coverage step disappeared before its fill", false);
        return GpuSceneExecutorPollResult::Failure;
    }
    const GpuSceneExecutorStep& step = steps[cursor];
    const render::GpuSolidParameters parameters{step.solidPixel, *step.solidDataWindow,
                                                *step.solidDisplayWindow, step.solidPixelAspect};
    const auto native = solid->beginCoveredResident(parameters, *pathCoverage, step.coveredOpacity,
                                                    remainingBudget());
    if (native.code != render::GpuSolidDiagnosticCode::None) {
        fail(diagnosticFromSolid(native).code, native.message, false);
        return GpuSceneExecutorPollResult::Failure;
    }
    ++counters.coveredSolidDispatches;
    nativeKind = NativeKind::Solid;
    nativeStart = std::chrono::steady_clock::now();
    timeoutRequested = false;
    cancelIssued = false;
    return GpuSceneExecutorPollResult::Pending;
}

} // namespace bloom::runtime
