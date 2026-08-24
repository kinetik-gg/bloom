#include <bloom/runtime/prepared_preview_frame.hpp>

#include <utility>

namespace bloom::runtime {

std::optional<PreparedPreviewFrame>
PreparedPreviewFrame::create(const std::uint64_t requestGeneration,
                             std::shared_ptr<const EvaluatedFrame> evaluatedFrame) noexcept {
    if (requestGeneration == 0 || evaluatedFrame == nullptr ||
        evaluatedFrame->identity().plan == nullptr) {
        return std::nullopt;
    }

    const auto& cacheIdentity = evaluatedFrame->identity();
    const auto& plan = *cacheIdentity.plan;
    if (cacheIdentity.output != plan.output) {
        return std::nullopt;
    }

    PreviewRequestIdentity desiredIdentity{
        .projectId = plan.projectId,
        .compositionId = plan.compositionId,
        .sourceRevision = plan.sourceRevision,
        .requestGeneration = requestGeneration,
        .time = cacheIdentity.time,
        .output = PreviewOutput::Composition,
        .resolution = cacheIdentity.resolution,
        .quality = cacheIdentity.quality,
        .colorIntent = cacheIdentity.colorIntent,
    };
    return PreparedPreviewFrame(std::move(desiredIdentity), std::move(evaluatedFrame));
}

PreparedPreviewFrame::PreparedPreviewFrame(
    PreviewRequestIdentity desiredIdentity,
    std::shared_ptr<const EvaluatedFrame> evaluatedFrame) noexcept
    : desiredIdentity_(std::move(desiredIdentity)), evaluatedFrame_(std::move(evaluatedFrame)) {}

std::optional<PreviewPreparationResult>
PreviewPreparationResult::prepared(std::shared_ptr<const PreparedPreviewFrame> frame) noexcept {
    if (frame == nullptr) {
        return std::nullopt;
    }
    return PreviewPreparationResult(PreviewPreparationStatus::Prepared, std::move(frame));
}

PreviewPreparationResult PreviewPreparationResult::unsupported() noexcept {
    return PreviewPreparationResult(PreviewPreparationStatus::Unsupported, {});
}

PreviewPreparationResult::PreviewPreparationResult(
    const PreviewPreparationStatus status,
    std::shared_ptr<const PreparedPreviewFrame> frame) noexcept
    : status_(status), frame_(std::move(frame)) {}

} // namespace bloom::runtime
