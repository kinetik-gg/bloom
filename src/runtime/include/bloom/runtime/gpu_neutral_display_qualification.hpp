#pragma once

// Bounded runtime qualification for the already-embedded fixed Bloom Neutral v1 display compute
// operation (operation OcioDisplayV1 / "BloomNeutralV1Display").
//
// This is deliberately narrow: it does not create a GPU service, scheduler, frame product, or UI
// activation. It is the one blocking qualification step a future dedicated GPU service will run on
// the native device/pipeline owner thread to decide whether the embedded operation is eligible for
// a bounded interactive pixel interval. It never relabels the native device's whole capability
// report and grants no other operation any outcome.
//
// A successful pipeline construction or a Ready device is NOT qualification. Only the private
// report factory inside qualifyGpuNeutralDisplay() may produce a report, so later code cannot
// fabricate a successful qualification.
//
// Numeric contract (from the frozen shader manifest): RGB within one straight-RGBA8 code of the
// independent CPU OCIO oracle, alpha exact. The operation is therefore classified PreviewOnly --
// final output continues to use the CPU reference path. Nonzero subnormal input is intentionally
// shader-rejected and remains a per-frame CPU fallback; it is not a parity failure and is not a
// claimed supported domain.
//
// Threading: qualification must run on the thread that owns the GpuDevice and GpuNeutralDisplay
// pipeline (the native owner thread). The native begin()/poll()/readback() calls fail closed from
// another thread; this function does not move that work off the owner thread. On a bounded
// per-dispatch deadline, cancellation, or any native failure the caller must destroy or drain the
// failed/cancelled pipeline on the owner thread before reusing it; this function never leaves a
// partial frame published.

#include <bloom/color/ocio_cpu_display_frame.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_neutral_display.hpp>
#include <bloom/render/image_types.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::runtime {

// The exact pinned identities this operation is allowed to qualify. Any other processor -- a
// different config revision, cache ID, display/view, or OCIO version -- fails eligibility rather
// than being substituted.
inline constexpr core::Sha256Digest kGpuNeutralDisplayShaderDigest =
    core::Sha256Digest::fromBytes({{
        0xfc, 0x0a, 0x97, 0xd3, 0x6c, 0x42, 0xaa, 0xfd, //
        0x3c, 0xfc, 0xe5, 0x6d, 0xcf, 0xdd, 0x7f, 0xf3, //
        0xc7, 0x5d, 0x3e, 0xc7, 0xfd, 0x0e, 0x52, 0xa8, //
        0x3c, 0x7f, 0x27, 0x2c, 0x54, 0xfe, 0xb3, 0xf9, //
    }});
inline constexpr std::string_view kGpuNeutralDisplayProcessorCacheId =
    "9a9c93f1bcaaa90825d44353f45998ac";
inline constexpr std::string_view kGpuNeutralDisplayOcioVersion = "2.5.2";
inline constexpr std::string_view kGpuNeutralDisplayDisplayName = "srgb_rec709_display";
inline constexpr std::string_view kGpuNeutralDisplayViewName = "srgb_rec709_display";
inline constexpr std::uint32_t kGpuNeutralDisplayRgbToleranceCodes = 1;
inline constexpr std::uint32_t kGpuNeutralDisplayAlphaToleranceCodes = 0;
inline constexpr std::size_t kGpuNeutralDisplayCpuChunkPixelCount = 65536;
inline constexpr std::string_view kGpuNeutralDisplayNumericContract =
    "rgb<=1-code alpha-exact preview-only";

// Qualification outcome. There is intentionally no ReferenceParity arm: the frozen shader declares
// one 8-bit RGB code of tolerance, so the honest ceiling for this operation is PreviewOnly.
enum class GpuNeutralDisplayQualificationOutcome : std::uint8_t {
    // Not eligible, parity failed, native failed, or measured slower than CPU at every size.
    Unavailable,
    // Parity passed within the documented contract. Final output is unchanged and stays on CPU; the
    // eligible interval is populated only when the timing gate also passed.
    PreviewOnly,
};

enum class GpuNeutralDisplayQualificationDiagnosticCode : std::uint8_t {
    None,
    NotEligibleProcessor,
    NotEligibleDevice,
    FixtureDigestMismatch,
    Cancelled,
    ParityFailure,
    NativeFailure,
    NativeTimeout,
    CpuOracleFailure,
    TimingNotImproved,
    InternalInvariant,
};

struct GpuNeutralDisplayQualificationDiagnostic final {
    GpuNeutralDisplayQualificationDiagnosticCode code =
        GpuNeutralDisplayQualificationDiagnosticCode::None;
    std::string message;

