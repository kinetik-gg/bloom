#pragma once

#include <bloom/core/sha256.hpp>

// output_analysis_analyzer.hpp (which declares the real OutputAnalysisReportV1 class) must be
// visible before png_reopen_verifier.hpp's own header: that G1 header uses
// std::shared_ptr<const OutputAnalysisReportV1> in verify()'s public signature without including or
// forward-declaring it itself (out of scope to change -- "consume G1's API as-is"). A blank-line-
// separated include block (this codebase's .clang-format uses the default IncludeBlocks: Preserve,
// which sorts alphabetically only WITHIN a block, never across one) keeps this header strictly
// first regardless of clang-format's usual alphabetical reordering. Identical arrangement to
// flat_exr_export_write.hpp's own leading block, for the identical reason.
#include <bloom/output/output_analysis_analyzer.hpp>

#include <bloom/output/output_analysis_attempt.hpp>
#include <bloom/output/output_export_stage.hpp>
#include <bloom/output/output_limits.hpp>
#include <bloom/output/png_output_adapter.hpp>
#include <bloom/output/png_reopen_verifier.hpp>
#include <bloom/runtime/cancellation.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>

// The PNG counterpart of flat_exr_export_write.hpp, and the one place the approved export job's
// PNG-specific nodes live (docs/architecture/frame-output.md, "Non-Blocking Execution", node 2:
// "For PNG, dependent output preparation applies the retained qualified built-in processor on CPU
// in bounded chunks ... then produces one immutable prepared display/output frame"). It composes,
// in the doc's own ordered stage vocabulary:
//
//   ColorPreparing  -- bloom::runtime::CpuQualifiedDisplayPreparer over the attempt's RETAINED
//                      qualified processor handle, which itself drives C2's chunked
//                      color::produceBloomNeutralDisplayFrame/applyBloomNeutralDisplayChunk pair
//                      with cancellation checked between chunks. This reuses the established
//                      qualified-display staging seam rather than calling bloom_color_ocio a
//                      second, differently-bounded way.
//   PreparingOutput -- the two-field aggregate construction png_output_adapter.hpp's own
//                      PngRgba8SrgbPreparedStreamV1 comment predicted: dimensions + pixels off the
//                      prepared display frame, with no reshaping, copy, or stride translation.
//   Writing         -- G1's PngRgba8SrgbWriterV1.
//   Verifying       -- G1's PngRgba8SrgbReopenVerifierV1, fed the attempt's retained
//                      processIdentity, report, expected OCIO revision, and canonical
//                      DisplayProcessorIdentity (all four of verify()'s existing parameters).
//   Publishing      -- the shared bounded streaming artifact SHA-256.
//
// Like its flat OpenEXR sibling this writes to a caller-owned SCRATCH path outside the publication
// lease's own staging file (G1's writer/verifier are path-based; StagedArtifactLease exposes only a
// byte-oriented API and never a filesystem path). The scratch file is never published, never
// atomically replaces anything, and is not tracked by StagedArtifactCoordinator; the caller
// (bloom::host::executeExportPublication) streams its bytes into the real lease afterward and
// discards it. No zlib or OpenColorIO type appears here.
namespace bloom::output {

enum class PngExportWriteStatusV1 : std::uint8_t {
    Written,
    Cancelled,
    Failed,
};

enum class PngExportWriteErrorCodeV1 : std::uint8_t {
    None,
    // The attempt is not a PNG attempt, or its frame/process identity/report is missing.
    InvalidAttempt,
    // A PNG attempt that reached this job without the retained qualified display-processor handle,
    // canonical identity, and expected OCIO revision the preset's writer/verifier both require.
    // Unreachable for an APPROVED export (an approvable PNG report exists only when those products
    // do), but typed rather than assumed.
    MissingDisplayProducts,
    // The prepared straight-RGBA8 stream would exceed the effective retained-prepared-PNG-bytes
    // limit (output_limits.hpp's kOutputExportPreparedPngBytesMaximumV1, or the lower limit this
    // call was given). Nothing is allocated for the prepared stream and no file is created.
    PreparedBytesLimitExceeded,
    // The qualified display processor could not be applied to the retained process frame (a
    // non-finite sample, an unusable floating-point environment, an allocation failure, or an
    // incompatible descriptor).
    ColorPrepareFailed,
    WriteFailed,
    VerifyFailed,
    ArtifactHashFailed,
    InternalInvariant,
};

class [[nodiscard]] PngExportWriteResultV1 final {
  public:
    [[nodiscard]] static PngExportWriteResultV1 written(core::Sha256Digest semanticDigest,
                                                        core::Sha256Digest artifactDigest,
                                                        std::uint64_t artifactByteCount,
                                                        std::uint64_t preparedByteCount) noexcept;
    [[nodiscard]] static PngExportWriteResultV1 cancelled() noexcept;
    [[nodiscard]] static PngExportWriteResultV1
    failed(PngExportWriteErrorCodeV1 error, PngWriteErrorCodeV1 writeError = {},
           const PngVerifyDiagnosticV1& verifyDiagnostic = {}) noexcept;

