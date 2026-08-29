#include <bloom/project/staged_copy.hpp>

#include "zip_container_preflight.hpp"

#include <bloom/project/project_io_memory_resource.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory_resource>
#include <new>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace bloom::project {

namespace {

// A strict subset translation, mirroring zip_container.cpp's own private translatePreflightError()
// exactly (that function is anonymous-namespace-local to zip_container.cpp, not exported -- see
// zip_container_preflight.hpp's own header comment: "every value here has an exact translation" in
// the public ZipContainerError enum). Duplicated here rather than exported, matching this
// codebase's existing precedent for small private per-translation-unit mappings.
[[nodiscard]] ZipContainerError
translatePreflightError(const detail::ZipContainerPreflightError error) noexcept {
    switch (error) {
    case detail::ZipContainerPreflightError::None:
        return ZipContainerError::None;
    case detail::ZipContainerPreflightError::InvalidLimits:
        return ZipContainerError::InvalidLimits;
    case detail::ZipContainerPreflightError::ArchiveTooLarge:
        return ZipContainerError::ArchiveTooLarge;
    case detail::ZipContainerPreflightError::ArchiveTruncated:
        return ZipContainerError::ArchiveTruncated;
    case detail::ZipContainerPreflightError::MissingOrMalformedEocd:
        return ZipContainerError::MissingOrMalformedEocd;
    case detail::ZipContainerPreflightError::ArchiveCommentForbidden:
        return ZipContainerError::ArchiveCommentForbidden;
    case detail::ZipContainerPreflightError::MultiDiskForbidden:
        return ZipContainerError::MultiDiskForbidden;
    case detail::ZipContainerPreflightError::WrongEntryCount:
        return ZipContainerError::WrongEntryCount;
    case detail::ZipContainerPreflightError::WrongEntryName:
        return ZipContainerError::WrongEntryName;
    case detail::ZipContainerPreflightError::WrongEntryOrder:
        return ZipContainerError::WrongEntryOrder;
    case detail::ZipContainerPreflightError::Utf8FlagMissing:
        return ZipContainerError::Utf8FlagMissing;
    case detail::ZipContainerPreflightError::ForbiddenGeneralPurposeFlag:
        return ZipContainerError::ForbiddenGeneralPurposeFlag;
    case detail::ZipContainerPreflightError::UnsupportedCompressionMethod:
        return ZipContainerError::UnsupportedCompressionMethod;
    case detail::ZipContainerPreflightError::ExtraFieldForbidden:
        return ZipContainerError::ExtraFieldForbidden;
    case detail::ZipContainerPreflightError::EntryCommentForbidden:
        return ZipContainerError::EntryCommentForbidden;
    case detail::ZipContainerPreflightError::Zip64Forbidden:
        return ZipContainerError::Zip64Forbidden;
    case detail::ZipContainerPreflightError::LocalCentralHeaderDisagreement:
        return ZipContainerError::LocalCentralHeaderDisagreement;
    case detail::ZipContainerPreflightError::OverlappingOrUnaccountedByteRange:
        return ZipContainerError::OverlappingOrUnaccountedByteRange;
    case detail::ZipContainerPreflightError::NonRegularEntry:
        return ZipContainerError::NonRegularEntry;
    case detail::ZipContainerPreflightError::ExecutableEntry:
        return ZipContainerError::ExecutableEntry;
    case detail::ZipContainerPreflightError::ExpandedSizeLimitExceeded:
        return ZipContainerError::ExpandedSizeLimitExceeded;
    case detail::ZipContainerPreflightError::ExpansionRatioExceeded:
        return ZipContainerError::ExpansionRatioExceeded;
    case detail::ZipContainerPreflightError::StoredSizeMismatch:
        return ZipContainerError::StoredSizeMismatch;
    case detail::ZipContainerPreflightError::SizeOverflow:
        return ZipContainerError::SizeOverflow;
    }
    // Unreachable: every enumerator is handled above. Fails closed rather than falling through to
    // an unrelated default.
    return ZipContainerError::QualifiedReaderDisagreement;
}

} // namespace

StagedCopyResult StagedCopyResult::success() noexcept {
    StagedCopyResult result;
    return result;
}

StagedCopyResult
StagedCopyResult::failure(StagedCopyFailure failureValue,
                          const std::optional<platform::StagedArtifactError> rejectDiagnostic) {
    StagedCopyResult result;
    // StagedCopyFailurePayload's alternatives are all trivial types (unlike staged_save.hpp's
    // StagedSaveFailurePayload, which carries SaveArchiveErrorPath-bearing alternatives), so
    // StagedCopyFailure itself ends up trivially copyable; std::move() would be a no-op clang-tidy
    // flags (performance-move-const-arg), so this is a plain copy.
    result.failure_.emplace(failureValue);
    result.rejectDiagnostic_ = rejectDiagnostic;
    return result;
}

