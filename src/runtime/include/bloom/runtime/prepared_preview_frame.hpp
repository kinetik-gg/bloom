#pragma once

#include <bloom/runtime/reference_display_preparation.hpp>

#include <cstdint>
#include <memory>
#include <optional>

namespace bloom::runtime {

enum class PreviewOutput : std::uint8_t {
    Composition,
};

struct PreviewRequestIdentity final {
    document::ProjectId projectId;
    document::CompositionId compositionId;
    document::Revision sourceRevision;
    std::uint64_t requestGeneration = 0;
    core::RationalTime time;
    PreviewOutput output = PreviewOutput::Composition;
    EvaluationResolution resolution;
    EvaluationQuality quality = EvaluationQuality::Reference;
    EvaluationColorIntent colorIntent = EvaluationColorIntent::LinearRec709Scene;

    friend bool operator==(const PreviewRequestIdentity&, const PreviewRequestIdentity&) = default;
};

class PreparedPreviewFrame final {
  public:
    [[nodiscard]] static std::optional<PreparedPreviewFrame>
    create(std::uint64_t requestGeneration,
           std::shared_ptr<const ReferenceDisplayFrame> displayFrame) noexcept;

    [[nodiscard]] const PreviewRequestIdentity& desiredIdentity() const& noexcept {
        return desiredIdentity_;
    }
    [[nodiscard]] const PreviewRequestIdentity& desiredIdentity() const&& = delete;
    [[nodiscard]] const ProcessFrameIdentity& processIdentity() const& noexcept {
        return displayFrame_->identity().processFrame;
    }
    [[nodiscard]] const ProcessFrameIdentity& processIdentity() const&& = delete;
    [[nodiscard]] const ReferenceDisplayFrameIdentity& displayIdentity() const& noexcept {
        return displayFrame_->identity();
    }
    [[nodiscard]] const ReferenceDisplayFrameIdentity& displayIdentity() const&& = delete;
    [[nodiscard]] const std::shared_ptr<const ProcessFrame>& processFrame() const& noexcept {
        return displayFrame_->processFrame();
    }
    [[nodiscard]] const std::shared_ptr<const ProcessFrame>& processFrame() const&& = delete;
    [[nodiscard]] const std::shared_ptr<const ReferenceDisplayFrame>&
    displayFrame() const& noexcept {
        return displayFrame_;
    }
    [[nodiscard]] const std::shared_ptr<const ReferenceDisplayFrame>&
    displayFrame() const&& = delete;
    [[nodiscard]] const render::Rgba32fImage& processImage() const& noexcept {
        return displayFrame_->processFrame()->processImage();
    }
    [[nodiscard]] const render::Rgba32fImage& processImage() const&& = delete;
    [[nodiscard]] const render::PreparedReferenceDisplayBuffer& displayBuffer() const& noexcept {
        return displayFrame_->buffer();
    }
    [[nodiscard]] const render::PreparedReferenceDisplayBuffer& displayBuffer() const&& = delete;

  private:
    PreparedPreviewFrame(PreviewRequestIdentity desiredIdentity,
                         std::shared_ptr<const ReferenceDisplayFrame> displayFrame) noexcept;

    PreviewRequestIdentity desiredIdentity_;
    std::shared_ptr<const ReferenceDisplayFrame> displayFrame_;
};

enum class PreviewPreparationStatus : std::uint8_t {
    Prepared,
    Unsupported,
};

class PreviewPreparationResult final {
  public:
    [[nodiscard]] static std::optional<PreviewPreparationResult>
    prepared(std::shared_ptr<const PreparedPreviewFrame> frame) noexcept;
    [[nodiscard]] static PreviewPreparationResult unsupported() noexcept;

    [[nodiscard]] PreviewPreparationStatus status() const noexcept { return status_; }
    [[nodiscard]] const std::shared_ptr<const PreparedPreviewFrame>& frame() const noexcept {
        return frame_;
    }

  private:
    PreviewPreparationResult(PreviewPreparationStatus status,
                             std::shared_ptr<const PreparedPreviewFrame> frame) noexcept;

    PreviewPreparationStatus status_ = PreviewPreparationStatus::Unsupported;
    std::shared_ptr<const PreparedPreviewFrame> frame_;
};

using PreviewPreparationResultHandle = std::shared_ptr<const PreviewPreparationResult>;

} // namespace bloom::runtime
