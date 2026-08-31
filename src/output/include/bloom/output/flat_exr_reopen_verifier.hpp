#pragma once

#include <bloom/core/sha256.hpp>
#include <bloom/output/output_analysis.hpp>
#include <bloom/output/process_frame_semantic_identity.hpp>
#include <bloom/runtime/cancellation.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace bloom::output {

// Reported between bounded scanline-comparison chunks during header/channel/scanline
// verification. Lets a caller observe monotonic verify progress and, in tests, deterministically
// synchronize a mid-verify cancellation instead of racing a background thread against an
// unobserved internal loop.
struct FlatExrVerifyScanProgressV1 final {
    std::uint64_t completedScanlines = 0;
    std::uint64_t totalScanlines = 0;

    friend bool operator==(const FlatExrVerifyScanProgressV1&,
                           const FlatExrVerifyScanProgressV1&) noexcept = default;
};

using FlatExrVerifyScanProgressCallbackV1 = std::function<void(const FlatExrVerifyScanProgressV1&)>;

enum class FlatExrVerifyStatusV1 : std::uint8_t {
    Verified,
    Cancelled,
    Failed,
};

// Every code below is a typed diagnosis of a specific reopen-verification step, in the order
// docs/architecture/frame-output.md's "Flat OpenEXR Preset Version 1" and "Determinism And
// Portable Output Identity" sections require: version field, then the complete attribute
// allowlist with exact types and values, then the channel list, then streamed scanline samples,
// then (only once every prior check passes) kind-2 semantic-identity issuance.
enum class FlatExrVerifyErrorCodeV1 : std::uint8_t {
    None,
    // The staged path could not be opened/read at all before header parsing began.
    SourceUnavailable,
    // Fewer than 8 bytes, or the magic number, is not the exact OpenEXR magic.
    TruncatedFile,
    // The magic number was correct but the version number was not 2 or a feature flag was set
    // (tiled, long-names, non-image/deep, or multi-part).
    InvalidVersionField,
    // OpenEXR's own header/chunk parser rejected the file for a reason not already classified.
    HeaderParseFailed,
    // An attribute name outside the closed ten-entry allowlist is present.
    UnexpectedAttribute,
    // A required attribute from the closed ten-entry allowlist is absent.
    MissingAttribute,
    // A required attribute is present under the right name but the wrong OpenEXR type.
    AttributeTypeMismatch,
    // A required attribute has the right name and type but the wrong exact value.
    AttributeValueMismatch,
    // The channel list is not exactly A, B, G, R FLOAT 1x1 pLinear=0 in that lexical order.
    InvalidChannelList,
    // The decoded data or display window does not exactly equal the source frame's window.
    WindowMismatch,
    // The decoded pixelAspectRatio bits do not equal the source's rounded binary32.
    PixelAspectMismatch,
    // A decoded channel sample's Float32 bits differ from the source frame's bits.
    SampleMismatch,
    // Scanline decoding failed partway through (short read, corrupt compressed chunk).
    ScanlineReadFailed,
    ResourceLimitExceeded,
    // Header/channel/scanline verification passed, but binding or issuing the kind-2
    // `OutputSemanticIdentity` from the decoded values failed -- an input-construction problem
    // (e.g. a null/mismatched `processIdentity`/`report`) rather than a file-content defect.
    IdentityIssuanceFailed,
    AllocationFailure,
    InternalInvariant,
};

// Names the exact location of a Failed diagnosis. Fields are populated only for the codes that
// name a location; an unused field stays at its default.
struct FlatExrVerifyDiagnosticV1 final {
    FlatExrVerifyErrorCodeV1 code = FlatExrVerifyErrorCodeV1::None;
    std::string attributeName;
    std::optional<std::int64_t> scanlineY;
    std::optional<std::uint8_t> channelIndex; // 0 = R, 1 = G, 2 = B, 3 = A

    friend bool operator==(const FlatExrVerifyDiagnosticV1&,
                           const FlatExrVerifyDiagnosticV1&) = default;
};

// `OutputSemanticIdentityV1` itself is module-private (docs/architecture/frame-output.md: "the
// module-private Output Semantic Identity version 1 streaming serializer/preparer"), so this
// public result surfaces only the one thing design decision 5 asks for: the resulting digest.
class [[nodiscard]] FlatExrVerifyResultV1 final {
  public:
    [[nodiscard]] static FlatExrVerifyResultV1 verified(core::Sha256Digest digest) noexcept;
    [[nodiscard]] static FlatExrVerifyResultV1 cancelled() noexcept;
    [[nodiscard]] static FlatExrVerifyResultV1
    failed(FlatExrVerifyDiagnosticV1 diagnostic) noexcept;

    [[nodiscard]] FlatExrVerifyStatusV1 status() const noexcept { return status_; }
    // Valid only when status() is Verified.
    [[nodiscard]] const core::Sha256Digest& digest() const& noexcept { return digest_; }
    [[nodiscard]] const core::Sha256Digest& digest() const&& = delete;
    [[nodiscard]] const FlatExrVerifyDiagnosticV1& diagnostic() const& noexcept {
        return diagnostic_;
    }
    [[nodiscard]] const FlatExrVerifyDiagnosticV1& diagnostic() const&& = delete;

  private:
    FlatExrVerifyResultV1(FlatExrVerifyStatusV1 status, core::Sha256Digest digest,
                          FlatExrVerifyDiagnosticV1 diagnostic) noexcept;

    FlatExrVerifyStatusV1 status_ = FlatExrVerifyStatusV1::Failed;
    core::Sha256Digest digest_{};
    FlatExrVerifyDiagnosticV1 diagnostic_{};
};

// Reopens `stagedPath` (a path/handle a future publication slice will own), proves the closed
// version 1 header/channel/scanline contract against `processIdentity`'s exact retained source
// frame, and -- only when every check passes -- feeds the DECODED reopened values (not the
// writer's inputs) into the existing kind-2 output-semantic-identity serializer, binding them
// through the same module-private seam the pre-approval analysis pipeline already produces
// `processIdentity`/`report` for (`bloom::output::analyzeFlatExrRgba32fLinRec709SceneV1`), and
// surfacing the resulting digest as this result's `digest()`. A Failed or Cancelled result never
// surfaces a digest. Bounded memory (scanline-chunked reads), cancellation-checked between
// chunks. No OpenEXR/Imath type appears in this public header.
class FlatExrRgba32fLinRec709SceneReopenVerifierV1 final {
  public:
    [[nodiscard]] FlatExrVerifyResultV1
    verify(const std::filesystem::path& stagedPath,
           const std::shared_ptr<const ProcessFrameSemanticIdentityV1>& processIdentity,
           const std::shared_ptr<const OutputAnalysisReportV1>& report,
           const runtime::CancellationToken& cancellation,
           const FlatExrVerifyScanProgressCallbackV1& scanProgress = {}) const noexcept;
};

} // namespace bloom::output
