#include <bloom/project/staged_save.hpp>

#include "save_archive_internal.hpp"

#include <bloom/project/project_io_memory_resource.hpp>

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <new>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace bloom::project {

StagedSaveResult StagedSaveResult::success() noexcept {
    StagedSaveResult result;
    return result;
}

StagedSaveResult
StagedSaveResult::failure(StagedSaveFailure failureValue,
                          const std::optional<platform::StagedArtifactError> rejectDiagnostic) {
    StagedSaveResult result;
    result.failure_.emplace(std::move(failureValue));
    result.rejectDiagnostic_ = rejectDiagnostic;
    return result;
}

StagedSaveResult stageSaveArchive(platform::StagedArtifactLease& lease,
                                  const CanonicalManifestV1& manifest,
                                  const CanonicalDocumentV1& document,
                                  const SaveArchiveLimits& limits,
                                  ProjectIoOperationMemory operation) noexcept {
    auto stage = StagedSaveStage::Build;
    try {
        // Step 1: build the archive in memory. No in-memory verification here -- the
        // verification that matters is over the staged bytes (steps 3-4 below). `operation` is a
        // cheap shared_ptr-backed handle (see save_archive.cpp's equivalent comment), so this
        // copy and the later move into verifySaveArchive() are both safe; buildSaveArchiveEntries
        // is the same shared build routine buildSaveArchive()/buildVerifiedSaveArchive() use (see
        // save_archive_internal.hpp), returned here with the manifest/document entry bytes still
        // attached because step 4 needs them for SaveArchiveExpectedContent.
        auto outcome = detail::buildSaveArchiveEntries(manifest, document, limits, operation);
        if (!outcome) {
            return StagedSaveResult::failure(
                StagedSaveFailure(StagedSaveStage::Build, std::move(outcome).takeFailure()));
        }
        const auto archiveBytes = outcome.archiveBytes();

        // Step 2: write the built archive to the stage and close/reopen it for verification.
        stage = StagedSaveStage::StageWrite;
        const auto writeResult = lease.write(archiveBytes);
        if (!writeResult) {
            return StagedSaveResult::failure(StagedSaveFailure(
                StagedSaveStage::StageWrite,
                StagedSavePlatformFailure{writeResult.error, StagedSaveLeaseCall::Write}));
        }

        stage = StagedSaveStage::StageFinish;
        const auto finishResult = lease.finishWriting();
        if (!finishResult) {
            return StagedSaveResult::failure(StagedSaveFailure(
                StagedSaveStage::StageFinish,
                StagedSavePlatformFailure{finishResult.error, StagedSaveLeaseCall::FinishWriting}));
        }

        // Step 3: read the staged bytes back. Validate the platform's own accounting of what got
        // staged against the archive this function built and wrote, and fail closed before
        // allocating a read-back buffer sized from a value that disagrees with what was written.
        // This is defense in depth: the real platform coordinator only reports stageBytes() as
        // the sum of accepted write() calls, and this function issues exactly one write() of
        // `archiveBytes`, so the two are not expected to disagree in practice (see the
        // implementor's report for why this path is believed unreachable through the public
        // lease API and shipped fault points).
        stage = StagedSaveStage::StagedSizeDisagreement;
        const auto stagedBytes = lease.stageBytes();
        if (stagedBytes != archiveBytes.size() ||
            archiveBytes.size() > limits.container.maxArchiveBytes) {
            return StagedSaveResult::failure(
                StagedSaveFailure(StagedSaveStage::StagedSizeDisagreement,
                                  StagedSaveSizeDisagreement{archiveBytes.size(), stagedBytes}));
        }

        stage = StagedSaveStage::ReadBackAllocation;
        ProjectIoMemoryResource readBackResource(operation);
        std::pmr::vector<std::byte> readBack(&readBackResource);
        // ProjectIoMemoryResource::do_allocate() throws on budget rejection (see
        // project_io_memory_resource.hpp); the catch clauses below translate that into a typed
        // ResourceExhausted failure at this stage.
        readBack.resize(static_cast<std::size_t>(stagedBytes));

        stage = StagedSaveStage::StageRead;
        std::uint64_t offset = 0;
        const std::uint64_t total = readBack.size();
        while (offset < total) {
            const auto remaining = total - offset;
            const auto destination = std::span<std::byte>(readBack).subspan(offset, remaining);
            const auto readResult = lease.readForVerification(offset, destination);
            if (!readResult) {
                return StagedSaveResult::failure(StagedSaveFailure(
                    StagedSaveStage::StageRead,
                    StagedSavePlatformFailure{readResult.error,
                                              StagedSaveLeaseCall::ReadForVerification}));
            }
            if (readResult.bytesRead == 0 || readResult.bytesRead > remaining) {
                // A zero-progress or over-bound "successful" read is inconsistent with the
                // requested span; the platform contract has no dedicated error for this
                // (StagedArtifactError enumerates real platform failure causes, not caller-side
                // protocol violations it cannot itself commit), so it is reported at this call
                // using the closest existing verification-read error.
                return StagedSaveResult::failure(StagedSaveFailure(
                    StagedSaveStage::StageRead,
                    StagedSavePlatformFailure{
                        platform::StagedArtifactError::StageVerificationReadFailed,
                        StagedSaveLeaseCall::ReadForVerification}));
            }
            offset += readResult.bytesRead;
        }

        // Step 4: verify the staged/read-back bytes against the in-memory entries this function
        // built (not against the archive it wrote -- the whole point is to verify what actually
        // reached the stage).
        stage = StagedSaveStage::Verification;
        const auto expected = outcome.expectedContent(manifest.documentSchemaVersion);
        // Final use of `operation` in this function.
        auto verified = verifySaveArchive(std::span<const std::byte>(readBack), expected, limits,
                                          std::move(operation));
        if (!verified) {
            auto verificationFailure = std::move(verified).takeFailure();
            // Best-effort: a rejectVerification() failure is a secondary diagnostic that must
            // never mask the primary verification failure above. The lease destructor still owns
            // stage cleanup either way (see the header comment), so this call's only job is to
            // move the lease to its correct terminal state when it can.
            const auto rejectResult = lease.rejectVerification();
            const std::optional<platform::StagedArtifactError> rejectDiagnostic =
                rejectResult ? std::nullopt : std::optional(rejectResult.error);
            return StagedSaveResult::failure(
                StagedSaveFailure(StagedSaveStage::Verification, std::move(verificationFailure)),
                rejectDiagnostic);
        }

        // Step 5: accept. The lease is now exactly one publish() away; that call belongs to the
        // application-layer publication coordinator, never to this module.
        stage = StagedSaveStage::Accept;
        const auto acceptResult = lease.acceptVerification();
        if (!acceptResult) {
            return StagedSaveResult::failure(StagedSaveFailure(
                StagedSaveStage::Accept,
                StagedSavePlatformFailure{acceptResult.error,
                                          StagedSaveLeaseCall::AcceptVerification}));
        }
        return StagedSaveResult::success();
    } catch (const std::bad_alloc&) {
        return StagedSaveResult::failure(StagedSaveFailure(stage, SaveArchiveResourceExhausted{}));
    } catch (...) {
        return StagedSaveResult::failure(StagedSaveFailure(stage, SaveArchiveUnexpectedFailure{}));
    }
}

} // namespace bloom::project