    [[nodiscard]] PngExportWriteStatusV1 status() const noexcept { return status_; }
    [[nodiscard]] PngExportWriteErrorCodeV1 error() const noexcept { return error_; }
    [[nodiscard]] PngWriteErrorCodeV1 writeError() const noexcept { return writeError_; }
    [[nodiscard]] const PngVerifyDiagnosticV1& verifyDiagnostic() const& noexcept {
        return verifyDiagnostic_;
    }
    [[nodiscard]] const PngVerifyDiagnosticV1& verifyDiagnostic() const&& = delete;
    // Valid only when status() is Written: the kind-1 OutputSemanticIdentity digest G1's verifier
    // issued from the DECODED reopened scratch file, and the artifact SHA-256 of the scratch file's
    // complete raw bytes, plus its byte count.
    [[nodiscard]] const core::Sha256Digest& semanticDigest() const& noexcept {
        return semanticDigest_;
    }
    [[nodiscard]] const core::Sha256Digest& semanticDigest() const&& = delete;
    [[nodiscard]] const core::Sha256Digest& artifactDigest() const& noexcept {
        return artifactDigest_;
    }
    [[nodiscard]] const core::Sha256Digest& artifactDigest() const&& = delete;
    [[nodiscard]] std::uint64_t artifactByteCount() const noexcept { return artifactByteCount_; }
    // The retained prepared straight-RGBA8 byte count this run actually charged. Valid only when
    // status() is Written.
    [[nodiscard]] std::uint64_t preparedByteCount() const noexcept { return preparedByteCount_; }

  private:
    PngExportWriteResultV1() = default;

    PngExportWriteStatusV1 status_ = PngExportWriteStatusV1::Failed;
    PngExportWriteErrorCodeV1 error_ = PngExportWriteErrorCodeV1::InternalInvariant;
    PngWriteErrorCodeV1 writeError_ = PngWriteErrorCodeV1::None;
    PngVerifyDiagnosticV1 verifyDiagnostic_;
    core::Sha256Digest semanticDigest_;
    core::Sha256Digest artifactDigest_;
    std::uint64_t artifactByteCount_ = 0;
    std::uint64_t preparedByteCount_ = 0;
};

// The checked byte count of the prepared straight-RGBA8 stream a PNG export of `attempt` would
// retain (width * height * 4 over the retained process frame's data window). Returns nullopt when
// the attempt carries no valid frame descriptor or the arithmetic does not fit. Callers use this
// BEFORE any allocation or file creation, both to expand the job's resource reservation and to
// reject an over-limit request (frame-output.md: "An insufficient or overflowing reservation
// rejects the stage before allocation or file creation").
[[nodiscard]] std::optional<std::uint64_t>
checkedPngPreparedByteCountV1(const OutputAnalysisAttemptV1& attempt) noexcept;

class PngExportWriterV1 final {
  public:
    // Runs ColorPreparing -> PreparingOutput -> Writing -> Verifying -> the artifact hash over
    // `attempt`'s retained products, staging into `scratchPath` (which must not already exist;
    // this call creates it). On any non-Written outcome the caller owns removing whatever partial
    // bytes exist at `scratchPath`, exactly like FlatExrExportWriterV1::run().
    //
    // `preparedByteLimit` is the effective retained-prepared-PNG-bytes ceiling. It is clamped down
    // to kOutputExportPreparedPngBytesMaximumV1 and can never raise it -- frame-output.md's
    // "Version 1 export limits are closed; a request may lower but not raise them".
    [[nodiscard]] PngExportWriteResultV1
    run(const OutputAnalysisAttemptV1& attempt, const std::filesystem::path& scratchPath,
        const runtime::CancellationToken& cancellation,
        const OutputExportProgressCallbackV1& progress = {},
        std::uint64_t preparedByteLimit = kOutputExportPreparedPngBytesMaximumV1) const noexcept;
};

} // namespace bloom::output
