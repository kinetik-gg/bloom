#pragma once

#include <bloom/runtime/gpu_resident_frame_lease.hpp>
#include <bloom/runtime/qualified_display_preparation.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

// Forward-declared at its true namespace scope, never as a nested bloom::runtime::render, so the
// existing `render::` spellings throughout this header keep resolving to bloom::render.
namespace bloom::render {
struct GpuNeutralDisplayReadback;
class GpuDisplayImage;
} // namespace bloom::render

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

    std::optional<render::ImageWindow> roi = std::nullopt;
    ViewAdjust viewAdjust{};
    std::string displayName;
    std::string viewName;
    bool showLook = true;

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

class GpuNeutralDisplayQualificationReport;
class GpuResidentPreviewQualificationReport;

// Which execution produced a display-only frame's packed pixels. This is provenance, not a second
// color model: the CPU reference mapper, the qualified CPU Bloom Neutral transform, and the
// qualified GPU Bloom Neutral transform all publish the same straight-RGBA8 sRGB packing.
//
// GpuResident is the fourth, closed display arm: the pixels never crossed the host boundary at all.
// They live in one opaque, owner-bound GpuResidentFrameLease; there is no packed CPU buffer, no
// native object, and no GpuNeutralDisplayReadback. It is deliberately NOT spelled GpuNeutral: that
// provider means the PACKED READBACK product, and stamping this arm with it would claim a host
// transfer that did not happen.
enum class PreviewDisplayProvider : std::uint8_t {
    CpuReference,
    CpuOcio,
    GpuNeutral,
    GpuResident,
};

// The provenance a display-only frame retains beside its pixels. `gpuQualification` is non-null
// exactly for GpuNeutral and is the immutable report from the dispatch that produced the pixels; a
// CPU frame never carries one. The report is retained (not copied) and never reinterpreted. A
// GpuResident frame reports GpuResident here and carries its own immutable resident qualification
// report on PreviewResidentDisplayFrame, because that report is a different type.
struct PreviewDisplayProvenance final {
    PreviewDisplayProvider provider = PreviewDisplayProvider::CpuReference;
    std::shared_ptr<const GpuNeutralDisplayQualificationReport> gpuQualification;

    friend bool operator==(const PreviewDisplayProvenance&,
                           const PreviewDisplayProvenance&) = default;
};

class PreviewDisplayOnlyFrame;
class PreviewResidentDisplayFrame;
class PreparedPreviewFrame;
class PreviewCpuStage;

// The one validating GPU product entry point, declared here so PreviewDisplayOnlyFrame can friend
// it to construct its private, already-validated storage directly. Defined in
// gpu_preview_display_product.cpp.
[[nodiscard]] std::optional<PreparedPreviewFrame>
makeGpuNeutralDisplayPreview(const PreviewCpuStage& stage,
                             std::shared_ptr<const GpuNeutralDisplayQualificationReport> report,
                             render::GpuNeutralDisplayReadback&& readback) noexcept;

namespace detail {

// The single private-storage builder for the resident arm. Declared here (and friended below) so
// only the validating product factory in gpu_resident_preview_product.cpp can construct a resident
// frame; there is deliberately no public unchecked factory that could attach arbitrary provenance
// or a forged report. The factory has already validated the device/registry/report/identity/budget
// and published the lease when it calls this.
[[nodiscard]] std::optional<PreparedPreviewFrame> buildResidentPreviewFrame(
    PreviewRequestIdentity identity, ProcessFrameIdentity processIdentity,
    GpuResidentFrameLease lease,
    std::shared_ptr<const GpuResidentPreviewQualificationReport> qualification,
    std::vector<EvaluatedOperationBounds> bounds) noexcept;

} // namespace detail

