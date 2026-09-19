#pragma once

// Bounded runtime qualification for the already-implemented RESIDENT GPU preview route:
// upload -> (Solid / CoveredSolid) -> (translation+opacity / source-over) -> resident Bloom
// Neutral RGBA8 display, read back only inside this qualification.
//
// This is deliberately narrow. It creates no service, scheduler, frame product, or UI activation,
// and it never relabels GpuDevice's bootstrap capability report. It is the one blocking step a
// future GPU service runs on the device/pipeline owner thread to decide whether the full resident
// preview route is eligible for a bounded interactive pixel interval. It deliberately does NOT
// reuse the older PACKED READBACK display qualification (GpuNeutralDisplay): that report covers a
// different, non-resident operation and cannot qualify this route.
//
// A Ready device or a constructed pipeline is NOT qualification. Only the private report factory
// inside qualifyResidentPreview() may produce a report, so later code cannot fabricate success.
//
// Identity: the capability report's generation is always 1 for every device, so it cannot tell two
// instances apart. This report therefore pins the device's unique ownershipEpoch() and its full
// identity, and eligibleFor() rejects any other device, including a second device on the same
// physical GPU. A non-default processor is rejected, never substituted.
//
// Numeric contract: resident display RGB within one straight-RGBA8 code of the independent CPU
// OCIO oracle and alpha exact (PreviewOnly, final output unchanged); translation/opacity and
// source-over within the documented per-finite-component 2e-6 absolute-or-relative gate against the
// existing CPU primitives and never bit-exact. Nonzero subnormal input is shader-rejected
// whole-frame and remains a per-frame CPU fallback; it is not a parity failure and never a
// successfully published frame.
//
// Threading: qualification must run on the GpuDevice owner thread. Native begin/poll/readback fail
// closed from another thread. On cancellation, a bounded per-dispatch deadline, or any budget
// refusal the caller must destroy or drain the failed/cancelled pipeline on the owner thread before
// reusing it; this function never leaves a partial frame published.

#include <bloom/color/ocio_cpu_display_frame.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/gpu_composite.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image_upload.hpp>
#include <bloom/render/gpu_resident_display.hpp>
#include <bloom/render/gpu_solid.hpp>
#include <bloom/render/image_types.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::runtime {

// --- Pinned identities and numeric limits -----------------------------------------------------
//
// The exact SPIR-V digests the resident route embeds. `source_sha256` pins the checked-in .comp;
// `spv_sha256` pins the embedded array. The native proof re-hashes every embedded array against
// these before the report is trusted; the report carries the same pins so a consumer can detect a
// substitute.
struct GpuResidentPreviewShaderPin final {
    std::string_view name;
    std::string_view spv_sha256;
    std::string_view source_sha256;

    friend constexpr bool operator==(const GpuResidentPreviewShaderPin&,
                                     const GpuResidentPreviewShaderPin&) noexcept = default;
};

inline constexpr std::array<GpuResidentPreviewShaderPin, 5> kGpuResidentPreviewShaderPins{{
    {"solid.comp", "2fae4aef67267e98a53034448639bfb00a69568ec3be1d8ed1a1bc60b31a0318",
     "ac0d5f31bc8b23e2c3744f3edd1ceb2308566d853cb36aea1bffbe5f009a110c"},
    {"solid_covered.comp", "3d54bcb0b9394b381df9f271cdfd05af4cb2f80620ba142227fec53ba26e1a9b",
     "0675f8a7c1bdcafe68ffc45f21cc4b9437251d18f8e7652cc39f52bcc486947b"},
    {"translation_opacity.comp", "0049b132bf98214375023a82472f2839a0e8f30e1b1209a0f214ef283b97c3b8",
     "b92912217b61368c8b65cfda59620d9aad1a4fec5d6d9a31ce843982ee057650"},
    {"source_over.comp", "2aea19b4e3620e9e99f09f7f0e22f97822119ea798b44a1077179c4fc58194ee",
     "217423fab13d2fae83bc61f00ed37803cc23e7284df17bebee1a8cdf7e2e017f"},
    {"neutral_display.comp", "fc0a97d36c42aafd3cfce56dcfdd7ff3c75d3ec7fd0e52a83c7f272c54feb3f9",
     "63fa4699a9a6980312d34ad7c0c7f6ad5d20b7c249b0e050942a50baac367057"},
}};

