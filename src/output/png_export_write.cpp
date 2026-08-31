#include <bloom/output/png_export_write.hpp>

#include "output_export_artifact_hash.hpp"
#include <bloom/render/image_types.hpp>
#include <bloom/runtime/qualified_display_preparation.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace bloom::output {

PngExportWriteResultV1 PngExportWriteResultV1::written(
    const core::Sha256Digest semanticDigest, const core::Sha256Digest artifactDigest,
    const std::uint64_t artifactByteCount, const std::uint64_t preparedByteCount) noexcept {
    PngExportWriteResultV1 result;
    result.status_ = PngExportWriteStatusV1::Written;
    result.error_ = PngExportWriteErrorCodeV1::None;
    result.semanticDigest_ = semanticDigest;
    result.artifactDigest_ = artifactDigest;
    result.artifactByteCount_ = artifactByteCount;
    result.preparedByteCount_ = preparedByteCount;
    return result;
}

PngExportWriteResultV1 PngExportWriteResultV1::cancelled() noexcept {
    PngExportWriteResultV1 result;
    result.status_ = PngExportWriteStatusV1::Cancelled;
    result.error_ = PngExportWriteErrorCodeV1::None;
    return result;
}

PngExportWriteResultV1
PngExportWriteResultV1::failed(const PngExportWriteErrorCodeV1 error,
                               const PngWriteErrorCodeV1 writeError,
                               const PngVerifyDiagnosticV1& verifyDiagnostic) noexcept {
    PngExportWriteResultV1 result;
    result.status_ = PngExportWriteStatusV1::Failed;
    result.error_ = error == PngExportWriteErrorCodeV1::None
                        ? PngExportWriteErrorCodeV1::InternalInvariant
                        : error;
    result.writeError_ = writeError;
    result.verifyDiagnostic_ = verifyDiagnostic;
    return result;
}

std::optional<std::uint64_t>
checkedPngPreparedByteCountV1(const OutputAnalysisAttemptV1& attempt) noexcept {
    if (attempt.frame() == nullptr) {
        return std::nullopt;
    }
    const auto* descriptor = attempt.frame()->processImage().descriptor();
    if (descriptor == nullptr) {
        return std::nullopt;
    }
    // The same extent, and the same checked RGBA8 layout arithmetic, the ColorPreparing stage
    // itself uses (color::produceBloomNeutralDisplayFrame windows its output to the source
    // process frame's DATA window and sizes it with render::checkedRgba8Layout).
    const auto layout = render::checkedRgba8Layout(descriptor->dataWindow().extent());
    if (!layout) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(layout.value()->pixelStorageBytes);
}

namespace {

[[nodiscard]] PngExportWriteErrorCodeV1 translateColorPrepareFailure(
    const std::vector<runtime::QualifiedDisplayDiagnostic>& diagnostics) noexcept {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.code ==
            runtime::QualifiedDisplayDiagnosticCode::PixelStorageBudgetExceeded) {
            return PngExportWriteErrorCodeV1::PreparedBytesLimitExceeded;
        }
    }
    return PngExportWriteErrorCodeV1::ColorPrepareFailed;
}

} // namespace

