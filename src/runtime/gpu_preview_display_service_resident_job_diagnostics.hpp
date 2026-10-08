#pragma once

// Owner-thread resident failure diagnostics. Included only by resident jobs.
namespace bloom::runtime::detail {
namespace {

[[nodiscard]] TaskDiagnostic residentJobDiagnostic(std::string code, std::string summary,
                                                   std::string detail = {}) {
    return {.code = std::move(code),
            .severity = DiagnosticSeverity::Error,
            .summary = std::move(summary),
            .detail = std::move(detail),
            .suggestedAction = "Review the GPU preview display service resident diagnostics."};
}

// Publish the concrete refusal reason for a resident-stage failure. Without this the service
// reports an empty residentDetail and the actual GpuSceneExecutor/native diagnostic is lost.
void publishResidentJobDetail(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                              std::string detail) {
    std::lock_guard lock(core->stateMutex);
    core->publishedResidentDetail = std::move(detail);
}

// The executor's own diagnostic (code + message) plus the actual peak live bytes it charged
// against the per-request allowance: the required-vs-admitted evidence a refusal lacks otherwise.
[[nodiscard]] std::string executorFailureDetail(const GpuSceneExecutor& executor,
                                                const std::uint64_t admittedBytes,
                                                std::string_view prefix,
                                                const GpuSceneExecutorPollResult poll) {
    const auto& diagnostic = executor.diagnostic();
    const auto counters = executor.counters();
    std::string detail(prefix);
    if (!diagnostic.message.empty()) {
        detail += ": ";
        detail += diagnostic.message;
    }
    detail += " [poll=";
    detail += std::to_string(static_cast<unsigned>(poll));
    detail += ", code=";
    detail += std::to_string(static_cast<unsigned>(diagnostic.code));
    detail += ", admittedBytes=";
    detail += std::to_string(admittedBytes);
    detail += ", peakLiveBytes=";
    detail += std::to_string(counters.peakLiveImageBytes);
    detail += ", liveBytes=";
    detail += std::to_string(counters.currentLiveImageBytes);
    detail += ']';
    return detail;
}

[[nodiscard]] bool residentDisplayCancelled(const PreviewDisplayServiceCore& core,
                                            const PreviewDisplayStageRecord& stage) {
    return (stage.gpuStage != nullptr && stage.gpuStage->hasGeneralDisplayProgram())
               ? (core.residentGeneralDisplay != nullptr &&
                  core.residentGeneralDisplay->executor().diagnostic().code ==
                      GpuOcioExecutorDiagnosticCode::Cancelled)
               : (core.residentDisplay != nullptr &&
                  core.residentDisplay->diagnostic().code ==
                      render::GpuResidentDisplayDiagnosticCode::Cancelled);
}

[[nodiscard]] std::string
residentDisplayFailureDetail(const PreviewDisplayServiceCore& core,
                             const PreviewDisplayStageRecord& stage,
                             const render::GpuResidentDisplayPollResult poll) {
    std::string detail =
        "resident display failed [poll=" + std::to_string(static_cast<unsigned>(poll)) + "]";
    if (stage.gpuStage != nullptr && stage.gpuStage->hasGeneralDisplayProgram() &&
        core.residentGeneralDisplay != nullptr) {
        const auto& diagnostic = core.residentGeneralDisplay->executor().diagnostic();
        detail += ": " + diagnostic.message +
                  " [code=" + std::to_string(static_cast<unsigned>(diagnostic.code)) + ']';
    } else if (core.residentDisplay != nullptr) {
        const auto& diagnostic = core.residentDisplay->diagnostic();
        detail += ": " + diagnostic.message +
                  " [code=" + std::to_string(static_cast<unsigned>(diagnostic.code)) + ']';
    }
    return detail;
}

void publishResidentLossDetail(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                               const PreviewDisplayStageRecord& stage) {
    const auto detail = core->residentExecutor && core->residentExecutor->deviceLost()
                            ? executorFailureDetail(*core->residentExecutor, stage.gpuByteAllowance,
                                                    "resident scene executor device lost",
                                                    GpuSceneExecutorPollResult::Failure)
                            : residentDisplayFailureDetail(
                                  *core, stage, render::GpuResidentDisplayPollResult::Failure);
    publishResidentJobDetail(core, "resident route device lost: " + detail);
}

} // namespace
} // namespace bloom::runtime::detail
