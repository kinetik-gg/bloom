#include <bloom/host/copy_publication.hpp>

#include <new>
#include <optional>
#include <utility>

namespace bloom::host {

CopyPublicationResult
CopyPublicationResult::published(platform::StagedArtifactPublicationResult publication,
                                 const PublicationIntentId intentId) noexcept {
    CopyPublicationResult result;
    result.publication_ = publication;
    result.intentId_ = intentId;
    return result;
}

CopyPublicationResult CopyPublicationResult::failure(
    CopyPublicationFailure failureValue,
    const std::optional<platform::StagedArtifactError> rejectDiagnostic) noexcept {
    CopyPublicationResult result;
    // CopyPublicationFailurePayload's alternatives are all trivial types
    // (project::StagedCopyFailure included -- see staged_copy.cpp's identical comment), so
    // CopyPublicationFailure ends up trivially copyable; std::move() would be a no-op clang-tidy
    // flags (performance-move-const-arg), so this is a plain copy.
    result.failure_.emplace(failureValue);
    result.rejectDiagnostic_ = rejectDiagnostic;
    return result;
}

namespace {

[[nodiscard]] bool cancelled(const std::atomic_bool* flag) noexcept {
    return flag != nullptr && flag->load(std::memory_order_relaxed);
}

[[nodiscard]] CopyPublicationResult cancelledResult() noexcept {
    return CopyPublicationResult::failure(
        CopyPublicationFailure(CopyPublicationStage::Cancelled, std::monostate{}));
}

} // namespace

CopyPublicationResult executeCopyPublication(PublicationCoordinator& coordinator,
                                             platform::StagedArtifactCoordinator& artifacts,
                                             const CopyPublicationRequest& request,
                                             project::ProjectIoOperationMemory operation) noexcept {
    auto stage = CopyPublicationStage::Admission;
    try {
        // Step 1: take ownership of a caller-supplied admission, or admit here -- identical to
        // executeSavePublication()'s own step 1 (see its comment for the full rationale).
        std::optional<PublicationAdmission> admission;
        if (request.preAdmitted != nullptr) {
            admission.emplace(std::move(*request.preAdmitted));
        } else {
            auto admissionResult = coordinator.admit();
            if (!admissionResult) {
                return CopyPublicationResult::failure(CopyPublicationFailure(
                    CopyPublicationStage::Admission, admissionResult.status()));
            }
            admission.emplace(std::move(admissionResult).takeAdmission());
        }

        if (cancelled(request.cancellationFlag)) {
            return cancelledResult();
        }

        // Step 2: platform preflight.
        stage = CopyPublicationStage::Preflight;
        auto preflightResult = artifacts.preflight({.targetPath = request.targetPath,
                                                    .overwritePolicy = request.overwritePolicy,
                                                    .expectedTarget = request.expectedTarget});
        if (!preflightResult) {
            return CopyPublicationResult::failure(
                CopyPublicationFailure(CopyPublicationStage::Preflight, preflightResult.error()));
        }
        auto target = std::move(preflightResult).takeTarget();
        const auto targetKey = target.targetKey();

        if (cancelled(request.cancellationFlag)) {
            return cancelledResult();
        }

        // Step 3: register the admission for this target, consuming it into a target claim -- the
        // same application coordinator sequence Save and frame export share (project-session.md's
        // "Per-Target Ordering And Publication").
        stage = CopyPublicationStage::Registration;
        auto registrationResult = coordinator.registerTarget(std::move(*admission), targetKey);
        if (!registrationResult) {
            return CopyPublicationResult::failure(CopyPublicationFailure(
                CopyPublicationStage::Registration, registrationResult.status()));
        }
        auto claim = std::move(registrationResult).takeClaim();
        const auto intentId = claim.intentId();

        // Step 4: stage the artifact, exchanging the preflight target for a writable lease.
        stage = CopyPublicationStage::Staging;
        auto stageResult = artifacts.stage(std::move(target));
        if (!stageResult) {
            return CopyPublicationResult::failure(
                CopyPublicationFailure(CopyPublicationStage::Staging, stageResult.error()));
        }
        auto lease = std::move(stageResult).takeLease();

        if (cancelled(request.cancellationFlag)) {
            return cancelledResult();
        }

        // Step 5: write/read-back/verify the byte copy over the staged lease (project::
        // stageCopyArchive() -- the byte-copy sibling of project::stageSaveArchive()).
        stage = CopyPublicationStage::StagedCopy;
        auto stagedCopyResult = project::stageCopyArchive(
            lease, request.sourceBytes, request.limits.container, std::move(operation));
        if (!stagedCopyResult) {
            return CopyPublicationResult::failure(
                CopyPublicationFailure(CopyPublicationStage::StagedCopy,
                                       *stagedCopyResult.failure()),
                stagedCopyResult.rejectDiagnostic());
        }

        // Step 6: attempt to enter the non-cancellable publication lease at the winning intent --
        // identical to executeSavePublication()'s own step 6 (see its comment for the full guard
        // contract, including why InvalidClaim/TargetBusy/AlreadyEntered are unreachable from this
        // single-executor composition but still handled).
        stage = CopyPublicationStage::Guard;
        auto guardResult = claim.tryEnterPublication();
        switch (guardResult.status()) {
        case PublicationGuardStatus::Entered: {
            auto guard = std::move(guardResult).takeGuard();
            auto publication = lease.publish(platform::PublicationDisposition::Proceed);
            return CopyPublicationResult::published(publication, intentId);
        }
        case PublicationGuardStatus::Superseded:
            return CopyPublicationResult::published(
                lease.publish(platform::PublicationDisposition::Superseded), intentId);
        case PublicationGuardStatus::InvalidClaim:
        case PublicationGuardStatus::TargetBusy:
        case PublicationGuardStatus::AlreadyEntered:
            return CopyPublicationResult::failure(
                CopyPublicationFailure(CopyPublicationStage::Guard, guardResult.status()));
        }
        return CopyPublicationResult::failure(
            CopyPublicationFailure(CopyPublicationStage::Guard, guardResult.status()));
    } catch (const std::bad_alloc&) {
        return CopyPublicationResult::failure(
            CopyPublicationFailure(stage, CopyPublicationResourceExhausted{}));
    } catch (...) {
        return CopyPublicationResult::failure(
            CopyPublicationFailure(stage, CopyPublicationUnexpectedFailure{}));
    }
}

} // namespace bloom::host