// The CPU primitive semantics version the resident operations reproduce, plus the covered-solid and
// resident-display dispatch semantics revisions this proof qualified. Pins the actual compiled
// behavior, not a marketing revision.
inline constexpr std::string_view kGpuResidentPreviewCoveredSemantics =
    "covered-palette-opacity-v1";
inline constexpr std::string_view kGpuResidentPreviewDispatchSemantics =
    "resident-neutral-1d-pixelcount-dispatch-status-zeroed-v1";

inline constexpr std::uint32_t kGpuResidentPreviewDisplayRgbToleranceCodes = 1;
inline constexpr std::uint32_t kGpuResidentPreviewDisplayAlphaToleranceCodes = 0;
inline constexpr double kGpuResidentPreviewCompositeAbsoluteOrRelative = 2e-6;
inline constexpr std::size_t kGpuResidentPreviewCpuChunkPixelCount = 65536;
inline constexpr std::string_view kGpuResidentPreviewNumericContract =
    "resident-display rgb<=1-code alpha-exact preview-only; composite abs-or-rel 2e-6";

// The exact expected processor identity, reused from the canonical qualification. Any other
// config revision, cache ID, OCIO version, display/view, source, context, look, or packing fails
// eligibility rather than being substituted. This is the same canonical identity codec invoked
// directly; it is not a second codec.
[[nodiscard]] bool
gpuResidentPreviewIdentityIsEligible(const color::DisplayProcessorIdentityV1& identity,
                                     std::string& reason) noexcept;
[[nodiscard]] bool
gpuResidentPreviewProcessorIsEligible(const color::PreparedCpuDisplayProcessorHandle& processor,
                                      std::string& reason) noexcept;

// --- Report vocabulary ------------------------------------------------------------------------

enum class GpuResidentPreviewOutcome : std::uint8_t {
    // Any required operation failed parity, a native call failed, the identity/device/budget/
    // deadline/cancellation gate refused, or (with parity holding) no faster interval was measured.
    Unavailable,
    // Every required operation passed parity within the documented contract. Final output is
    // unchanged and stays on CPU; the eligible interval is populated only when the timing gate also
    // passed. There is intentionally no ReferenceParity arm.
    PreviewOnly,
};

enum class GpuResidentPreviewDiagnosticCode : std::uint8_t {
    None,
    NotEligibleProcessor,
    NotEligibleDevice,
    WrongThread,
    DeviceUnavailable,
    FixtureDigestMismatch,
    Cancelled,
    ParityFailure,
    NativeFailure,
    NativeTimeout,
    CpuOracleFailure,
    OverBudget,
    TimingNotImproved,
    InternalInvariant,
};

struct GpuResidentPreviewDiagnostic final {
    GpuResidentPreviewDiagnosticCode code = GpuResidentPreviewDiagnosticCode::None;
    std::string message;

    friend bool operator==(const GpuResidentPreviewDiagnostic&,
                           const GpuResidentPreviewDiagnostic&) = default;
};

// The already-created pipelines the caller owns on the device owner thread. Every non-null member
// must be bound to the same device; a null or foreign member is NotEligibleDevice and no native
// call is made.
struct GpuResidentPreviewPipelines final {
    render::GpuSolid* solid = nullptr;
    render::GpuImageUpload* upload = nullptr;
    render::GpuComposite* composite = nullptr;
    render::GpuResidentDisplay* display = nullptr;
};

struct GpuResidentPreviewBudgets final {
    std::uint64_t maxImageBytes = 256ULL * 1024ULL * 1024ULL;
    std::uint64_t maxMetadataBytes = 16ULL * 1024ULL * 1024ULL;
    // Bounded wall-clock deadline for one begin->poll native dispatch. The resident display reads
    // back only inside this qualification.
    std::uint64_t perDispatchDeadlineNanoseconds = 5ULL * 1000ULL * 1000ULL * 1000ULL;
};

// One measured size. `native_full_ms` is the full resident interval: upload begin->ready, then
// resident display begin->fence (no normal readback). `cpu_full_ms` is exactly the CPU OCIO
// produceBloomNeutralDisplayFrame call. These are qualification timings, not application FPS.
struct GpuResidentPreviewTimingSample final {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t pixel_count = 0;
    double native_full_ms = 0.0;
    double cpu_full_ms = 0.0;
    bool native_improved = false;

