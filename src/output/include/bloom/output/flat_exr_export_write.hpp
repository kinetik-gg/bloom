#pragma once

#include <bloom/core/sha256.hpp>
#include <bloom/output/flat_exr_output_adapter.hpp>

// output_analysis_analyzer.hpp (which declares the real OutputAnalysisReportV1 class) must be
// visible before flat_exr_reopen_verifier.hpp's own header: that F1 header uses
// std::shared_ptr<const OutputAnalysisReportV1> in verify()'s public signature without including
// or forward-declaring it itself (out of scope to change -- "no changes to F1 adapter/verifier").
// A blank-line-separated include block (this codebase's .clang-format uses the default
// IncludeBlocks: Preserve, which sorts alphabetically only WITHIN a block, never across one) keeps
// this header strictly first regardless of clang-format's usual alphabetical reordering.
#include <bloom/output/output_analysis_analyzer.hpp>

#include <bloom/output/flat_exr_reopen_verifier.hpp>
#include <bloom/output/output_analysis_attempt.hpp>
#include <bloom/output/output_export_stage.hpp>
#include <bloom/runtime/cancellation.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>

// Bridges F1's path-based OpenEXR adapter/verifier (bloom/output/flat_exr_output_adapter.hpp,
// bloom/output/flat_exr_reopen_verifier.hpp -- OpenEXR's classic API opens a real filesystem path,
// it has no in-memory-buffer or byte-sink overload) to a caller that ultimately publishes through
// bloom::platform::StagedArtifactLease, whose own exclusive staged file is reached only through its
// byte-oriented write()/finishWriting()/readForVerification() API and never exposes a filesystem
// path (see the implementor's report's "interpretations/deviations" for the full rationale this
// documents). This writer therefore targets a caller-owned SCRATCH path outside the lease's own
// staging file -- not a second "private staging" artifact in project-format.md's atomic-publication
// sense (design decision 1: "format adapters cannot implement private staging"): this scratch file
// is never published, never atomically replaces anything, and is not tracked by
// StagedArtifactCoordinator at all. The caller (bloom::host::executeExportPublication) streams its
// bytes into the real lease afterward and discards it. Composes ONLY F1's existing public writer/
// verifier -- no OpenEXR/Imath type appears here either.
namespace bloom::output {

enum class FlatExrExportWriteStatusV1 : std::uint8_t {
    Written,
    Cancelled,
    Failed,
};

enum class FlatExrExportWriteErrorCodeV1 : std::uint8_t {
    None,
    WriteFailed,
    VerifyFailed,
    ArtifactHashFailed,
    InternalInvariant,
};

class [[nodiscard]] FlatExrExportWriteResultV1 final {
  public:
    [[nodiscard]] static FlatExrExportWriteResultV1
    written(core::Sha256Digest semanticDigest, core::Sha256Digest artifactDigest,
            std::uint64_t artifactByteCount) noexcept;
    [[nodiscard]] static FlatExrExportWriteResultV1 cancelled() noexcept;
    [[nodiscard]] static FlatExrExportWriteResultV1
    failed(FlatExrExportWriteErrorCodeV1 error, FlatExrWriteErrorCodeV1 writeError = {},
           const FlatExrVerifyDiagnosticV1& verifyDiagnostic = {}) noexcept;

    [[nodiscard]] FlatExrExportWriteStatusV1 status() const noexcept { return status_; }
    [[nodiscard]] FlatExrExportWriteErrorCodeV1 error() const noexcept { return error_; }
    [[nodiscard]] FlatExrWriteErrorCodeV1 writeError() const noexcept { return writeError_; }
    [[nodiscard]] const FlatExrVerifyDiagnosticV1& verifyDiagnostic() const& noexcept {
        return verifyDiagnostic_;
    }
    [[nodiscard]] const FlatExrVerifyDiagnosticV1& verifyDiagnostic() const&& = delete;
    // Valid only when status() is Written: the kind-2 OutputSemanticIdentity digest F1's verifier
    // issued from the DECODED reopened scratch file, and the artifact SHA-256 of the scratch file's
    // complete raw bytes (frame-output.md "Atomic Publication" step 5: "compute the artifact
    // SHA-256"), plus its byte count.
    [[nodiscard]] const core::Sha256Digest& semanticDigest() const& noexcept {
        return semanticDigest_;
    }
    [[nodiscard]] const core::Sha256Digest& semanticDigest() const&& = delete;
    [[nodiscard]] const core::Sha256Digest& artifactDigest() const& noexcept {
        return artifactDigest_;
    }
    [[nodiscard]] const core::Sha256Digest& artifactDigest() const&& = delete;
    [[nodiscard]] std::uint64_t artifactByteCount() const noexcept { return artifactByteCount_; }

  private:
    FlatExrExportWriteResultV1() = default;

    FlatExrExportWriteStatusV1 status_ = FlatExrExportWriteStatusV1::Failed;
    FlatExrExportWriteErrorCodeV1 error_ = FlatExrExportWriteErrorCodeV1::InternalInvariant;
    FlatExrWriteErrorCodeV1 writeError_ = FlatExrWriteErrorCodeV1::None;
    FlatExrVerifyDiagnosticV1 verifyDiagnostic_;
    core::Sha256Digest semanticDigest_;
    core::Sha256Digest artifactDigest_;
    std::uint64_t artifactByteCount_ = 0;
};

class FlatExrExportWriterV1 final {
  public:
    // Writes `attempt.frame()` to `scratchPath` (F1's writer -- progress-stage `Writing`), reopens
    // and semantically verifies it against `attempt.processIdentity()`/`attempt.report()` (F1's
    // verifier -- progress-stage `Verifying`), then streams `scratchPath`'s raw bytes back in
    // `kOutputAdapterMaximumStreamingChunkBytesV1`-bounded chunks to compute the artifact SHA-256,
    // checking `cancellation` between chunks at every stage. `scratchPath` must not already exist;
    // this call creates it. On any non-Written outcome the caller owns removing whatever partial
    // bytes exist at `scratchPath` (mirroring FlatExrWriteResultV1::destinationRemoved()'s own
    // caller-visible contract) -- this class does not delete the caller's scratch path itself,
    // consistent with the caller (not this bridge) owning the scratch directory's lifetime.
    [[nodiscard]] FlatExrExportWriteResultV1
    run(const OutputAnalysisAttemptV1& attempt, const std::filesystem::path& scratchPath,
        const runtime::CancellationToken& cancellation,
        const OutputExportProgressCallbackV1& progress = {}) const noexcept;
};

} // namespace bloom::output
