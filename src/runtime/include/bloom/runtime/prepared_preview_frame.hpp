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

enum class PreviewResolutionPolicy : std::uint8_t { Auto, Full, Half, Quarter };

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

    PreviewResolutionPolicy resolutionPolicy = PreviewResolutionPolicy::Auto;

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

class PreparedPreviewFrame;

// A preview frame reduced to what a viewer actually paints: the packed RGBA8 display buffer, the
// request identity it answers, and the process identity that produced it -- and NOT the Float32
// process image (task PERF1, FORMAL AMENDMENT 1).
//
// This exists because of what retaining frames costs. At 1920x1080 the packed display buffer is
// about 8 MB while the process image it was mapped from is about 33 MB, so a RAM preview cache that
// held whole frames would spend four fifths of its budget on pixels playback never reads. A
// display-only frame is the same picture at a quarter of the memory: a 2 GiB budget holds roughly
// 250 frames of it instead of 50.
//
// What is deliberately NOT here is the process frame. Anything that needs scene-linear pixels --
// export, sampling, a future analysis -- must evaluate the frame again rather than be handed a
// cached one; hasProcessFrame() on the envelope below is how a caller asks rather than assumes.
// The process IDENTITY is kept because it is small (a shared plan pointer plus scalars) and is what
// makes a retained frame self-describing.
class PreviewDisplayOnlyFrame final {
  public:
    PreviewDisplayOnlyFrame(const PreviewDisplayOnlyFrame&) = delete;
    PreviewDisplayOnlyFrame& operator=(const PreviewDisplayOnlyFrame&) = delete;
    PreviewDisplayOnlyFrame(PreviewDisplayOnlyFrame&&) noexcept = default;
    PreviewDisplayOnlyFrame& operator=(PreviewDisplayOnlyFrame&&) = delete;
    ~PreviewDisplayOnlyFrame() = default;

    // Copies `source`'s packed display pixels -- whichever alternative produced them -- and drops
    // everything else. std::nullopt when the source has no valid display buffer, when its packed
    // layout is not the one this storage can hold, or when the copy would exceed the byte limit.
    [[nodiscard]] static std::optional<PreviewDisplayOnlyFrame>
    create(const PreparedPreviewFrame& source, std::size_t pixelStorageByteLimit) noexcept;

    [[nodiscard]] const PreviewRequestIdentity& desiredIdentity() const& noexcept {
        return desiredIdentity_;
    }
    [[nodiscard]] const PreviewRequestIdentity& desiredIdentity() const&& = delete;
    [[nodiscard]] const ProcessFrameIdentity& processIdentity() const& noexcept {
        return processIdentity_;
    }
    [[nodiscard]] const ProcessFrameIdentity& processIdentity() const&& = delete;
    [[nodiscard]] bool isOcioQualified() const noexcept { return isOcioQualified_; }
    [[nodiscard]] std::optional<PreviewDisplayBufferView> displayBufferView() const noexcept;
    // What retaining this frame costs: its packed display pixels and nothing else.
    [[nodiscard]] std::size_t displayByteCost() const noexcept;

  private:
    PreviewDisplayOnlyFrame(PreviewRequestIdentity desiredIdentity,
                            ProcessFrameIdentity processIdentity,
                            render::PreparedReferenceDisplayBuffer buffer,
                            bool isOcioQualified) noexcept;

    PreviewRequestIdentity desiredIdentity_;
    ProcessFrameIdentity processIdentity_;
    // One storage type for both display products: PreviewDisplayBufferView already normalizes them
    // to the same packed shape, so the qualified flag is carried beside the pixels rather than by
    // the pixels' own type.
    render::PreparedReferenceDisplayBuffer buffer_;
    bool isOcioQualified_ = false;
};

// A closed alternative over the two production display products (issue #97, task C3, design
// decision 2) plus the retained display-only product above: the temporary built-in reference
// product (ReferenceDisplayFrame), the qualified OCIO product (QualifiedDisplayFrame), or a
// PreviewDisplayOnlyFrame -- exactly one of the three. The reference alternative's own accessors
// (displayFrame(), displayIdentity(), displayBuffer()) keep their exact pre-existing signatures and
// behavior for backward compatibility -- calling one of them when another alternative is active is
// a precondition violation (a null-pointer dereference, never a thrown exception): no existing or
// new caller does this, since isOcioQualified()/qualifiedDisplayFrame()/displayBufferView() are how
// a caller that does not already know which alternative it holds finds out.
//
// processFrame() is the one accessor a display-only frame ANSWERS rather than refuses: it hands
// back a null handle, and hasProcessFrame() says so in advance. processImage() dereferences it and
// is therefore a precondition violation on such a frame -- a caller that needs scene-linear pixels
// must evaluate them rather than take them from a cache that deliberately did not keep them.
class PreparedPreviewFrame final {
  public:
    [[nodiscard]] static std::optional<PreparedPreviewFrame>
    create(std::uint64_t requestGeneration,
           std::shared_ptr<const ReferenceDisplayFrame> displayFrame) noexcept;
    [[nodiscard]] static std::optional<PreparedPreviewFrame>
    createQualified(std::uint64_t requestGeneration,
                    std::shared_ptr<const QualifiedDisplayFrame> displayFrame) noexcept;
    // Re-stamps a retained display-only frame with the generation of the request it now answers.
    [[nodiscard]] static std::optional<PreparedPreviewFrame>
    createDisplayOnly(std::uint64_t requestGeneration,
                      std::shared_ptr<const PreviewDisplayOnlyFrame> displayFrame) noexcept;

    [[nodiscard]] const PreviewRequestIdentity& desiredIdentity() const& noexcept {
        return desiredIdentity_;
    }
    [[nodiscard]] const PreviewRequestIdentity& desiredIdentity() const&& = delete;

    // True exactly when this frame's pixels came from the qualified transform. The reference
    // alternative always reports false here, never silently relabeled (design decision 2); a
    // display-only frame reports what the product it was copied from reported.
    [[nodiscard]] bool isOcioQualified() const noexcept;

    // False exactly for a display-only frame: its scene-linear pixels were deliberately not
    // retained, so processFrame() is null and processImage() must not be called.
    [[nodiscard]] bool hasProcessFrame() const noexcept;

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

    // Display-only accessor; precondition mirrors the two above.
    [[nodiscard]] const std::shared_ptr<const PreviewDisplayOnlyFrame>&
    displayOnlyFrame() const& noexcept;
    [[nodiscard]] const std::shared_ptr<const PreviewDisplayOnlyFrame>&
    displayOnlyFrame() const&& = delete;

  private:
    using DisplayFrameVariant = std::variant<std::shared_ptr<const ReferenceDisplayFrame>,
                                             std::shared_ptr<const QualifiedDisplayFrame>,
                                             std::shared_ptr<const PreviewDisplayOnlyFrame>>;

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
