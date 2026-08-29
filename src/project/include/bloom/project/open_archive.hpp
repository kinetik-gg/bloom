#pragma once

#include <bloom/document/color_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/schema_version.hpp>
#include <bloom/project/manifest_requirements.hpp>
#include <bloom/project/project_io_memory.hpp>
#include <bloom/project/round_trip_state.hpp>
#include <bloom/project/save_archive.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

// openProjectArchive(): the Open-side counterpart to verifySaveArchive()/buildVerifiedSaveArchive()
// (save_archive.hpp). Both reuse the exact same bounded reopen/decode/reconstruct/validate chain
// (bloom::project::detail::runReopenChain, reopen_chain_internal.hpp): container read, strict parse
// of both entries under one shared JSON value budget, manifest decode, version agreement, document
// decode, reconstruction, and manifest-requirements validation against the reconstructed project.
// Open omits Save's re-encode and byte-comparison stages (there is nothing to compare an Open
// against) and Save's captured-input version leg of VersionAgreement (there is no prior in-memory
// value for Open to agree with) -- see docs/architecture/project-format.md, "Versions, Migrations,
// And Preservation".
//
// This is a pure decode/reconstruct composition boundary: no file I/O, no async work, no
// migration, and no Save Copy. It performs no session/host wiring and depends on no host type --
// OpenedArchive's shape mirrors what bloom::host::DecodedProjectSessionRequest
// (project_session.hpp) needs one-to-one (minus host-owned concerns this module cannot determine,
// such as editability from unavailable providers, or display path) so a future session Open can
// install it directly.

namespace bloom::project {

// Which side of the reopen chain classified an archive as requiring preserved-read-only handling
// (see docs/architecture/project-format.md, "Versions, Migrations, And Preservation": "Open
// returns a preserved-read-only result containing the bounded original archive and diagnostics").
enum class OpenArchivePreservedReadOnlySide : std::uint8_t {
    None,
    // decodeManifestEnvelope() classified containerVersion as same-major newer-minor
    // (ManifestDecodeOutcome::PreservationRequired). Manifest round-trip capture is a later slice
    // (see manifest_decode.hpp's file comment), so this side never reaches document decode at all.
    Manifest,
    // decodeDocumentEnvelope() classified the document itself as PreservedReadOnlyRequired (an
    // unknown core discriminator or an out-of-subset unknown number; see document_decode.hpp's
    // RoundTripPreservationReason).
    Document,
};

// A successfully opened archive: every decoded/reconstructed value a future session Open needs to
// install editable (or degraded-editable) content. Owns every ProjectIoMemoryReservation the
// shared reopen chain accrued for this resident state -- document, colorSettings, roundTrip, and
// the manifest-derived fields below are each backed by one of the three reservations, which
// release only when this struct is destroyed (see docs/architecture/project-format.md, "Resource
// Limits": "Moving storage between stages transfers its charge"). The caller keeps this struct
// alive for as long as the opened document stays resident.
struct OpenedArchive final {
    std::unique_ptr<document::Document> document;
    document::ColorSettings colorSettings;
    // Present only for a same-major newer-minor (schema {1, minor > 0}) document whose unknown
    // additive members were all safely captured; nullopt for an exact {1,0} document.
    std::optional<RoundTripState> roundTrip;
    // The document root's decoded schemaVersion.minor (0 for an exact {1,0} document).
    std::uint32_t schemaMinor = 0;
    // Verbatim from the decoded manifest's requirement set (manifest_requirements.hpp); coverage
    // against the reconstructed project has already been validated by the shared chain.
    std::vector<ManifestRequirement> requirements;
    document::SchemaVersion containerVersion;
    document::SchemaVersion documentSchemaVersion;