// A preview frame reduced to what a viewer actually paints: the packed RGBA8 display buffer, the
// request identity it answers, evaluated geometry, and the process identity that produced it --
// without the Float32 process image (task PERF1, FORMAL AMENDMENT 1).
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

    // Copies `source`'s packed display pixels -- whichever alternative produced them -- and retains
    // its evaluated geometry without the Float32 image. std::nullopt when the source has no valid
    // display buffer, when its packed layout is not the one this storage can hold, or when the copy
    // would exceed the byte limit.
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
    [[nodiscard]] std::span<const EvaluatedOperationBounds> evaluatedBounds() const noexcept {
        return bounds_;
    }
    // The display provider this frame was copied from. Derived from provenance, never independently
    // claimed: the storage type below carries no qualification flag of its own.
    [[nodiscard]] const PreviewDisplayProvenance& provenance() const& noexcept {
        return provenance_;
    }
    [[nodiscard]] const PreviewDisplayProvenance& provenance() const&& = delete;
    [[nodiscard]] bool isOcioQualified() const noexcept {
        return provenance_.provider != PreviewDisplayProvider::CpuReference;
    }
    [[nodiscard]] std::optional<PreviewDisplayBufferView> displayBufferView() const noexcept;
    // What retaining this frame costs: packed display pixels plus evaluated geometry.
    [[nodiscard]] std::size_t displayByteCost() const noexcept;

  private:
    // Only the validating GPU product finalizer may construct this private storage directly; there
    // is deliberately no public unchecked factory that could attach arbitrary provenance.
    friend std::optional<PreparedPreviewFrame>
    makeGpuNeutralDisplayPreview(const PreviewCpuStage& stage,
                                 std::shared_ptr<const GpuNeutralDisplayQualificationReport> report,
                                 render::GpuNeutralDisplayReadback&& readback) noexcept;

    PreviewDisplayOnlyFrame(PreviewRequestIdentity desiredIdentity,
                            ProcessFrameIdentity processIdentity,
                            render::PreparedReferenceDisplayBuffer buffer,
                            PreviewDisplayProvenance provenance,
                            std::vector<EvaluatedOperationBounds> bounds) noexcept;

    PreviewRequestIdentity desiredIdentity_;
    ProcessFrameIdentity processIdentity_;
    // One storage type for all CPU display products: PreviewDisplayBufferView already normalizes
    // them to the same packed shape, so provenance is carried beside the pixels rather than by the
    // pixels' own type.
    render::PreparedReferenceDisplayBuffer buffer_;
    PreviewDisplayProvenance provenance_;
    std::vector<EvaluatedOperationBounds> bounds_;
};

// The fourth, closed display arm: a GPU-resident frame whose pixels never left the device. Its only
// pixel storage is the opaque GpuResidentFrameLease, which owns the native GpuDisplayImage through
// the owner-bound registry -- there is deliberately no native object, Qt type, Vulkan handle, or
// CPU pixel vector here. Geometry (evaluated bounds), the request/process identity, an explicit
// process-origin provenance, and the immutable resident qualification report that admitted the
// route are retained so the frame is self-describing without a readback.
//
// The frame is valid exactly while its lease is alive. Because an invalidated lease must never be
// served, isDisplayValid() is the one validity test a consumer (and the frame cache) uses; it is
// not implied by hasProcessFrame(), which is false here.
class PreviewResidentDisplayFrame final {
  public:
    PreviewResidentDisplayFrame(const PreviewResidentDisplayFrame&) = delete;
    PreviewResidentDisplayFrame& operator=(const PreviewResidentDisplayFrame&) = delete;
    PreviewResidentDisplayFrame(PreviewResidentDisplayFrame&&) noexcept = default;
    PreviewResidentDisplayFrame& operator=(PreviewResidentDisplayFrame&&) = delete;
    ~PreviewResidentDisplayFrame() = default;

    [[nodiscard]] const PreviewRequestIdentity& desiredIdentity() const& noexcept {
        return desiredIdentity_;
    }
    [[nodiscard]] const PreviewRequestIdentity& desiredIdentity() const&& = delete;
    [[nodiscard]] const ProcessFrameIdentity& processIdentity() const& noexcept {
        return processIdentity_;
    }
    [[nodiscard]] const ProcessFrameIdentity& processIdentity() const&& = delete;
    [[nodiscard]] std::span<const EvaluatedOperationBounds> evaluatedBounds() const noexcept {
        return bounds_;
    }

    // The opaque, owner-bound resident storage. Copyable and cheap; never a native object.
    [[nodiscard]] const GpuResidentFrameLease& lease() const& noexcept { return lease_; }
    [[nodiscard]] const GpuResidentFrameLease& lease() const&& = delete;