    friend bool operator==(const GpuNeutralDisplayQualificationDiagnostic&,
                           const GpuNeutralDisplayQualificationDiagnostic&) = default;
};

// One measured size. `native_full_ms` is the full begin->poll->readback interval (source copy,
// upload, fence wait, invalidation, host output allocation and unpack); `cpu_full_ms` is exactly
// the produceBloomNeutralDisplayFrame call including its own output allocation, with no extra
// oracle copy. These are qualification timings, not application frame rates.
struct GpuNeutralDisplayTimingSample final {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t pixel_count = 0;
    double native_full_ms = 0.0;
    double cpu_full_ms = 0.0;
    bool native_improved = false;

    friend bool operator==(const GpuNeutralDisplayTimingSample&,
                           const GpuNeutralDisplayTimingSample&) = default;
};

// The conservative pixel interval for which this profile measured native faster than CPU: from the
// lowest measured size of the contiguous faster suffix at the largest measured size through 4K. If
// the largest measured size itself is not faster, the suffix is empty and the interval is absent
// (CPU-only). Set from measured timings only; no vendor threshold.
struct GpuNeutralDisplayEligibleInterval final {
    std::uint64_t min_pixels = 0;
    std::uint64_t max_pixels = 0;

    friend bool operator==(const GpuNeutralDisplayEligibleInterval&,
                           const GpuNeutralDisplayEligibleInterval&) = default;
};

// Narrow, reusable identity predicate for the fixed Bloom Neutral v1 display operation. It does NOT
// re-implement the canonical identity codec: it builds the exact expected input record (pinned
// revision, empty context, lin_rec709_scene source, the default srgb_rec709_display display/view,
// LookMode::Bypass with empty looks, and the existing constant quality/semantics/packing), writes
// it with the official writeDisplayProcessorIdentityV1 into a small fixed stack buffer, and
// compares the resulting canonical bytes to the record's own canonical bytes. A canonically valid
// but source/context/look/packing-incompatible record therefore fails. No GPU-specific
// serialization.
[[nodiscard]] bool
gpuNeutralDisplayIdentityIsEligible(const color::DisplayProcessorIdentityV1& identity,
                                    std::string& reason) noexcept;

// Narrow, reusable eligibility predicate for the fixed Bloom Neutral v1 display processor. Calls
// the identity predicate above and additionally pins the processor cache ID, OCIO version, and
// default display/view provenance. Any other processor must be rejected, never substituted.
[[nodiscard]] bool
gpuNeutralDisplayProcessorIsEligible(const color::PreparedCpuDisplayProcessorHandle& processor,
                                     std::string& reason) noexcept;

class GpuNeutralDisplayQualificationReport;

// Runs the bounded qualification on the native owner thread. Returns an immutable report. Never
// throws; every failure is a typed diagnostic on the report. The caller keeps ownership of the
// device and pipeline.
[[nodiscard]] GpuNeutralDisplayQualificationReport
qualifyGpuNeutralDisplay(const color::PreparedCpuDisplayProcessorHandle& processor,
                         render::GpuDevice& device, render::GpuNeutralDisplay& pipeline,
                         color::CancellationPredicateRef isCancelled = {}) noexcept;

// Immutable, move-only qualification product. Construction is private: only
// qualifyGpuNeutralDisplay() may build one, so no later code can forge a successful outcome.
class [[nodiscard]] GpuNeutralDisplayQualificationReport final {
  public:
    GpuNeutralDisplayQualificationReport(GpuNeutralDisplayQualificationReport&&) noexcept;
    GpuNeutralDisplayQualificationReport&
    operator=(GpuNeutralDisplayQualificationReport&&) noexcept;
    GpuNeutralDisplayQualificationReport(const GpuNeutralDisplayQualificationReport&) = delete;
    GpuNeutralDisplayQualificationReport&
    operator=(const GpuNeutralDisplayQualificationReport&) = delete;
    ~GpuNeutralDisplayQualificationReport() = default;

    [[nodiscard]] GpuNeutralDisplayQualificationOutcome outcome() const noexcept {
        return outcome_;
    }
    [[nodiscard]] bool eligible() const noexcept {
        return outcome_ == GpuNeutralDisplayQualificationOutcome::PreviewOnly &&
               eligibleInterval_.has_value();
    }
    [[nodiscard]] const GpuNeutralDisplayQualificationDiagnostic& diagnostic() const& noexcept {
        return diagnostic_;
    }
    [[nodiscard]] const GpuNeutralDisplayQualificationDiagnostic& diagnostic() const&& = delete;

    [[nodiscard]] std::uint32_t deviceGeneration() const noexcept { return deviceGeneration_; }
    [[nodiscard]] const render::GpuDeviceIdentity& deviceIdentity() const& noexcept {
        return deviceIdentity_;
    }
    [[nodiscard]] const render::GpuDeviceIdentity& deviceIdentity() const&& = delete;

