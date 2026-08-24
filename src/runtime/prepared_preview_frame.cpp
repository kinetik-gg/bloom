#include <bloom/runtime/prepared_preview_frame.hpp>

#include <utility>

namespace bloom::runtime {

std::optional<PreparedPreviewFrame>
PreparedPreviewFrame::create(const std::uint64_t requestGeneration,
                             std::shared_ptr<const ReferenceDisplayFrame> displayFrame) noexcept {
    if (requestGeneration == 0 || displayFrame == nullptr ||
        displayFrame->processFrame() == nullptr ||
        displayFrame->identity().processFrame.plan == nullptr) {
        return std::nullopt;
    }

    const auto& processIdentity = displayFrame->identity().processFrame;
    const auto& plan = *processIdentity.plan;
    if (processIdentity.output != plan.output ||
        displayFrame->processFrame()->identity() != processIdentity) {
        return std::nullopt;
    }

    PreviewRequestIdentity desiredIdentity{
        .projectId = plan.projectId,
        .compositionId = plan.compositionId,
        .sourceRevision = plan.sourceRevision,
        .requestGeneration = requestGeneration,
        .time = processIdentity.time,
        .output = PreviewOutput::Composition,
        .resolution = processIdentity.resolution,
        .quality = processIdentity.quality,
        .colorIntent = processIdentity.colorIntent,
    };
    return PreparedPreviewFrame(desiredIdentity, std::move(displayFrame));
}

PreparedPreviewFrame::PreparedPreviewFrame(
    PreviewRequestIdentity desiredIdentity,
    std::shared_ptr<const ReferenceDisplayFrame> displayFrame) noexcept
    : desiredIdentity_(desiredIdentity), displayFrame_(std::move(displayFrame)) {}

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
