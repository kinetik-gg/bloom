#include <bloom/host/save_publication.hpp>

#include <new>
#include <optional>
#include <utility>

namespace bloom::host {

SavePublicationResult
SavePublicationResult::published(platform::StagedArtifactPublicationResult publication,
                                 const PublicationIntentId intentId) noexcept {
    SavePublicationResult result;
    result.publication_ = publication;
    result.intentId_ = intentId;
    return result;
}

SavePublicationResult SavePublicationResult::failure(
    SavePublicationFailure failureValue,
    const std::optional<platform::StagedArtifactError> rejectDiagnostic) noexcept {
    SavePublicationResult result;
    result.failure_.emplace(std::move(failureValue));
    result.rejectDiagnostic_ = rejectDiagnostic;
    return result;
}

namespace {

[[nodiscard]] bool cancelled(const std::atomic_bool* flag) noexcept {
    return flag != nullptr && flag->load(std::memory_order_relaxed);
}

[[nodiscard]] SavePublicationResult cancelledResult() noexcept {
    return SavePublicationResult::failure(
        SavePublicationFailure(SavePublicationStage::Cancelled, std::monostate{}));
}

} // namespace

SavePublicationResult executeSavePublication(PublicationCoordinator& coordinator,
                                             platform::StagedArtifactCoordinator& artifacts,
                                             const SavePublicationRequest& request,
                                             project::ProjectIoOperationMemory operation) noexcept {
    auto stage = SavePublicationStage::Admission;
    try {
        // Step 1 (project-format.md step 1's admission half; the contract requires admission
        // before any path work): take ownership of a caller-supplied admission, or admit here.
        std::optional<PublicationAdmission> admission;
        if (request.preAdmitted != nullptr) {
            admission.emplace(std::move(*request.preAdmitted));
        } else {
            auto admissionResult = coordinator.admit();
            if (!admissionResult) {
                return SavePublicationResult::failure(SavePublicationFailure(
                    SavePublicationStage::Admission, admissionResult.status()));
            }
            admission.emplace(std::move(admissionResult).takeAdmission());
        }
        // A pre-admitted admission that is invalid or foreign to `coordinator` is not rejected
        // here: coordinator.registerTarget() below already performs that exact check and reports
        // it as a typed Registration-stage PublicationRegistrationStatus::InvalidAdmission
        // failure, so duplicating the check here would only duplicate the failure path.

        if (cancelled(request.cancellationFlag)) {
            return cancelledResult();
            // `admission` goes out of scope here and abandons via RAII.
        }

        // Step 2: platform preflight (project-format.md steps 1-2's path-resolution/rejection
        // half).
        stage = SavePublicationStage::Preflight;
        auto preflightResult = artifacts.preflight({.targetPath = request.targetPath,
                                                    .overwritePolicy = request.overwritePolicy,
                                                    .expectedTarget = request.expectedTarget});
        if (!preflightResult) {
            return SavePublicationResult::failure(
                SavePublicationFailure(SavePublicationStage::Preflight, preflightResult.error()));
            // `admission` abandons via RAII; nothing else was obtained yet.
        }
        auto target = std::move(preflightResult).takeTarget();
        const auto targetKey = target.targetKey();

        if (cancelled(request.cancellationFlag)) {
            return cancelledResult();
            // `admission` abandons and `target` releases its platform active-target admission.
        }

        // Step 3: register the admission for this target, consuming it into a target claim
        // (project-session.md's "the coordinator registers that intent for the target key
        // returned by ... preflight").
        stage = SavePublicationStage::Registration;
        auto registrationResult = coordinator.registerTarget(std::move(*admission), targetKey);
        if (!registrationResult) {
            return SavePublicationResult::failure(SavePublicationFailure(
                SavePublicationStage::Registration, registrationResult.status()));
            // registerTarget() already consumed or abandoned `admission` itself; `target`
            // releases its platform active-target admission.
        }
        auto claim = std::move(registrationResult).takeClaim();
        const auto intentId = claim.intentId();

        // Step 4: stage the artifact, exchanging the preflight target for a writable lease.
        stage = SavePublicationStage::Staging;
        auto stageResult = artifacts.stage(std::move(target));
        if (!stageResult) {
            return SavePublicationResult::failure(
                SavePublicationFailure(SavePublicationStage::Staging, stageResult.error()));
            // `claim` releases its target-claim record.
        }
        auto lease = std::move(stageResult).takeLease();

        if (cancelled(request.cancellationFlag)) {
            return cancelledResult();
            // `lease` cleans its unpublished stage; `claim` releases its target-claim record.
        }

        // Step 5: build/write/verify the archive over the staged lease (project-format.md steps
        // 3-5, owned by project::stageSaveArchive()).
        stage = SavePublicationStage::StagedSave;
        auto stagedSaveResult = project::stageSaveArchive(
            lease, *request.manifest, *request.document, request.limits, std::move(operation));
        if (!stagedSaveResult) {
            // Wrap the StagedSaveResult's failure verbatim: the nested project::StagedSaveFailure
            // is copied as-is into this stage's payload, and its own rejectDiagnostic (relevant
            // only for StagedSaveStage::Verification) is forwarded alongside it.
            return SavePublicationResult::failure(
                SavePublicationFailure(SavePublicationStage::StagedSave,
                                       *stagedSaveResult.failure()),
                stagedSaveResult.rejectDiagnostic());
            // `lease` cleans its unpublished/rejected stage; `claim` releases its target-claim
            // record.
        }

        // Step 6: attempt to enter the non-cancellable publication lease at the winning intent
        // (project-session.md: "Immediately before the platform coordinator grants its
        // non-cancellable publication lease, the staged artifact must still own the application
        // coordinator's winning intent for that target").
        stage = SavePublicationStage::Guard;
        auto guardResult = claim.tryEnterPublication();
        switch (guardResult.status()) {
        case PublicationGuardStatus::Entered: {
            // Held only for the duration of publish(): `guard` releases its publication-guard
            // record when this case's scope ends, after publish() has already run.
            auto guard = std::move(guardResult).takeGuard();
            auto publication = lease.publish(platform::PublicationDisposition::Proceed);
            return SavePublicationResult::published(publication, intentId);
        }
        case PublicationGuardStatus::Superseded:
            // No guard is granted on Superseded (a newer same-target intent already owns
            // publication order), but the lease must still run its publish path so the platform
            // records the superseded outcome and cleans up the stage -- it does not know about
            // application-coordinator intents on its own.
            return SavePublicationResult::published(
                lease.publish(platform::PublicationDisposition::Superseded), intentId);
        case PublicationGuardStatus::InvalidClaim:
        case PublicationGuardStatus::TargetBusy:
        case PublicationGuardStatus::AlreadyEntered:
            // Unreachable in this single-executor composition (see the header's PublicationGuard
            // status comment): `claim` is always freshly registered and entered at most once by
            // this one call, so InvalidClaim/AlreadyEntered cannot occur, and this call never
            // holds a concurrent guard against its own claim's target, so TargetBusy cannot
            // occur either. Still handled as a typed protocol failure -- never calling publish()
            // -- because a future concurrent multi-executor flow can reach them for real.
            return SavePublicationResult::failure(
                SavePublicationFailure(SavePublicationStage::Guard, guardResult.status()));
            // `lease` cleans its unpublished stage; `claim` releases its target-claim record.
        }
        // All PublicationGuardStatus enumerators are handled above; this is unreachable but keeps
        // the function's control flow provably total for the compiler.
        return SavePublicationResult::failure(
            SavePublicationFailure(SavePublicationStage::Guard, guardResult.status()));
    } catch (const std::bad_alloc&) {
        return SavePublicationResult::failure(
            SavePublicationFailure(stage, SavePublicationResourceExhausted{}));
    } catch (...) {
        return SavePublicationResult::failure(
            SavePublicationFailure(stage, SavePublicationUnexpectedFailure{}));
    }
}

} // namespace bloom::host
