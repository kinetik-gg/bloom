#pragma once

#include <bloom/runtime/qualified_display_preparation.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <variant>

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

// The viewer's normalized, alternative-agnostic view of a published display buffer (issue #97,
// task C3, design decision 2): "The viewer consumes the packed RGBA8 buffer identically in both
// cases (same packing/alpha association) -- the difference is identity/provenance and the
// qualified flag." Both render::ReferenceDisplayBufferDescriptor/View and
// color::PreparedDisplayFrame already expose the same shape (display window, pixel aspect, packed
// layout, packed RGBA8 pixels); this struct is that shared shape, sourced from whichever
// alternative PreparedPreviewFrame actually holds. isOcioQualified is the ONLY place that
// distinguishes them -- it is never inferred from anything else, and the reference alternative
// always reports false here exactly as render::ReferenceDisplayBufferDescriptor::isOcioQualified()
// itself always does.
struct PreviewDisplayBufferView final {
    render::ImageWindow displayWindow;
    core::PixelAspectRatio pixelAspect;
    render::PackedImageLayout layout;
    std::span<const render::Rgba8> pixels;
    bool isOcioQualified = false;
};

// A closed alternative over the two production display products (issue #97, task C3, design
// decision 2): EITHER the temporary built-in reference product (ReferenceDisplayFrame) OR the
// qualified OCIO product (QualifiedDisplayFrame), never both and never a third kind. The reference
// alternative's own accessors (displayFrame(), displayIdentity(), displayBuffer()) keep their exact
// pre-existing signatures and behavior for backward compatibility -- calling one of them when the
// qualified alternative is active is a precondition violation (std::get throws bad_variant_access,
// which a noexcept accessor turns into std::terminate): no existing or new caller does this, since
// isOcioQualified()/qualifiedDisplayFrame()/displayBufferView() are how a caller that does not
// already know which alternative it holds finds out.
class PreparedPreviewFrame final {
  public:
    [[nodiscard]] static std::optional<PreparedPreviewFrame>
    create(std::uint64_t requestGeneration,
           std::shared_ptr<const ReferenceDisplayFrame> displayFrame) noexcept;
    [[nodiscard]] static std::optional<PreparedPreviewFrame>
    createQualified(std::uint64_t requestGeneration,
                    std::shared_ptr<const QualifiedDisplayFrame> displayFrame) noexcept;

    [[nodiscard]] const PreviewRequestIdentity& desiredIdentity() const& noexcept {
        return desiredIdentity_;
    }
    [[nodiscard]] const PreviewRequestIdentity& desiredIdentity() const&& = delete;

    // True exactly when the qualified alternative is active. The reference alternative always
    // reports false here, never silently relabeled (design decision 2).
    [[nodiscard]] bool isOcioQualified() const noexcept {
        return std::holds_alternative<std::shared_ptr<const QualifiedDisplayFrame>>(displayFrame_);
    }

    [[nodiscard]] const ProcessFrameIdentity& processIdentity() const& noexcept;
    [[nodiscard]] const ProcessFrameIdentity& processIdentity() const&& = delete;
    [[nodiscard]] const std::shared_ptr<const ProcessFrame>& processFrame() const& noexcept;
    [[nodiscard]] const std::shared_ptr<const ProcessFrame>& processFrame() const&& = delete;
    [[nodiscard]] const render::Rgba32fImage& processImage() const& noexcept {
        return processFrame()->processImage();
    }
    [[nodiscard]] const render::Rgba32fImage& processImage() const&& = delete;

    // Alternative-agnostic packed-buffer accessor (design decision 2); std::nullopt only if the
    // active alternative's own buffer is unexpectedly invalid (never observed for a successfully
    // published frame -- both preparers reject publishing an invalid buffer).
    [[nodiscard]] std::optional<PreviewDisplayBufferView> displayBufferView() const noexcept;

    // Reference-only accessors; see the class-level precondition documentation above.
    [[nodiscard]] const std::shared_ptr<const ReferenceDisplayFrame>&
    displayFrame() const& noexcept;
    [[nodiscard]] const std::shared_ptr<const ReferenceDisplayFrame>&
    displayFrame() const&& = delete;
    [[nodiscard]] const ReferenceDisplayFrameIdentity& displayIdentity() const& noexcept;
    [[nodiscard]] const ReferenceDisplayFrameIdentity& displayIdentity() const&& = delete;
    [[nodiscard]] const render::PreparedReferenceDisplayBuffer& displayBuffer() const& noexcept;
    [[nodiscard]] const render::PreparedReferenceDisplayBuffer& displayBuffer() const&& = delete;

    // Qualified-only accessors; precondition mirrors the reference-only accessors above.
    [[nodiscard]] const std::shared_ptr<const QualifiedDisplayFrame>&
    qualifiedDisplayFrame() const& noexcept;
    [[nodiscard]] const std::shared_ptr<const QualifiedDisplayFrame>&
    qualifiedDisplayFrame() const&& = delete;
    [[nodiscard]] const QualifiedDisplayFrameIdentity& qualifiedDisplayIdentity() const& noexcept;
    [[nodiscard]] const QualifiedDisplayFrameIdentity& qualifiedDisplayIdentity() const&& = delete;

  private:
    using DisplayFrameVariant = std::variant<std::shared_ptr<const ReferenceDisplayFrame>,
                                             std::shared_ptr<const QualifiedDisplayFrame>>;

    PreparedPreviewFrame(PreviewRequestIdentity desiredIdentity,
                         DisplayFrameVariant displayFrame) noexcept;

    PreviewRequestIdentity desiredIdentity_;
    DisplayFrameVariant displayFrame_;
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