    // True exactly while the lease still names a live registry entry. An invalidated lease can
    // never be served as a frame.
    [[nodiscard]] bool isDisplayValid() const noexcept { return lease_.isValid(); }

    [[nodiscard]] bool isOcioQualified() const noexcept { return true; }
    [[nodiscard]] PreviewDisplayProvider displayProvider() const noexcept {
        return PreviewDisplayProvider::GpuResident;
    }
    // The honest scene-linear process origin, taken from the retained ProcessFrameIdentity itself.
    // A GPU-evaluated process reports GpuResident here and is never stamped CpuReference.
    [[nodiscard]] EvaluationProvider processProvider() const noexcept {
        return processIdentity_.provider;
    }

    // The immutable qualification report from the real qualifyResidentPreview() run that admitted
    // this exact device/processor. Retained, never copied or reinterpreted.
    [[nodiscard]] const std::shared_ptr<const GpuResidentPreviewQualificationReport>&
    qualification() const& noexcept {
        return qualification_;
    }
    [[nodiscard]] const std::shared_ptr<const GpuResidentPreviewQualificationReport>&
    qualification() const&& = delete;

    // What retaining this frame costs: the ACTUAL native allocation the lease charges (allocator
    // granularity included), plus the retained evaluated geometry and the small identity/report
    // metadata. Unlike the packed CPU arm this is a real GPU-resident allocation, not a host copy.
    [[nodiscard]] std::size_t retainedByteCost() const noexcept;

  private:
    friend std::optional<PreparedPreviewFrame> detail::buildResidentPreviewFrame(
        PreviewRequestIdentity, ProcessFrameIdentity, GpuResidentFrameLease,
        std::shared_ptr<const GpuResidentPreviewQualificationReport>,
        std::vector<EvaluatedOperationBounds>) noexcept;

    PreviewResidentDisplayFrame(
        PreviewRequestIdentity desiredIdentity, ProcessFrameIdentity processIdentity,
        GpuResidentFrameLease lease,
        std::shared_ptr<const GpuResidentPreviewQualificationReport> qualification,
        std::vector<EvaluatedOperationBounds> bounds) noexcept;

    PreviewRequestIdentity desiredIdentity_;
    ProcessFrameIdentity processIdentity_;
    GpuResidentFrameLease lease_;
    std::shared_ptr<const GpuResidentPreviewQualificationReport> qualification_;
    std::vector<EvaluatedOperationBounds> bounds_;
};

// A closed alternative over the production display products (issue #97, task C3, design decision 2)
// plus the retained display-only product and the GPU-resident product above: the temporary built-in
// reference product (ReferenceDisplayFrame), the qualified OCIO product (QualifiedDisplayFrame), a
// PreviewDisplayOnlyFrame, or a PreviewResidentDisplayFrame -- exactly one of the four. A
// display-only frame is itself produced by any of the three CPU/readback PreviewDisplayProvider
// executions; a resident frame reports GpuResident and has no CPU pixels at all. The reference
// alternative's own accessors (displayFrame(), displayIdentity(), displayBuffer()) keep their exact
// pre-existing signatures and behavior for backward compatibility -- calling one of them when
// another alternative is active is a precondition violation (a null-pointer dereference, never a
// thrown exception): no existing or new caller does this, since
// isOcioQualified()/qualifiedDisplayFrame()/displayBufferView()/residentFrame() are how a caller
// that does not already know which alternative it holds finds out.
//
// processFrame() is the one accessor a display-only or resident frame ANSWERS rather than refuses:
// it hands back a null handle, and hasProcessFrame() says so in advance. processImage()
// dereferences it and is therefore a precondition violation on such a frame -- a caller that needs
// scene-linear pixels must evaluate them rather than take them from a cache that deliberately did
// not keep them.
class PreparedPreviewFrame final {
  public:
    [[nodiscard]] static std::optional<PreparedPreviewFrame>
    create(std::uint64_t requestGeneration,
           std::shared_ptr<const ReferenceDisplayFrame> displayFrame,
           PreviewResolutionPolicy resolutionPolicy = PreviewResolutionPolicy::Auto) noexcept;
    [[nodiscard]] static std::optional<PreparedPreviewFrame> createQualified(
        std::uint64_t requestGeneration, std::shared_ptr<const QualifiedDisplayFrame> displayFrame,
        PreviewResolutionPolicy resolutionPolicy = PreviewResolutionPolicy::Auto) noexcept;
    // Re-stamps a retained display-only frame with the generation of the request it now answers.
    [[nodiscard]] static std::optional<PreparedPreviewFrame>
    createDisplayOnly(std::uint64_t requestGeneration,
                      std::shared_ptr<const PreviewDisplayOnlyFrame> displayFrame) noexcept;
    // Re-stamps a retained resident frame with the generation of the request it now answers. The
    // lease is shared by pointer, so this copies no pixels and no native allocation.
    [[nodiscard]] static std::optional<PreparedPreviewFrame>
    createResident(std::uint64_t requestGeneration,
                   std::shared_ptr<const PreviewResidentDisplayFrame> displayFrame) noexcept;

