#include <bloom/project/open_archive.hpp>

#include "reopen_chain_internal.hpp"

#include <exception>
#include <optional>
#include <utility>

namespace bloom::project {

OpenArchiveResult OpenArchiveResult::opened(OpenedArchive value) {
    OpenArchiveResult result;
    result.outcome_ = OpenArchiveOutcome::Opened;
    result.opened_.emplace(std::move(value));
    return result;
}

OpenArchiveResult OpenArchiveResult::preservedReadOnlyRequired(OpenArchivePreservedReadOnly value) {
    OpenArchiveResult result;
    result.outcome_ = OpenArchiveOutcome::PreservedReadOnlyRequired;
    // OpenArchivePreservedReadOnly is trivially copyable (two enums plus a fixed-capacity
    // SaveArchiveErrorPath), so std::move() here would have no effect over a plain copy.
    result.preservedReadOnly_.emplace(value);
    return result;
}

OpenArchiveResult OpenArchiveResult::failure(SaveArchiveFailure failureValue) {
    OpenArchiveResult result;
    result.outcome_ = OpenArchiveOutcome::Failed;
    result.failure_.emplace(std::move(failureValue));
    return result;
}

OpenedArchive OpenArchiveResult::takeOpened() && {
    if (!opened_.has_value()) {
        std::terminate();
    }
    return std::move(*opened_);
}

SaveArchiveFailure OpenArchiveResult::takeFailure() && {
    if (!failure_.has_value()) {
        return {};
    }
    return std::move(*failure_);
}

// By-value `operation` is this module's uniform public-entry-point contract (matching
// buildSaveArchive(), buildVerifiedSaveArchive(), and verifySaveArchive() in save_archive.hpp, and
// this task's own frozen signature) even though, unlike those, this function has no final sink of
// its own to move it into: runReopenChain() takes `operation` by const reference (see
// reopen_chain_internal.hpp's own rationale), since it is the only Project I/O work
// openProjectArchive() does.
OpenArchiveResult openProjectArchive(const std::span<const std::byte> archive,
                                     const SaveArchiveLimits& limits,
                                     // NOLINTNEXTLINE(performance-unnecessary-value-param)
                                     ProjectIoOperationMemory operation) noexcept {
    auto chainResult = detail::runReopenChain(archive, limits, std::nullopt, operation);
    switch (chainResult.outcome()) {
    case detail::ReopenChainOutcome::Failed:
        return OpenArchiveResult::failure(std::move(chainResult).takeFailure());
    case detail::ReopenChainOutcome::ManifestPreservationRequired: {
        const auto* preservation = chainResult.manifestPreservation();
        return OpenArchiveResult::preservedReadOnlyRequired(OpenArchivePreservedReadOnly{
            .side = OpenArchivePreservedReadOnlySide::Manifest,
            .documentReason = RoundTripPreservationReason::None,
            .path = preservation != nullptr ? preservation->path : SaveArchiveErrorPath{},
        });
    }
    case detail::ReopenChainOutcome::DocumentPreservedReadOnlyRequired: {
        const auto* preservation = chainResult.documentPreservation();
        return OpenArchiveResult::preservedReadOnlyRequired(OpenArchivePreservedReadOnly{
            .side = OpenArchivePreservedReadOnlySide::Document,
            .documentReason =
                preservation != nullptr ? preservation->reason : RoundTripPreservationReason::None,
            .path = preservation != nullptr ? preservation->path : SaveArchiveErrorPath{},
        });
    }
    case detail::ReopenChainOutcome::Success: {
        auto success = std::move(chainResult).takeSuccess();
        OpenedArchive opened{
            .document = std::move(success.document.document),
            .colorSettings = std::move(success.document.colorSettings),
            .roundTrip = std::move(success.roundTrip),
            .schemaMinor =
                success.documentRootVersion.has_value() ? success.documentRootVersion->minor : 0,
            .requirements = std::move(success.manifest.requirements),
            .containerVersion = success.manifest.containerVersion,
            .documentSchemaVersion = success.manifest.documentSchemaVersion,
            .manifestReservation = std::move(success.manifestReservation),
            .decodeReservation = std::move(success.decodeReservation),
            .reconstructionReservation = std::move(success.reconstructionReservation),
        };
        return OpenArchiveResult::opened(std::move(opened));
    }
    }
    return OpenArchiveResult::failure(
        SaveArchiveFailure(SaveArchiveStage::None, SaveArchiveUnexpectedFailure{}));
}

} // namespace bloom::project
