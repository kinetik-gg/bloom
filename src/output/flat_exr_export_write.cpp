#include <bloom/output/flat_exr_export_write.hpp>

#include <bloom/output/output_limits.hpp>

#include <array>
#include <fstream>
#include <vector>

namespace bloom::output {

FlatExrExportWriteResultV1
FlatExrExportWriteResultV1::written(const core::Sha256Digest semanticDigest,
                                    const core::Sha256Digest artifactDigest,
                                    const std::uint64_t artifactByteCount) noexcept {
    FlatExrExportWriteResultV1 result;
    result.status_ = FlatExrExportWriteStatusV1::Written;
    result.error_ = FlatExrExportWriteErrorCodeV1::None;
    result.semanticDigest_ = semanticDigest;
    result.artifactDigest_ = artifactDigest;
    result.artifactByteCount_ = artifactByteCount;
    return result;
}

FlatExrExportWriteResultV1 FlatExrExportWriteResultV1::cancelled() noexcept {
    FlatExrExportWriteResultV1 result;
    result.status_ = FlatExrExportWriteStatusV1::Cancelled;
    result.error_ = FlatExrExportWriteErrorCodeV1::None;
    return result;
}

FlatExrExportWriteResultV1
FlatExrExportWriteResultV1::failed(const FlatExrExportWriteErrorCodeV1 error,
                                   const FlatExrWriteErrorCodeV1 writeError,
                                   const FlatExrVerifyDiagnosticV1& verifyDiagnostic) noexcept {
    FlatExrExportWriteResultV1 result;
    result.status_ = FlatExrExportWriteStatusV1::Failed;
    result.error_ = error == FlatExrExportWriteErrorCodeV1::None
                        ? FlatExrExportWriteErrorCodeV1::InternalInvariant
                        : error;
    result.writeError_ = writeError;
    result.verifyDiagnostic_ = verifyDiagnostic;
    return result;
}

namespace {

void reportProgress(const OutputExportProgressCallbackV1& callback,
                    const OutputExportProgressV1& progress) noexcept {
    if (!callback) {
        return;
    }
    try {
        callback(progress);
    } catch (...) {
        return;
    }
}

// Streams `path`'s complete raw bytes in bounded chunks, computing their SHA-256. Returns
// nullopt on I/O failure, allocation failure, or cancellation.
[[nodiscard]] std::optional<core::Sha256Digest>
hashArtifactFile(const std::filesystem::path& path, const runtime::CancellationToken& cancellation,
                 const OutputExportProgressCallbackV1& progress,
                 std::uint64_t& byteCountOut) noexcept {
    try {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) {
            return std::nullopt;
        }
        std::vector<std::byte> buffer(kOutputAdapterMaximumStreamingChunkBytesV1);
        core::Sha256Hasher hasher;
        std::uint64_t total = 0;
        reportProgress(progress,
                       {.stage = OutputExportStageV1::Publishing, .completed = 0, .total = 0});
        while (stream) {
            if (cancellation.isCancellationRequested()) {
                return std::nullopt;
            }
            stream.read(reinterpret_cast<char*>(buffer.data()),
                        static_cast<std::streamsize>(buffer.size()));
            const auto readCount = static_cast<std::size_t>(stream.gcount());
            if (readCount == 0) {
                break;
            }
            if (!hasher.update(std::span(buffer).first(readCount))) {
                return std::nullopt;
            }
            total += readCount;
            reportProgress(
                progress,
                {.stage = OutputExportStageV1::Publishing, .completed = total, .total = total});
        }
        if (stream.bad()) {
            return std::nullopt;
        }
        byteCountOut = total;
        return hasher.finalize();
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace

FlatExrExportWriteResultV1
FlatExrExportWriterV1::run(const OutputAnalysisAttemptV1& attempt,
                           const std::filesystem::path& scratchPath,
                           const runtime::CancellationToken& cancellation,
                           const OutputExportProgressCallbackV1& progress) const noexcept {
    if (attempt.frame() == nullptr || attempt.processIdentity() == nullptr ||
        attempt.report() == nullptr) {
        return FlatExrExportWriteResultV1::failed(FlatExrExportWriteErrorCodeV1::InternalInvariant);
    }

    const FlatExrRgba32fLinRec709SceneWriterV1 writer;
    const auto writeResult =
        writer.write(*attempt.frame(), scratchPath, cancellation,
                     [&progress](const FlatExrWriteProgressV1& writeProgress) {
                         reportProgress(progress, {.stage = OutputExportStageV1::Writing,
                                                   .completed = writeProgress.completedScanlines,
                                                   .total = writeProgress.totalScanlines});
                     });
    if (writeResult.status() == FlatExrWriteStatusV1::Cancelled) {
        return FlatExrExportWriteResultV1::cancelled();
    }
    if (writeResult.status() != FlatExrWriteStatusV1::Written) {
        return FlatExrExportWriteResultV1::failed(FlatExrExportWriteErrorCodeV1::WriteFailed,
                                                  writeResult.error());
    }

    const FlatExrRgba32fLinRec709SceneReopenVerifierV1 verifier;
    const auto verifyResult = verifier.verify(
        scratchPath, attempt.processIdentity(), attempt.report(), cancellation,
        [&progress](const FlatExrVerifyScanProgressV1& verifyProgress) {
            reportProgress(progress, {.stage = OutputExportStageV1::Verifying,
                                      .completed = verifyProgress.completedScanlines,
                                      .total = verifyProgress.totalScanlines});
        });
    if (verifyResult.status() == FlatExrVerifyStatusV1::Cancelled) {
        return FlatExrExportWriteResultV1::cancelled();
    }
    if (verifyResult.status() != FlatExrVerifyStatusV1::Verified) {
        return FlatExrExportWriteResultV1::failed(FlatExrExportWriteErrorCodeV1::VerifyFailed, {},
                                                  verifyResult.diagnostic());
    }

    std::uint64_t artifactByteCount = 0;
    const auto artifactDigest =
        hashArtifactFile(scratchPath, cancellation, progress, artifactByteCount);
    if (!artifactDigest.has_value()) {
        if (cancellation.isCancellationRequested()) {
            return FlatExrExportWriteResultV1::cancelled();
        }
        return FlatExrExportWriteResultV1::failed(
            FlatExrExportWriteErrorCodeV1::ArtifactHashFailed);
    }

    return FlatExrExportWriteResultV1::written(verifyResult.digest(), *artifactDigest,
                                               artifactByteCount);
}

} // namespace bloom::output
