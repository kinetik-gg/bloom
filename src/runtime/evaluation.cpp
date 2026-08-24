#include <bloom/runtime/evaluation.hpp>

#include <utility>

namespace bloom::runtime {

bool operator==(const EvaluationCacheIdentity& lhs, const EvaluationCacheIdentity& rhs) {
    const bool plansEqual = lhs.plan == rhs.plan ||
                            (lhs.plan != nullptr && rhs.plan != nullptr && *lhs.plan == *rhs.plan);
    return plansEqual && lhs.time == rhs.time && lhs.output == rhs.output &&
           lhs.resolution == rhs.resolution && lhs.quality == rhs.quality &&
           lhs.colorIntent == rhs.colorIntent && lhs.provider == rhs.provider &&
           lhs.displayIntent == rhs.displayIntent &&
           lhs.evaluatorSemanticsVersion == rhs.evaluatorSemanticsVersion &&
           lhs.animationSamplingSemanticsVersion == rhs.animationSamplingSemanticsVersion &&
           lhs.imagePrimitiveSemanticsVersion == rhs.imagePrimitiveSemanticsVersion &&
           lhs.displayMapperSemanticsVersion == rhs.displayMapperSemanticsVersion;
}

std::string_view evaluationDiagnosticCodeId(const EvaluationDiagnosticCode code) noexcept {
    switch (code) {
    case EvaluationDiagnosticCode::InvalidRequest:
        return "bloom.runtime.evaluation.invalid-request";
    case EvaluationDiagnosticCode::InvalidPlan:
        return "bloom.runtime.evaluation.invalid-plan";
    case EvaluationDiagnosticCode::InvalidProxyPixelAspect:
        return "bloom.runtime.evaluation.invalid-proxy-pixel-aspect";
    case EvaluationDiagnosticCode::ArithmeticOverflow:
        return "bloom.runtime.evaluation.arithmetic-overflow";
    case EvaluationDiagnosticCode::PixelStorageBudgetExceeded:
        return "bloom.runtime.evaluation.pixel-storage-budget-exceeded";
    case EvaluationDiagnosticCode::AllocationFailure:
        return "bloom.runtime.evaluation.allocation-failure";
    case EvaluationDiagnosticCode::InvalidPixel:
        return "bloom.runtime.evaluation.invalid-pixel";
    case EvaluationDiagnosticCode::InvalidParameter:
        return "bloom.runtime.evaluation.invalid-parameter";
    case EvaluationDiagnosticCode::UnsupportedFloatingPointEnvironment:
        return "bloom.runtime.evaluation.unsupported-floating-point-environment";
    case EvaluationDiagnosticCode::IncompatibleImageDescriptor:
        return "bloom.runtime.evaluation.incompatible-image-descriptor";
    case EvaluationDiagnosticCode::InternalInvariant:
        return "bloom.runtime.evaluation.internal-invariant";
    }
    return "bloom.runtime.evaluation.unknown";
}

EvaluatedFrame::EvaluatedFrame(EvaluationCacheIdentity identity, render::Rgba32fImage processImage,
                               render::PreparedReferenceDisplayBuffer displayBuffer) noexcept
    : identity_(std::move(identity)), processImage_(std::move(processImage)),
      displayBuffer_(std::move(displayBuffer)) {}

EvaluationResult EvaluationResult::evaluated(std::shared_ptr<const EvaluatedFrame> frame,
                                             std::vector<EvaluationDiagnostic> diagnostics) {
    if (frame == nullptr) {
        return failed({.code = EvaluationDiagnosticCode::InternalInvariant,
                       .severity = DiagnosticSeverity::Error,
                       .subject = {},
                       .summary = "Evaluation produced no frame",
                       .detail = "A successful evaluation must publish an immutable frame."});
    }
    return EvaluationResult(EvaluationStatus::Evaluated, std::move(frame), std::move(diagnostics));
}

EvaluationResult EvaluationResult::cancelled(std::vector<EvaluationDiagnostic> diagnostics) {
    return EvaluationResult(EvaluationStatus::Cancelled, {}, std::move(diagnostics));
}

EvaluationResult EvaluationResult::failed(EvaluationDiagnostic diagnostic) {
    std::vector<EvaluationDiagnostic> diagnostics;
    diagnostics.push_back(std::move(diagnostic));
    return failed(std::move(diagnostics));
}

EvaluationResult EvaluationResult::failed(std::vector<EvaluationDiagnostic> diagnostics) {
    return EvaluationResult(EvaluationStatus::Failed, {}, std::move(diagnostics));
}

EvaluationResult::EvaluationResult(const EvaluationStatus status,
                                   std::shared_ptr<const EvaluatedFrame> frame,
                                   std::vector<EvaluationDiagnostic> diagnostics) noexcept
    : status_(status), frame_(std::move(frame)), diagnostics_(std::move(diagnostics)) {}

} // namespace bloom::runtime