StagedCopyResult stageCopyArchive(platform::StagedArtifactLease& lease,
                                  const std::span<const std::byte> sourceBytes,
                                  const ZipContainerLimits& limits,
                                  ProjectIoOperationMemory operation) noexcept {
    auto stage = StagedCopyStage::StageWrite;
    try {
        // Step 1: write the source bytes to the stage verbatim and close/reopen for verification.
        const auto writeResult = lease.write(sourceBytes);
        if (!writeResult) {
            return StagedCopyResult::failure(StagedCopyFailure(
                StagedCopyStage::StageWrite,
                StagedSavePlatformFailure{writeResult.error, StagedSaveLeaseCall::Write}));
        }

        stage = StagedCopyStage::StageFinish;
        const auto finishResult = lease.finishWriting();
        if (!finishResult) {
            return StagedCopyResult::failure(StagedCopyFailure(
                StagedCopyStage::StageFinish,
                StagedSavePlatformFailure{finishResult.error, StagedSaveLeaseCall::FinishWriting}));
        }

        // Step 2: validate the platform's own accounting of what got staged against the bytes this
        // call wrote, before allocating a read-back buffer sized from a value that disagrees with
        // what was written (defense in depth; see staged_save.cpp's identical
        // StagedSizeDisagreement comment -- the same unreachability argument applies here).
        stage = StagedCopyStage::StagedSizeDisagreement;
        const auto stagedBytes = lease.stageBytes();
        if (stagedBytes != sourceBytes.size() || sourceBytes.size() > limits.maxArchiveBytes) {
            return StagedCopyResult::failure(
                StagedCopyFailure(StagedCopyStage::StagedSizeDisagreement,
                                  StagedCopySizeDisagreement{sourceBytes.size(), stagedBytes}));
        }

        stage = StagedCopyStage::ReadBackAllocation;
        // Unlike staged_save.cpp's readBackResource (which passes `operation` by copy because it
        // is used again afterward), this function has no further use of `operation`: move it in.
        ProjectIoMemoryResource readBackResource(std::move(operation));
        std::pmr::vector<std::byte> readBack(&readBackResource);
        // ProjectIoMemoryResource::do_allocate() throws on budget rejection; the catch clauses
        // below translate that into a typed StagedCopyResourceExhausted failure at this stage.
        readBack.resize(static_cast<std::size_t>(stagedBytes));

        stage = StagedCopyStage::StageRead;
        std::uint64_t offset = 0;
        const std::uint64_t total = readBack.size();
        while (offset < total) {
            const auto remaining = total - offset;
            const auto destination = std::span<std::byte>(readBack).subspan(offset, remaining);
            const auto readResult = lease.readForVerification(offset, destination);
            if (!readResult) {
                return StagedCopyResult::failure(StagedCopyFailure(
                    StagedCopyStage::StageRead,
                    StagedSavePlatformFailure{readResult.error,
                                              StagedSaveLeaseCall::ReadForVerification}));
            }
            if (readResult.bytesRead == 0 || readResult.bytesRead > remaining) {
                return StagedCopyResult::failure(StagedCopyFailure(
                    StagedCopyStage::StageRead,
                    StagedSavePlatformFailure{
                        platform::StagedArtifactError::StageVerificationReadFailed,
                        StagedSaveLeaseCall::ReadForVerification}));
            }
            offset += readResult.bytesRead;
        }

        // Step 3a: verify (a) -- the read-back bytes are byte-identical to the source. Sizes are
        // already known equal (the StagedSizeDisagreement check above), so a mismatch can only be a
        // content difference.
        stage = StagedCopyStage::Verification;
        const auto readBackBytes = std::span<const std::byte>(readBack);
        const auto mismatch = std::ranges::mismatch(sourceBytes, readBackBytes);
        if (mismatch.in1 != sourceBytes.end()) {
            const auto byteOffset =
                static_cast<std::uint64_t>(std::distance(sourceBytes.begin(), mismatch.in1));
            const auto rejectResult = lease.rejectVerification();
            const std::optional<platform::StagedArtifactError> rejectDiagnostic =
                rejectResult ? std::nullopt : std::optional(rejectResult.error);
            return StagedCopyResult::failure(StagedCopyFailure(StagedCopyStage::Verification,
                                                               StagedCopyByteMismatch{byteOffset}),
                                             rejectDiagnostic);
        }

        // Step 3b: verify (b) -- the read-back bytes still preflight as a conforming Constrained
        // ZIP Profile container. Allocation-free and cheap: no decompression, no dependency call.
        const detail::ZipContainerPreflightLimits preflightLimits{
            .maxArchiveBytes = limits.maxArchiveBytes,
            .maxManifestBytes = limits.maxManifestBytes,
            .maxDocumentBytes = limits.maxDocumentBytes,
            .maxTotalExpandedBytes = limits.maxTotalExpandedBytes,
            .maxExpansionRatio = limits.maxExpansionRatio,
        };
        const auto preflight = detail::preflightZipContainer(readBackBytes, preflightLimits);
        if (!preflight.succeeded()) {
            const auto rejectResult = lease.rejectVerification();
            const std::optional<platform::StagedArtifactError> rejectDiagnostic =
                rejectResult ? std::nullopt : std::optional(rejectResult.error);
            return StagedCopyResult::failure(
                StagedCopyFailure(
                    StagedCopyStage::Verification,
                    StagedCopyContainerSanityFailure{translatePreflightError(preflight.error),
                                                     preflight.byteOffset}),
                rejectDiagnostic);
        }

        // Step 4: accept. The lease is now exactly one publish() away; that call belongs to the
        // application-layer publication executor, never to this module.
        stage = StagedCopyStage::Accept;
        const auto acceptResult = lease.acceptVerification();
        if (!acceptResult) {
            return StagedCopyResult::failure(StagedCopyFailure(
                StagedCopyStage::Accept,
                StagedSavePlatformFailure{acceptResult.error,
                                          StagedSaveLeaseCall::AcceptVerification}));
        }
        return StagedCopyResult::success();
    } catch (const std::bad_alloc&) {
        return StagedCopyResult::failure(StagedCopyFailure(stage, StagedCopyResourceExhausted{}));
    } catch (...) {
        return StagedCopyResult::failure(StagedCopyFailure(stage, StagedCopyUnexpectedFailure{}));
    }
}

} // namespace bloom::project