    ProjectIoMemoryReservation manifestReservation;
    ProjectIoMemoryReservation decodeReservation;
    ProjectIoMemoryReservation reconstructionReservation;
};

// Diagnostics for a PreservedReadOnlyRequired outcome. Per the format contract the application
// keeps the bounded original archive itself for Save Copy: the caller already holds `archive` (the
// span passed to openProjectArchive()), so this result deliberately does not copy or retain any
// archive bytes.
struct OpenArchivePreservedReadOnly final {
    OpenArchivePreservedReadOnlySide side = OpenArchivePreservedReadOnlySide::None;
    // Valid only when side == Document; None when side == Manifest (manifest-side preservation
    // carries no finer-grained reason than "same-major newer-minor" -- see
    // ManifestDecodeOutcome::PreservationRequired).
    RoundTripPreservationReason documentReason = RoundTripPreservationReason::None;
    SaveArchiveErrorPath path;
};

enum class OpenArchiveOutcome : std::uint8_t {
    Opened,
    PreservedReadOnlyRequired,
    Failed,
};

// [[nodiscard]] three-way, move-only result (OpenedArchive and ProjectIoMemoryReservation are each
// move-only). outcome() names which of Opened/PreservedReadOnlyRequired/Failed is populated.
class [[nodiscard]] OpenArchiveResult final {
  public:
    OpenArchiveResult(OpenArchiveResult&&) noexcept = default;
    OpenArchiveResult& operator=(OpenArchiveResult&&) noexcept = default;
    OpenArchiveResult(const OpenArchiveResult&) = delete;
    OpenArchiveResult& operator=(const OpenArchiveResult&) = delete;
    ~OpenArchiveResult() = default;

    [[nodiscard]] static OpenArchiveResult opened(OpenedArchive value);
    [[nodiscard]] static OpenArchiveResult
    preservedReadOnlyRequired(OpenArchivePreservedReadOnly value);
    [[nodiscard]] static OpenArchiveResult failure(SaveArchiveFailure failure);

    [[nodiscard]] OpenArchiveOutcome outcome() const noexcept { return outcome_; }

    [[nodiscard]] const OpenedArchive* opened() const& noexcept {
        return opened_.has_value() ? &*opened_ : nullptr;
    }
    [[nodiscard]] const OpenedArchive* opened() const&& = delete;
    [[nodiscard]] OpenedArchive takeOpened() &&;

    [[nodiscard]] const OpenArchivePreservedReadOnly* preservedReadOnly() const& noexcept {
        return preservedReadOnly_.has_value() ? &*preservedReadOnly_ : nullptr;
    }
    [[nodiscard]] const OpenArchivePreservedReadOnly* preservedReadOnly() const&& = delete;

    [[nodiscard]] const SaveArchiveFailure* failure() const& noexcept {
        return failure_.has_value() ? &*failure_ : nullptr;
    }
    [[nodiscard]] const SaveArchiveFailure* failure() const&& = delete;
    [[nodiscard]] SaveArchiveFailure takeFailure() &&;

  private:
    OpenArchiveResult() = default;

    OpenArchiveOutcome outcome_ = OpenArchiveOutcome::Failed;
    std::optional<OpenedArchive> opened_;
    std::optional<OpenArchivePreservedReadOnly> preservedReadOnly_;
    std::optional<SaveArchiveFailure> failure_;
};

// Runs the shared reopen/decode/reconstruct/validate chain against `archive` with no captured-input
// version leg, then classifies the outcome into Opened / PreservedReadOnlyRequired / Failed (see
// the file comment above). Every allocation is charged through `operation`; on Opened, every
// reservation the chain accrued for the returned resident state transfers into the result (see
// OpenedArchive above). `archive` need not outlive the call except when the result is
// PreservedReadOnlyRequired, in which case the contract is that the CALLER retains it (this result
// never copies archive bytes). Never throws.
[[nodiscard]] OpenArchiveResult openProjectArchive(std::span<const std::byte> archive,
                                                   const SaveArchiveLimits& limits,
                                                   ProjectIoOperationMemory operation) noexcept;

} // namespace bloom::project