PngExportWriteResultV1 PngExportWriterV1::run(
    const OutputAnalysisAttemptV1& attempt, const std::filesystem::path& scratchPath,
    const runtime::CancellationToken& cancellation, const OutputExportProgressCallbackV1& progress,
    const std::uint64_t preparedByteLimit) const noexcept {
    if (attempt.preset() != OutputPresetV1::PngRgba8SrgbV1 || attempt.frame() == nullptr ||
        attempt.processIdentity() == nullptr || attempt.report() == nullptr) {
        return PngExportWriteResultV1::failed(PngExportWriteErrorCodeV1::InvalidAttempt);
    }
    const auto& display = attempt.display();
    if (!display.isPresent()) {
        return PngExportWriteResultV1::failed(PngExportWriteErrorCodeV1::MissingDisplayProducts);
    }
    // isPresent() above already proves this, but it proves it through a member function call
    // clang-tidy's bugprone-unchecked-optional-access cannot follow; taking the value out once,
    // here, with an immediately-adjacent check is this repository's established way to avoid an
    // unprovable-at-a-distance dereference at the use site below.
    core::Sha256Digest expectedOcioRevision;
    if (display.expectedOcioRevision.has_value()) {
        expectedOcioRevision = *display.expectedOcioRevision;
    } else {
        return PngExportWriteResultV1::failed(PngExportWriteErrorCodeV1::MissingDisplayProducts);
    }

    // Closed-limit clamp: a caller may lower the retained-prepared-PNG-bytes ceiling, never raise
    // it. Zero would mean "no prepared stream is representable at all", which is a caller error
    // rather than a resource outcome, so it is rejected as an over-limit request before anything
    // is allocated.
    const auto effectiveLimit = std::min(preparedByteLimit, kOutputExportPreparedPngBytesMaximumV1);
    const auto preparedBytes = checkedPngPreparedByteCountV1(attempt);
    if (!preparedBytes.has_value()) {
        return PngExportWriteResultV1::failed(PngExportWriteErrorCodeV1::InvalidAttempt);
    }
    if (effectiveLimit == 0 || *preparedBytes > effectiveLimit) {
        return PngExportWriteResultV1::failed(
            PngExportWriteErrorCodeV1::PreparedBytesLimitExceeded);
    }

    // --- ColorPreparing. The preparer drives C2's chunked apply and checks `cancellation` between
    // chunks; its own budget check is the second enforcement of the same effective limit checked
    // above (that one rejects before allocation, this one is the allocator's own guard).
    const runtime::CpuQualifiedDisplayPreparer preparer(*display.processor);
    const auto colorResult = preparer.prepare(
        attempt.frame(),
        {.aggregatePixelStorageByteLimit = static_cast<std::size_t>(effectiveLimit),
         .chunkPixelCount = runtime::kDefaultQualifiedDisplayChunkPixelCount},
        cancellation, [&progress](const runtime::QualifiedDisplayProgress& stageProgress) {
            detail::reportExportProgress(progress, {.stage = OutputExportStageV1::ColorPreparing,
                                                    .completed = stageProgress.completed,
                                                    .total = stageProgress.total});
        });
    if (colorResult.status() == runtime::QualifiedDisplayPreparationStatus::Cancelled) {
        return PngExportWriteResultV1::cancelled();
    }
    if (colorResult.status() != runtime::QualifiedDisplayPreparationStatus::Prepared ||
        colorResult.frame() == nullptr) {
        return PngExportWriteResultV1::failed(
            translateColorPrepareFailure(colorResult.diagnostics()));
    }

    // --- PreparingOutput: the two-field aggregate png_output_adapter.hpp predicted.
    // `preparedFrame` (and therefore this span) stays alive for the whole Writing/Verifying
    // sequence below, which is exactly what PngRgba8SrgbPreparedStreamV1's non-owning `pixels`
    // contract requires.
    const auto& preparedFrame = colorResult.frame()->buffer();
    const PngRgba8SrgbPreparedStreamV1 prepared{
        .dimensions = preparedFrame.displayWindow().extent(), .pixels = preparedFrame.pixels()};
    const auto preparedPixelCount = static_cast<std::uint64_t>(preparedFrame.layout().pixelCount);
    detail::reportExportProgress(progress, {.stage = OutputExportStageV1::PreparingOutput,
                                            .completed = preparedPixelCount,
                                            .total = preparedPixelCount});
    if (cancellation.isCancellationRequested()) {
        return PngExportWriteResultV1::cancelled();
    }

    // --- Writing.
    const PngRgba8SrgbWriterV1 writer;
    const auto writeResult = writer.write(
        prepared, scratchPath, cancellation, [&progress](const PngWriteProgressV1& writeProgress) {
            detail::reportExportProgress(progress, {.stage = OutputExportStageV1::Writing,
                                                    .completed = writeProgress.completedRows,
                                                    .total = writeProgress.totalRows});
        });
    if (writeResult.status() == PngWriteStatusV1::Cancelled) {
        return PngExportWriteResultV1::cancelled();
    }
    if (writeResult.status() != PngWriteStatusV1::Written) {
        return PngExportWriteResultV1::failed(PngExportWriteErrorCodeV1::WriteFailed,
                                              writeResult.error());
    }

    // --- Verifying: all four of G1 verify()'s identity-issuance inputs come from the attempt's own
    // retained products; nothing here reconstructs or substitutes one.
    const PngRgba8SrgbReopenVerifierV1 verifier;
    const auto verifyResult = verifier.verify(
        scratchPath, prepared, attempt.processIdentity(), attempt.report(), expectedOcioRevision,
        display.identity, cancellation, [&progress](const PngVerifyProgressV1& verifyProgress) {
            detail::reportExportProgress(progress, {.stage = OutputExportStageV1::Verifying,
                                                    .completed = verifyProgress.completedRows,
                                                    .total = verifyProgress.totalRows});
        });
    if (verifyResult.status() == PngVerifyStatusV1::Cancelled) {
        return PngExportWriteResultV1::cancelled();
    }
    if (verifyResult.status() != PngVerifyStatusV1::Verified) {
        return PngExportWriteResultV1::failed(PngExportWriteErrorCodeV1::VerifyFailed, {},
                                              verifyResult.diagnostic());
    }

    std::uint64_t artifactByteCount = 0;
    const auto artifactDigest =
        detail::hashExportArtifactFile(scratchPath, cancellation, progress, artifactByteCount);
    if (!artifactDigest.has_value()) {
        if (cancellation.isCancellationRequested()) {
            return PngExportWriteResultV1::cancelled();
        }
        return PngExportWriteResultV1::failed(PngExportWriteErrorCodeV1::ArtifactHashFailed);
    }

    return PngExportWriteResultV1::written(verifyResult.digest(), *artifactDigest,
                                           artifactByteCount, *preparedBytes);
}

} // namespace bloom::output