    [[nodiscard]] const core::Sha256Digest& shaderDigest() const& noexcept { return shaderDigest_; }
    [[nodiscard]] const core::Sha256Digest& shaderDigest() const&& = delete;
    [[nodiscard]] const core::Sha256Digest& configRevision() const& noexcept {
        return configRevision_;
    }
    [[nodiscard]] const core::Sha256Digest& configRevision() const&& = delete;
    [[nodiscard]] const std::string& processorCacheId() const& noexcept {
        return processorCacheId_;
    }
    [[nodiscard]] const std::string& processorCacheId() const&& = delete;
    [[nodiscard]] const core::Sha256Digest& fixtureDigest() const& noexcept {
        return fixtureDigest_;
    }
    [[nodiscard]] const core::Sha256Digest& fixtureDigest() const&& = delete;
    [[nodiscard]] const std::string& numericContract() const& noexcept { return numericContract_; }
    [[nodiscard]] const std::string& numericContract() const&& = delete;

    [[nodiscard]] const std::vector<GpuNeutralDisplayTimingSample>& timings() const& noexcept {
        return timings_;
    }
    [[nodiscard]] const std::vector<GpuNeutralDisplayTimingSample>& timings() const&& = delete;
    [[nodiscard]] const std::optional<GpuNeutralDisplayEligibleInterval>&
    eligibleInterval() const& noexcept {
        return eligibleInterval_;
    }
    [[nodiscard]] const std::optional<GpuNeutralDisplayEligibleInterval>&
    eligibleInterval() const&& = delete;

    // Measured fact: a nonzero subnormal source frame was rejected whole-frame by the shader and
    // remains a per-frame CPU fallback. Not a parity failure.
    [[nodiscard]] bool subnormalFrameRejected() const noexcept { return subnormalFrameRejected_; }

  private:
    friend GpuNeutralDisplayQualificationReport
    qualifyGpuNeutralDisplay(const color::PreparedCpuDisplayProcessorHandle&, render::GpuDevice&,
                             render::GpuNeutralDisplay&, color::CancellationPredicateRef) noexcept;

    GpuNeutralDisplayQualificationReport() = default;

    GpuNeutralDisplayQualificationOutcome outcome_ =
        GpuNeutralDisplayQualificationOutcome::Unavailable;
    GpuNeutralDisplayQualificationDiagnostic diagnostic_;
    std::uint32_t deviceGeneration_ = 0;
    render::GpuDeviceIdentity deviceIdentity_;
    core::Sha256Digest shaderDigest_ = kGpuNeutralDisplayShaderDigest;
    core::Sha256Digest configRevision_;
    std::string processorCacheId_;
    core::Sha256Digest fixtureDigest_;
    // Populated only on a successful qualification so a failure report is non-allocating.
    std::string numericContract_;
    std::vector<GpuNeutralDisplayTimingSample> timings_;
    std::optional<GpuNeutralDisplayEligibleInterval> eligibleInterval_;
    bool subnormalFrameRejected_ = false;
};

namespace detail {

// Implementation detail of the qualification fixtures translation unit. Not part of the public
// contract; callers must not depend on it. Kept here only because a separate fixture translation
// unit needs one shared declaration and the turn scope allows no additional private header.
struct GpuNeutralDisplayFixture final {
    std::string name;
    std::uint32_t width = 1;
    std::uint32_t height = 1;
    std::vector<render::Rgba32f> pixels;
};

// Deterministic parity fixtures: odd 257 tail, alpha endpoints and quantization-adjacent samples,
// signed/HDR and tiny normal values. Nonzero subnormal input is deliberately excluded because the
// shader rejects it whole-frame. Returns nullopt if any authored fixture pixel is invalid, so
// qualification fails rather than silently substituting malformed data.
[[nodiscard]] std::optional<std::vector<GpuNeutralDisplayFixture>>
makeGpuNeutralDisplayParityFixtures();

// SHA-256 over the canonical little-endian serialization of the fixture bytes.
[[nodiscard]] core::Sha256Digest
gpuNeutralDisplayFixtureDigest(const std::vector<GpuNeutralDisplayFixture>& fixtures) noexcept;

// Deterministic source pixels for one measured size, generated the same way the large fixtures are,
// so the timing source is reproducible and independent of any GPU/OCIO path. Returns nullopt on an
// invalid generated pixel.
[[nodiscard]] std::optional<std::vector<render::Rgba32f>>
makeGpuNeutralDisplayMeasuredPixels(std::uint32_t width, std::uint32_t height);

} // namespace detail

} // namespace bloom::runtime