    friend bool operator==(const GpuResidentPreviewTimingSample&,
                           const GpuResidentPreviewTimingSample&) = default;
};

// The conservative pixel interval for which this profile measured the resident route faster: from
// the lowest measured size of the contiguous faster suffix at the largest measured size through 4K.
// Empty when the largest measured size is not faster. Set from measured timings only; no vendor
// threshold, and never inherited from the packed-display upload/readback break-even.
struct GpuResidentPreviewEligibleInterval final {
    std::uint64_t min_pixels = 0;
    std::uint64_t max_pixels = 0;

    friend bool operator==(const GpuResidentPreviewEligibleInterval&,
                           const GpuResidentPreviewEligibleInterval&) noexcept = default;
};

class GpuResidentPreviewQualificationReport;
[[nodiscard]] GpuResidentPreviewQualificationReport
qualifyResidentPreview(const color::PreparedCpuDisplayProcessorHandle& processor,
                       render::GpuDevice& device, const GpuResidentPreviewPipelines& pipelines,
                       const GpuResidentPreviewBudgets& budgets = {},
                       color::CancellationPredicateRef isCancelled = {}) noexcept;

// Immutable, move-only qualification product. Construction is private: only
// qualifyResidentPreview() may build one, so no later code can forge a successful outcome.
class [[nodiscard]] GpuResidentPreviewQualificationReport final {
  public:
    GpuResidentPreviewQualificationReport(GpuResidentPreviewQualificationReport&&) noexcept;
    GpuResidentPreviewQualificationReport&
    operator=(GpuResidentPreviewQualificationReport&&) noexcept;
    GpuResidentPreviewQualificationReport(const GpuResidentPreviewQualificationReport&) = delete;
    GpuResidentPreviewQualificationReport&
    operator=(const GpuResidentPreviewQualificationReport&) = delete;
    ~GpuResidentPreviewQualificationReport() = default;

    [[nodiscard]] GpuResidentPreviewOutcome outcome() const noexcept { return outcome_; }
    [[nodiscard]] bool eligible() const noexcept {
        return outcome_ == GpuResidentPreviewOutcome::PreviewOnly && eligibleInterval_.has_value();
    }
    [[nodiscard]] const GpuResidentPreviewDiagnostic& diagnostic() const& noexcept {
        return diagnostic_;
    }
    [[nodiscard]] const GpuResidentPreviewDiagnostic& diagnostic() const&& = delete;

    [[nodiscard]] std::uint32_t deviceGeneration() const noexcept { return deviceGeneration_; }
    // Unique ownership identity of the exact device this report qualified. Zero only on a failure
    // report from a null/moved-from device.
    [[nodiscard]] std::uint64_t ownershipEpoch() const noexcept { return ownershipEpoch_; }
    [[nodiscard]] const render::GpuDeviceIdentity& deviceIdentity() const& noexcept {
        return deviceIdentity_;
    }
    [[nodiscard]] const render::GpuDeviceIdentity& deviceIdentity() const&& = delete;

    [[nodiscard]] const std::array<GpuResidentPreviewShaderPin, 5>& shaderPins() const& noexcept {
        return shaderPins_;
    }
    [[nodiscard]] const std::array<GpuResidentPreviewShaderPin, 5>& shaderPins() const&& = delete;
    [[nodiscard]] std::uint32_t primitiveSemanticsVersion() const noexcept {
        return primitiveSemanticsVersion_;
    }
    [[nodiscard]] const std::string& coveredSemantics() const& noexcept {
        return coveredSemantics_;
    }
    [[nodiscard]] const std::string& coveredSemantics() const&& = delete;
    [[nodiscard]] const std::string& dispatchSemantics() const& noexcept {
        return dispatchSemantics_;
    }
    [[nodiscard]] const std::string& dispatchSemantics() const&& = delete;

    [[nodiscard]] const core::Sha256Digest& fixtureDigest() const& noexcept {
        return fixtureDigest_;
    }
    [[nodiscard]] const core::Sha256Digest& fixtureDigest() const&& = delete;
    [[nodiscard]] const std::string& processorCacheId() const& noexcept {
        return processorCacheId_;
    }
    [[nodiscard]] const std::string& processorCacheId() const&& = delete;
    // SHA-256 over the processor's own canonical identity bytes.
    [[nodiscard]] const core::Sha256Digest& processorIdentityDigest() const& noexcept {
        return processorIdentityDigest_;
    }
    [[nodiscard]] const core::Sha256Digest& processorIdentityDigest() const&& = delete;
    [[nodiscard]] const std::string& numericContract() const& noexcept { return numericContract_; }
    [[nodiscard]] const std::string& numericContract() const&& = delete;

