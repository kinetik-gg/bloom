#pragma once

// Private cohesive support helpers for the GPU process-frame evaluator: closed diagnostic mapping,
// the throw-safe progress bridge, counter snapshotting, and the GPU content-semantics key. Kept in
// their own private header so gpu_process_frame.cpp stays within its owning-file size budget; this
// header is included by exactly that one translation unit and its functions have internal linkage.

#include <bloom/runtime/gpu_process_frame.hpp>
#include <bloom/runtime/gpu_scene_executor.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>

#include <string>
#include <utility>
#include <variant>

namespace bloom::runtime {
namespace {

[[nodiscard]] GpuProcessFrameDiagnosticCode
mapExecutorDiagnostic(const GpuSceneExecutorDiagnosticCode code) {
    switch (code) {
    case GpuSceneExecutorDiagnosticCode::OverBudget:
        return GpuProcessFrameDiagnosticCode::OverBudget;
    case GpuSceneExecutorDiagnosticCode::Cancelled:
        return GpuProcessFrameDiagnosticCode::Cancelled;
    case GpuSceneExecutorDiagnosticCode::DeviceLost:
        return GpuProcessFrameDiagnosticCode::DeviceLost;
    case GpuSceneExecutorDiagnosticCode::DeviceUnavailable:
        return GpuProcessFrameDiagnosticCode::DeviceUnavailable;
    default:
        return GpuProcessFrameDiagnosticCode::InternalInvariant;
    }
}

// A progress callback is caller-supplied and may throw; it must never unwind out of the calling
// CPU worker, the owner worker, or a scheduler task and take the process down.
inline void safeProgress(const EvaluationProgressCallback& progress,
                         const EvaluationProgress& event) noexcept {
    if (!progress) {
        return;
    }
    try {
        progress(event);
    } catch (...) {
    }
}

[[nodiscard]] GpuProcessFrameCounters snapshotCounters(const GpuSceneExecutor& executor) noexcept {
    const auto counters = executor.counters();
    GpuProcessFrameCounters result;
    result.sceneBegins = counters.sceneBegins;
    result.scenesCompleted = counters.scenesCompleted;
    result.outputCacheHits = counters.outputCacheHits;
    result.outputCacheMisses = counters.outputCacheMisses;
    result.solidDispatches = counters.solidDispatches;
    result.coveredSolidDispatches = counters.coveredSolidDispatches;
    result.translationDispatches = counters.translationDispatches;
    result.sourceOverDispatches = counters.sourceOverDispatches;
    result.uploads = counters.uploads;
    result.nativeDispatches = counters.dispatches;
    return result;
}

[[nodiscard]] inline std::string outputSemanticKey(const PreparedGpuScene& scene) {
    const auto index = scene.outputCommand();
    const auto& commands = scene.commands();
    if (index == kInvalidGpuSceneCommand || static_cast<std::size_t>(index) >= commands.size()) {
        return {};
    }
    return std::visit([](const auto& command) -> const std::string& { return command.semanticKey; },
                      commands[index]);
}

[[nodiscard]] inline GpuProcessFrameOutcome failure(const GpuProcessFrameStatus status,
                                                    const GpuProcessFrameDiagnosticCode code,
                                                    std::string message) {
    GpuProcessFrameOutcome outcome;
    outcome.status = status;
    outcome.diagnostic = {code, std::move(message)};
    return outcome;
}

} // namespace
} // namespace bloom::runtime
