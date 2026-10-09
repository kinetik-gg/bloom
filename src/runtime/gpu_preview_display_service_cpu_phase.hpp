#pragma once

#include <bloom/runtime/gpu_preview_display_service.hpp>
#include <bloom/runtime/gpu_processing_phase_policy.hpp>

#include <limits>

namespace bloom::runtime::detail {

// Cheap immutable geometry only. No compiler, producer, media I/O, or rasterizer runs before the
// shared host-only phase grant. CPU process+display overlap deliberately uses the FloatFinal
// envelope; its source allowance becomes the existing evaluator/decoder pixel-storage ceiling.
inline render::GpuProcessingPhaseGrant
reservePreviewCpuPhase(const std::shared_ptr<GpuProcessingPhasePolicy>& policy,
                       const document::Snapshot& snapshot, const PreviewRequestIdentity& identity,
                       const std::size_t pixelStorageByteLimit,
                       const std::optional<std::uint64_t> nativeWorkingBytes = {}) noexcept {
    if (!policy) {
        return {};
    }
    const auto* composition = snapshot.project().findComposition(identity.compositionId);
    if (!composition) {
        return {};
    }
    std::uint64_t pixels =
        static_cast<std::uint64_t>(composition->format().width()) * composition->format().height();
    if (const auto* proxy = std::get_if<ProxyResolution>(&identity.resolution)) {
        pixels = static_cast<std::uint64_t>(proxy->extent.width()) * proxy->extent.height();
    }
    if (identity.roi) {
        pixels = static_cast<std::uint64_t>(identity.roi->extent().width()) *
                 identity.roi->extent().height();
    }
    if (nativeWorkingBytes) {
        if (pixels == 0 || pixels > std::numeric_limits<std::uint64_t>::max() / 16)
            return {};
        return policy->reserve({.output = GpuProcessingPhaseOutput::FloatFinal,
                                .pixels = pixels,
                                .encodedBytes = pixels * 16,
                                .metadataBytes = 16ULL * 1024 * 1024,
                                .retainedCoverageByteLimit = 16ULL * 1024 * 1024,
                                .requestedSourceBytes = pixelStorageByteLimit,
                                .minimumSourceBytes = pixels * 16,
                                .deviceWorkingBytes = *nativeWorkingBytes,
                                .native = true});
    }
    return reserveHostEvaluationPhase(*policy, pixels, pixelStorageByteLimit,
                                      GpuProcessingPhaseOutput::FloatFinal, true);
}

inline TaskDiagnostic previewCpuPhaseDiagnostic(const render::GpuProcessingPhaseAdmission status,
                                                std::optional<TaskAdmissionRetry> retry = {}) {
    const bool awaiting = status == render::GpuProcessingPhaseAdmission::AwaitingCapacity;
    const bool invalid = status == render::GpuProcessingPhaseAdmission::InvalidRequest;
    return {.code = awaiting  ? "bloom.preview.gpu-scene.awaiting-capacity"
                    : invalid ? "bloom.preview.processing-root-missing"
                              : "bloom.preview.gpu-scene.budget-refused",
            .severity = DiagnosticSeverity::Error,
            .summary = awaiting  ? "CPU preview awaits sampled processing capacity."
                       : invalid ? "CPU preview has no valid processing policy root."
                                 : "CPU preview processing phase was budget-refused.",
            .detail = "No evaluation or media preparation was started.",
            .suggestedAction = "Wait for capacity or reduce the processing request.",
            .admissionRetry = retry};
}
inline TaskDiagnostic previewCpuPhaseDiagnostic(const render::GpuProcessingPhaseGrant& grant,
                                                const bool native = false) {
    return previewCpuPhaseDiagnostic(
        grant.status, (grant.status == render::GpuProcessingPhaseAdmission::AwaitingCapacity ||
                       grant.status == render::GpuProcessingPhaseAdmission::BudgetRefused)
                          ? std::optional<TaskAdmissionRetry>{{grant.admissionRevision, native}}
                          : std::nullopt);
}
} // namespace bloom::runtime::detail
