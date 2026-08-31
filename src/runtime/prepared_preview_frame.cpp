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
    if (processIdentity.output != plan.output() ||
        displayFrame->processFrame()->identity() != processIdentity) {
        return std::nullopt;
    }

    PreviewRequestIdentity desiredIdentity{
        .projectId = plan.projectId(),
        .compositionId = plan.compositionId(),
        .sourceRevision = plan.sourceRevision(),
        .requestGeneration = requestGeneration,
        .time = processIdentity.time,
        .output = PreviewOutput::Composition,
        .resolution = processIdentity.resolution,
        .quality = processIdentity.quality,
        .colorIntent = processIdentity.colorIntent,
    };
    return PreparedPreviewFrame(desiredIdentity, DisplayFrameVariant(std::move(displayFrame)));
}

std::optional<PreparedPreviewFrame> PreparedPreviewFrame::createQualified(
    const std::uint64_t requestGeneration,
    std::shared_ptr<const QualifiedDisplayFrame> displayFrame) noexcept {
    if (requestGeneration == 0 || displayFrame == nullptr ||
        displayFrame->processFrame() == nullptr ||
        displayFrame->identity().processFrame.plan == nullptr) {
        return std::nullopt;
    }

    const auto& processIdentity = displayFrame->identity().processFrame;
    const auto& plan = *processIdentity.plan;
    if (processIdentity.output != plan.output() ||
        displayFrame->processFrame()->identity() != processIdentity) {
        return std::nullopt;
    }

    PreviewRequestIdentity desiredIdentity{
        .projectId = plan.projectId(),
        .compositionId = plan.compositionId(),
        .sourceRevision = plan.sourceRevision(),
        .requestGeneration = requestGeneration,
        .time = processIdentity.time,
        .output = PreviewOutput::Composition,
        .resolution = processIdentity.resolution,
        .quality = processIdentity.quality,
        .colorIntent = processIdentity.colorIntent,
    };
    return PreparedPreviewFrame(desiredIdentity, DisplayFrameVariant(std::move(displayFrame)));
}

PreparedPreviewFrame::PreparedPreviewFrame(PreviewRequestIdentity desiredIdentity,
                                           DisplayFrameVariant displayFrame) noexcept
    : desiredIdentity_(desiredIdentity), displayFrame_(std::move(displayFrame)) {}

// std::visit/std::get both have a (never-actually-reachable-here, since displayFrame_ is only ever
// constructed by create()/createQualified() and never reassigned) valueless_by_exception exception
// path that clang-tidy's bugprone-exception-escape correctly flags inside a noexcept function.
// Every accessor below instead uses the non-throwing std::get_if, matching this file's existing
// precondition style (a caller violating a documented precondition, such as calling a
// reference-only accessor on a qualified-backed envelope, gets a null-pointer dereference rather
// than a thrown bad_variant_access -- still a loud failure, never silent misbehavior).
using ReferencePtr = std::shared_ptr<const ReferenceDisplayFrame>;
using QualifiedPtr = std::shared_ptr<const QualifiedDisplayFrame>;

const ProcessFrameIdentity& PreparedPreviewFrame::processIdentity() const& noexcept {
    if (const auto* reference = std::get_if<ReferencePtr>(&displayFrame_)) {
        return (*reference)->identity().processFrame;
    }
    return std::get_if<QualifiedPtr>(&displayFrame_)->get()->identity().processFrame;
}

const std::shared_ptr<const ProcessFrame>& PreparedPreviewFrame::processFrame() const& noexcept {
    if (const auto* reference = std::get_if<ReferencePtr>(&displayFrame_)) {
        return (*reference)->processFrame();
    }
    return std::get_if<QualifiedPtr>(&displayFrame_)->get()->processFrame();
}

std::optional<PreviewDisplayBufferView> PreparedPreviewFrame::displayBufferView() const noexcept {
    if (const auto* reference = std::get_if<ReferencePtr>(&displayFrame_)) {
        const auto viewResult = (*reference)->buffer().view();
        if (!viewResult) {
            return std::nullopt;
        }
        const auto view = *viewResult.value();
        const auto descriptorResult = view.descriptor();
        if (!descriptorResult.has_value()) {
            return std::nullopt;
        }
        return PreviewDisplayBufferView{
            .displayWindow = descriptorResult->displayWindow(),
            .pixelAspect = descriptorResult->pixelAspect(),
            .layout = descriptorResult->layout(),
            .pixels = view.pixels(),
            .isOcioQualified = false,
        };
    }
    const auto& buffer = std::get_if<QualifiedPtr>(&displayFrame_)->get()->buffer();
    if (!buffer.isValid()) {
        return std::nullopt;
    }
    return PreviewDisplayBufferView{
        .displayWindow = buffer.displayWindow(),
        .pixelAspect = buffer.pixelAspect(),
        .layout = buffer.layout(),
        .pixels = buffer.pixels(),
        .isOcioQualified = true,
    };
}

const std::shared_ptr<const ReferenceDisplayFrame>&
PreparedPreviewFrame::displayFrame() const& noexcept {
    return *std::get_if<ReferencePtr>(&displayFrame_);
}

const ReferenceDisplayFrameIdentity& PreparedPreviewFrame::displayIdentity() const& noexcept {
    return std::get_if<ReferencePtr>(&displayFrame_)->get()->identity();
}

const render::PreparedReferenceDisplayBuffer&
PreparedPreviewFrame::displayBuffer() const& noexcept {
    return std::get_if<ReferencePtr>(&displayFrame_)->get()->buffer();
}

const std::shared_ptr<const QualifiedDisplayFrame>&
PreparedPreviewFrame::qualifiedDisplayFrame() const& noexcept {
    return *std::get_if<QualifiedPtr>(&displayFrame_);
}

const QualifiedDisplayFrameIdentity&
PreparedPreviewFrame::qualifiedDisplayIdentity() const& noexcept {
    return std::get_if<QualifiedPtr>(&displayFrame_)->get()->identity();
}

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
