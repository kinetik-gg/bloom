#include <bloom/runtime/prepared_preview_frame.hpp>

#include <exception>
#include <memory>
#include <utility>

namespace bloom::runtime {

std::optional<PreparedPreviewFrame>
PreparedPreviewFrame::create(const std::uint64_t requestGeneration,
                             std::shared_ptr<const ReferenceDisplayFrame> displayFrame,
                             const PreviewResolutionPolicy resolutionPolicy) noexcept {
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
        .resolutionPolicy = resolutionPolicy,
        .roi = processIdentity.roi,
        .viewAdjust = displayFrame->identity().viewAdjust,
        .displayName = displayFrame->identity().displayName,
        .viewName = displayFrame->identity().viewName,
        .showLook = displayFrame->identity().showLook,
    };
    return PreparedPreviewFrame(desiredIdentity, DisplayFrameVariant(std::move(displayFrame)));
}

std::optional<PreparedPreviewFrame>
PreparedPreviewFrame::createQualified(const std::uint64_t requestGeneration,
                                      std::shared_ptr<const QualifiedDisplayFrame> displayFrame,
                                      const PreviewResolutionPolicy resolutionPolicy) noexcept {
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
        .resolutionPolicy = resolutionPolicy,
        .roi = processIdentity.roi,
        .viewAdjust = displayFrame->identity().viewAdjust,
        .displayName = displayFrame->identity().displayName,
        .viewName = displayFrame->identity().viewName,
        .showLook = displayFrame->identity().showLook,
    };
    return PreparedPreviewFrame(desiredIdentity, DisplayFrameVariant(std::move(displayFrame)));
}

PreviewDisplayOnlyFrame::PreviewDisplayOnlyFrame(
    PreviewRequestIdentity desiredIdentity, ProcessFrameIdentity processIdentity,
    render::PreparedReferenceDisplayBuffer buffer, const bool isOcioQualified,
    std::vector<EvaluatedOperationBounds> bounds) noexcept
    : desiredIdentity_(std::move(desiredIdentity)), processIdentity_(std::move(processIdentity)),
      buffer_(std::move(buffer)), isOcioQualified_(isOcioQualified), bounds_(std::move(bounds)) {}

