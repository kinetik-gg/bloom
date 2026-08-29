#include <bloom/host/session_open.hpp>

#include <new>
#include <utility>

namespace bloom::host {

SessionOpenResult
SessionOpenResult::admissionFailure(const OpenIntentAdmissionStatus status) noexcept {
    SessionOpenResult result;
    result.stage_ = SessionOpenStage::Admission;
    result.admissionStatus_ = status;
    return result;
}

SessionOpenResult SessionOpenResult::openingFailure(project::SaveArchiveFailure failure) noexcept {
    SessionOpenResult result;
    result.stage_ = SessionOpenStage::Opening;
    result.openingFailure_ = std::move(failure);
    return result;
}

SessionOpenResult SessionOpenResult::installation(
    SessionOpenInstallOutcome outcome, const ProjectSessionContentKind attemptedContentKind,
    std::optional<project::OpenArchivePreservedReadOnly> preservedReadOnly) noexcept {
    SessionOpenResult result;
    result.stage_ = SessionOpenStage::Installation;
    result.installOutcome_ = outcome;
    result.attemptedContentKind_ = attemptedContentKind;
    result.preservedReadOnly_ = preservedReadOnly;
    return result;
}

SessionOpenResult SessionOpenResult::notOpened(const SessionOpenNotOpenedReason reason) noexcept {
    SessionOpenResult result;
    result.stage_ = SessionOpenStage::NotOpened;
    result.notOpenedReason_ = reason;
    return result;
}

namespace {

// Both install*() calls below are the only non-noexcept composed surfaces in this pipeline (each
// may allocate -- see ProjectSession::installDecodedReplacement()'s/
// installPreservedReadOnlyReplacement()'s declaration comments); each is wrapped individually so a
// thrown exception is reported as a typed SessionOpenInstallOutcome alternative rather than
// escaping this module's noexcept entry point. Per those methods' own strong exception guarantee, a
// caught exception here means the session was left completely untouched.

[[nodiscard]] SessionOpenResult
installDecodedAndReport(ProjectSession& session, const OpenIntentCapture intent,
                        DecodedReplacementContent content) noexcept {
    try {
        const auto status = session.installDecodedReplacement(intent, std::move(content));
        return SessionOpenResult::installation(status, ProjectSessionContentKind::DecodedDocument,
                                               std::nullopt);
    } catch (const std::bad_alloc&) {
        return SessionOpenResult::installation(SessionOpenInstallResourceExhausted{},
                                               ProjectSessionContentKind::DecodedDocument,
                                               std::nullopt);
    } catch (...) {
        return SessionOpenResult::installation(SessionOpenInstallUnexpectedFailure{},
                                               ProjectSessionContentKind::DecodedDocument,
                                               std::nullopt);
    }
}

[[nodiscard]] SessionOpenResult
installPreservedReadOnlyAndReport(ProjectSession& session, const OpenIntentCapture intent,
                                  ProjectDisplayPath displayPath,
                                  project::OpenArchivePreservedReadOnly diagnostics) noexcept {
    try {
        const auto status =
            session.installPreservedReadOnlyReplacement(intent, std::move(displayPath));
        return SessionOpenResult::installation(status, ProjectSessionContentKind::PreservedReadOnly,
                                               diagnostics);
    } catch (const std::bad_alloc&) {
        return SessionOpenResult::installation(SessionOpenInstallResourceExhausted{},
                                               ProjectSessionContentKind::PreservedReadOnly,
                                               diagnostics);
    } catch (...) {
        return SessionOpenResult::installation(SessionOpenInstallUnexpectedFailure{},
                                               ProjectSessionContentKind::PreservedReadOnly,
                                               diagnostics);
    }
}

} // namespace

SessionOpenResult
installOpenedArchiveResult(ProjectSession& session, const OpenIntentCapture intent,
                           project::OpenArchiveResult opened,
                           std::optional<ProjectDisplayPath> displayPath) noexcept {
    if (opened.outcome() == project::OpenArchiveOutcome::Failed) {
        // Session untouched: no install*() call is ever made on this path.
        return SessionOpenResult::openingFailure(std::move(opened).takeFailure());
    }

    if (opened.outcome() == project::OpenArchiveOutcome::PreservedReadOnlyRequired) {
        const auto* preservation = opened.preservedReadOnly();
        // Defensive: openProjectArchive()'s own contract guarantees this is non-null on this
        // outcome (see open_archive.hpp); handled anyway so this function stays provably total.
        project::OpenArchivePreservedReadOnly diagnostics =
            preservation != nullptr ? *preservation : project::OpenArchivePreservedReadOnly{};
        if (!displayPath.has_value()) {
            // createPreservedReadOnly() requires a path too: a pathless preserved-read-only
            // install is refused here, without ever calling installPreservedReadOnlyReplacement()
            // (which requires a real ProjectDisplayPath by contract) -- session untouched. The
            // caller still gets the preservation diagnostics verbatim (it retains the archive
            // bytes for Save Copy per this module's file comment).
            return SessionOpenResult::installation(SessionInstallStatus::InvalidContent,
                                                   ProjectSessionContentKind::PreservedReadOnly,
                                                   diagnostics);
        }
        return installPreservedReadOnlyAndReport(session, intent, std::move(*displayPath),
                                                 diagnostics);
    }

    // Opened: build a DecodedReplacementContent from the OpenedArchive, mirroring its shape
    // one-to-one (see open_archive.hpp's file comment and DecodedReplacementContent's own comment).
    auto openedValue = std::move(opened).takeOpened();
    DecodedReplacementReservations reservations(std::move(openedValue.manifestReservation),
                                                std::move(openedValue.decodeReservation),
                                                std::move(openedValue.reconstructionReservation));
    DecodedReplacementContent content(
        std::move(openedValue.document), std::move(openedValue.colorSettings),
        std::move(openedValue.roundTrip), openedValue.schemaMinor,
        std::move(openedValue.requirements),
        // Editability is always Editable in this slice: provider availability/degraded-editable
        // detection needs a capability registry that does not exist yet (see this task's design
        // decision 2) -- every opened archive installs as fully Editable here.
        DecodedProjectEditability::Editable, std::move(displayPath), std::move(reservations));
    return installDecodedAndReport(session, intent, std::move(content));
}

SessionOpenResult openSessionArchive(ProjectSession& session, std::span<const std::byte> archive,
                                     std::optional<ProjectDisplayPath> displayPath,
                                     const project::SaveArchiveLimits& limits,
                                     project::ProjectIoOperationMemory operation) noexcept {
    const auto admission = session.admitOpenIntent();
    if (!admission) {
        return SessionOpenResult::admissionFailure(admission.status());
    }
    auto opened = project::openProjectArchive(archive, limits, std::move(operation));
    return installOpenedArchiveResult(session, admission.capture(), std::move(opened),
                                      std::move(displayPath));
}

} // namespace bloom::host