    [[nodiscard]] const std::vector<GpuResidentPreviewTimingSample>& timings() const& noexcept {
        return timings_;
    }
    [[nodiscard]] const std::vector<GpuResidentPreviewTimingSample>& timings() const&& = delete;
    [[nodiscard]] const std::optional<GpuResidentPreviewEligibleInterval>&
    eligibleInterval() const& noexcept {
        return eligibleInterval_;
    }
    [[nodiscard]] const std::optional<GpuResidentPreviewEligibleInterval>&
    eligibleInterval() const&& = delete;

    // Measured fact: a nonzero subnormal source frame was rejected whole-frame by the resident
    // display shader and remains a per-frame CPU fallback. Not a parity failure.
    [[nodiscard]] bool subnormalFrameRejected() const noexcept { return subnormalFrameRejected_; }

    // True only for the exact device and processor this report qualified. Validates Ready state,
    // ownership epoch, generation, identity, and the canonical processor identity; every other
    // device (including a second device on the same physical GPU) or processor is rejected.
    [[nodiscard]] bool
    eligibleFor(const render::GpuDevice& device,
                const color::PreparedCpuDisplayProcessorHandle& processor) const noexcept;

  private:
    friend GpuResidentPreviewQualificationReport
    qualifyResidentPreview(const color::PreparedCpuDisplayProcessorHandle&, render::GpuDevice&,
                           const GpuResidentPreviewPipelines&, const GpuResidentPreviewBudgets&,
                           color::CancellationPredicateRef) noexcept;

    GpuResidentPreviewQualificationReport() = default;

    GpuResidentPreviewOutcome outcome_ = GpuResidentPreviewOutcome::Unavailable;
    GpuResidentPreviewDiagnostic diagnostic_;
    std::uint32_t deviceGeneration_ = 0;
    std::uint64_t ownershipEpoch_ = 0;
    render::GpuDeviceIdentity deviceIdentity_;
    std::array<GpuResidentPreviewShaderPin, 5> shaderPins_ = kGpuResidentPreviewShaderPins;
    std::uint32_t primitiveSemanticsVersion_ = 0;
    std::string coveredSemantics_;
    std::string dispatchSemantics_;
    core::Sha256Digest fixtureDigest_;
    std::string processorCacheId_;
    core::Sha256Digest processorIdentityDigest_;
    std::string numericContract_;
    // Populated only on a successful qualification so a failure report is non-allocating.
    std::vector<GpuResidentPreviewTimingSample> timings_;
    std::optional<GpuResidentPreviewEligibleInterval> eligibleInterval_;
    bool subnormalFrameRejected_ = false;
};

namespace detail {

// Deterministic little-endian SHA-256 over fixture bytes, shared by the fixture translation unit.
[[nodiscard]] core::Sha256Digest
residentPreviewHashBytes(std::span<const std::byte> bytes) noexcept;

// Deterministic non-uniform source pixels: signed/HDR or alpha/transparent patterns. Returns
// nullopt if any generated pixel is invalid, so qualification fails rather than substituting
// malformed data.
[[nodiscard]] std::optional<std::vector<render::Rgba32f>>
makeResidentPreviewNonUniformPixels(std::uint32_t width, std::uint32_t height, bool pattern_b);

// Deterministic measured-size source pixels used by the timing pass.
[[nodiscard]] std::optional<std::vector<render::Rgba32f>>
makeResidentPreviewMeasuredPixels(std::uint32_t width, std::uint32_t height);

// Deterministic signed/HDR checker source used by translation and source-over parity.
[[nodiscard]] std::optional<std::vector<render::Rgba32f>>
makeResidentPreviewCheckerPixels(std::uint32_t width, std::uint32_t height);

// Deterministic semantic foreground with distinct RGBA lanes for source-over ordering.
[[nodiscard]] std::optional<std::vector<render::Rgba32f>>
makeResidentPreviewSemanticPixels(std::uint32_t width, std::uint32_t height);

} // namespace detail

} // namespace bloom::runtime