std::optional<PreviewDisplayOnlyFrame>
PreviewDisplayOnlyFrame::create(const PreparedPreviewFrame& source,
                                const std::size_t pixelStorageByteLimit) noexcept {
    const auto view = source.displayBufferView();
    if (!view.has_value()) {
        return std::nullopt;
    }
    const auto descriptor =
        render::ReferenceDisplayBufferDescriptor::create(view->displayWindow, view->pixelAspect);
    if (!descriptor) {
        return std::nullopt;
    }
    // The two display products are both packed straight RGBA8 over the same window, so one storage
    // type holds either -- but that is an invariant worth checking rather than assuming, because a
    // mismatch here would mean copying pixels into a shape that does not describe them.
    if (descriptor.value()->layout() != view->layout) {
        return std::nullopt;
    }
    const auto geometry = source.evaluatedBounds();
    if (geometry.size_bytes() > pixelStorageByteLimit)
        return std::nullopt;
    auto buffer = render::PreparedReferenceDisplayBuffer::create(
        *descriptor.value(), view->pixels, pixelStorageByteLimit - geometry.size_bytes());
    if (!buffer) {
        return std::nullopt;
    }
    try {
        return PreviewDisplayOnlyFrame(
            source.desiredIdentity(), source.processIdentity(), std::move(*buffer.value()),
            view->isOcioQualified,
            std::vector<EvaluatedOperationBounds>(geometry.begin(), geometry.end()));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<PreviewDisplayBufferView>
PreviewDisplayOnlyFrame::displayBufferView() const noexcept {
    const auto* descriptor = buffer_.descriptor();
    if (descriptor == nullptr) {
        return std::nullopt;
    }
    return PreviewDisplayBufferView{
        .displayWindow = descriptor->displayWindow(),
        .pixelAspect = descriptor->pixelAspect(),
        .layout = descriptor->layout(),
        .pixels = buffer_.pixels(),
        .isOcioQualified = isOcioQualified_,
    };
}

std::size_t PreviewDisplayOnlyFrame::displayByteCost() const noexcept {
    return buffer_.pixels().size_bytes() + std::span(bounds_).size_bytes();
}

std::optional<PreparedPreviewFrame> PreparedPreviewFrame::createDisplayOnly(
    const std::uint64_t requestGeneration,
    std::shared_ptr<const PreviewDisplayOnlyFrame> displayFrame) noexcept {
    if (requestGeneration == 0 || displayFrame == nullptr ||
        !displayFrame->displayBufferView().has_value()) {
        return std::nullopt;
    }
    PreviewRequestIdentity desiredIdentity = displayFrame->desiredIdentity();
    desiredIdentity.requestGeneration = requestGeneration;
    return PreparedPreviewFrame(desiredIdentity, DisplayFrameVariant(std::move(displayFrame)));
}

PreparedPreviewFrame::PreparedPreviewFrame(PreviewRequestIdentity desiredIdentity,
                                           DisplayFrameVariant displayFrame) noexcept
    : desiredIdentity_(std::move(desiredIdentity)), displayFrame_(std::move(displayFrame)) {}

// std::visit/std::get both have a (never-actually-reachable-here, since displayFrame_ is only ever
// constructed by create()/createQualified() and never reassigned) valueless_by_exception exception
// path that clang-tidy's bugprone-exception-escape correctly flags inside a noexcept function.
// Every accessor below instead uses the non-throwing std::get_if, matching this file's existing
// precondition style (a caller violating a documented precondition, such as calling a
// reference-only accessor on a qualified-backed envelope, gets a null-pointer dereference rather
// than a thrown bad_variant_access -- still a loud failure, never silent misbehavior).
using ReferencePtr = std::shared_ptr<const ReferenceDisplayFrame>;
using QualifiedPtr = std::shared_ptr<const QualifiedDisplayFrame>;
using DisplayOnlyPtr = std::shared_ptr<const PreviewDisplayOnlyFrame>;

bool PreparedPreviewFrame::isOcioQualified() const noexcept {
    if (const auto* displayOnly = std::get_if<DisplayOnlyPtr>(&displayFrame_)) {
        return (*displayOnly)->isOcioQualified();
    }
    return std::holds_alternative<QualifiedPtr>(displayFrame_);
}

bool PreparedPreviewFrame::hasProcessFrame() const noexcept {
    return !std::holds_alternative<DisplayOnlyPtr>(displayFrame_);
}

std::span<const EvaluatedOperationBounds> PreparedPreviewFrame::evaluatedBounds() const noexcept {
    if (const auto* displayOnly = std::get_if<DisplayOnlyPtr>(&displayFrame_))
        return (*displayOnly)->evaluatedBounds();
    return processFrame()->evaluatedBounds();
}

const ProcessFrameIdentity& PreparedPreviewFrame::processIdentity() const& noexcept {
    if (const auto* reference = std::get_if<ReferencePtr>(&displayFrame_)) {
        return (*reference)->identity().processFrame;
    }
    if (const auto* displayOnly = std::get_if<DisplayOnlyPtr>(&displayFrame_)) {
        return (*displayOnly)->processIdentity();
    }
    return std::get_if<QualifiedPtr>(&displayFrame_)->get()->identity().processFrame;
}

const std::shared_ptr<const ProcessFrame>& PreparedPreviewFrame::processFrame() const& noexcept {
    if (const auto* reference = std::get_if<ReferencePtr>(&displayFrame_)) {
        return (*reference)->processFrame();
    }
    if (std::holds_alternative<DisplayOnlyPtr>(displayFrame_)) {
        // A display-only frame answers this honestly rather than refusing: it kept no process
        // frame, and hasProcessFrame() says so in advance. The handle is a function-local static so
        // a reference to it stays valid, and it is const so nothing can ever fill it in.
        static const std::shared_ptr<const ProcessFrame> none;
        return none;
    }
    return std::get_if<QualifiedPtr>(&displayFrame_)->get()->processFrame();
}

std::optional<PreviewDisplayBufferView> PreparedPreviewFrame::displayBufferView() const noexcept {
    if (const auto* displayOnly = std::get_if<DisplayOnlyPtr>(&displayFrame_)) {
        return (*displayOnly)->displayBufferView();
    }
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
    const auto* qualified = std::get_if<QualifiedPtr>(&displayFrame_)->get();
    if (const auto& adjusted = qualified->adjustedBuffer()) {
        const auto* descriptor = adjusted->descriptor();
        if (!descriptor)
            return std::nullopt;
        return PreviewDisplayBufferView{.displayWindow = descriptor->displayWindow(),
                                        .pixelAspect = descriptor->pixelAspect(),
                                        .layout = descriptor->layout(),
                                        .pixels = adjusted->pixels(),
                                        .isOcioQualified = true};
    }
    const auto& buffer = qualified->buffer();
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

std::optional<core::Color4d> PreparedPreviewFrame::displayLinearProbe() const noexcept {
    if (const auto* qualified = std::get_if<QualifiedPtr>(&displayFrame_)) {
        return (*qualified)->displayLinearProbe();
    }
    return std::nullopt;
}

const std::shared_ptr<const QualifiedDisplayFrame>&
PreparedPreviewFrame::qualifiedDisplayFrame() const& noexcept {
    return *std::get_if<QualifiedPtr>(&displayFrame_);
}

const QualifiedDisplayFrameIdentity&
PreparedPreviewFrame::qualifiedDisplayIdentity() const& noexcept {
    return std::get_if<QualifiedPtr>(&displayFrame_)->get()->identity();
}

const std::shared_ptr<const PreviewDisplayOnlyFrame>&
PreparedPreviewFrame::displayOnlyFrame() const& noexcept {
    return *std::get_if<DisplayOnlyPtr>(&displayFrame_);
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