    [[nodiscard]] const PreviewRequestIdentity& desiredIdentity() const& noexcept {
        return desiredIdentity_;
    }
    [[nodiscard]] const PreviewRequestIdentity& desiredIdentity() const&& = delete;

    // True exactly when this frame's pixels came from the qualified transform. The reference
    // alternative always reports false here, never silently relabeled (design decision 2); a
    // display-only frame reports what the product it was copied from reported; a resident frame is
    // always qualified.
    [[nodiscard]] bool isOcioQualified() const noexcept;

    // Which execution produced this frame's display pixels, with the GPU qualification report
    // retained for a GpuNeutral display-only frame. CPU reference/qualified frames report their
    // provider with no report; a resident frame reports GpuResident with its resident report held
    // on PreviewResidentDisplayFrame. No GPU report is ever fabricated for a CPU frame.
    [[nodiscard]] PreviewDisplayProvenance provenance() const noexcept;

    // False exactly for a display-only or resident frame: scene-linear pixels were deliberately not
    // retained, so processFrame() is null and processImage() must not be called.
    [[nodiscard]] bool hasProcessFrame() const noexcept;
    [[nodiscard]] std::span<const EvaluatedOperationBounds> evaluatedBounds() const noexcept;

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
    // published CPU frame) OR the active alternative is the resident arm, which has no CPU pixels
    // by construction. Geometry and identity remain available on a resident frame regardless; use
    // residentFrame()/isDisplayValid() rather than inferring from this nullopt.
    [[nodiscard]] std::optional<PreviewDisplayBufferView> displayBufferView() const noexcept;
    [[nodiscard]] std::optional<core::Color4d> displayLinearProbe() const noexcept;

    // The one display validity test that covers all four arms. For the resident arm this is exactly
    // the live lease: an invalidated lease makes this false and must never be served.
    [[nodiscard]] bool isDisplayValid() const noexcept;

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

    // Display-only accessor; precondition mirrors the two above. A resident frame does NOT answer
    // this: hasProcessFrame() is false for both, so callers must branch on residentFrame()/the
    // provenance rather than assume !hasProcessFrame() means "display-only".
    [[nodiscard]] const std::shared_ptr<const PreviewDisplayOnlyFrame>&
    displayOnlyFrame() const& noexcept;
    [[nodiscard]] const std::shared_ptr<const PreviewDisplayOnlyFrame>&
    displayOnlyFrame() const&& = delete;

    // Resident-only accessor; precondition mirrors the two above.
    [[nodiscard]] const std::shared_ptr<const PreviewResidentDisplayFrame>&
    residentFrame() const& noexcept;
    [[nodiscard]] const std::shared_ptr<const PreviewResidentDisplayFrame>&
    residentFrame() const&& = delete;

  private:
    using DisplayFrameVariant = std::variant<std::shared_ptr<const ReferenceDisplayFrame>,
                                             std::shared_ptr<const QualifiedDisplayFrame>,
                                             std::shared_ptr<const PreviewDisplayOnlyFrame>,
                                             std::shared_ptr<const PreviewResidentDisplayFrame>>;

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
