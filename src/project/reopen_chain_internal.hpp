#ifndef BLOOM_PROJECT_REOPEN_CHAIN_INTERNAL_HPP
#define BLOOM_PROJECT_REOPEN_CHAIN_INTERNAL_HPP

#include <bloom/document/schema_version.hpp>
#include <bloom/project/document_decode.hpp>
#include <bloom/project/document_reconstruct.hpp>
#include <bloom/project/manifest_decode.hpp>
#include <bloom/project/project_io_memory.hpp>
#include <bloom/project/round_trip_state.hpp>
#include <bloom/project/save_archive.hpp>
#include <bloom/project/zip_container.hpp>

#include <optional>
#include <span>
#include <utility>

// Shared reopen/decode/reconstruct implementation behind verifySaveArchive() (save_archive.cpp)
// and openProjectArchive() (open_archive.cpp). Not a public Project I/O contract: only these two
// translation units include this header, matching the save_archive_internal.hpp /
// document_decode_internal.hpp / canonical_json_string_detail.hpp precedent for a private
// cross-file implementation seam within one CMake target.
//
// runReopenChain() runs exactly the prefix the two callers share: container read, strict parse of
// both entries under one shared JSON value budget, manifest decode, version agreement, a routed
// document migration pass, document decode, reconstruction, and manifest-requirements validation
// against the reconstructed project (see save_archive.hpp's SaveArchiveStage enum, whose
// ContainerRead..RequirementsValidation members -- now including DocumentMigration -- name this
// exact prefix). The migration pass itself (document_migration.hpp's migrateDocumentDom()) only
// ever does real work for a same-major, at-or-below-current-minor document; today that is only the
// identity case, since schema {1,0} is the only version Bloom has ever shipped -- see this file's
// implementation in save_archive.cpp for the exact routing condition. Save's own re-encode and
// byte-comparison stages
// (ManifestReencode..DocumentByteComparison) are not part of this routine; they run only in
// verifySaveArchive() against the state this routine hands back. Every allocation is charged
// through `operation`, identically to the pre-split verifySaveArchive() this was extracted from.
//
// Implemented in save_archive.cpp (reusing that file's existing anonymous-namespace helpers
// directly) rather than a separate .cpp, mirroring save_archive_internal.hpp's own
// buildSaveArchiveEntries() precedent: the shared *declaration* lives in a sibling header, but the
// implementation stays colocated with the helpers it composes.
namespace bloom::project::detail {

enum class ReopenChainOutcome : std::uint8_t {
    // A hard failure occurred; failure() names the stage-scoped SaveArchiveFailure. Both callers
    // treat this identically.
    Failed,
    // decodeManifestEnvelope() classified containerVersion as same-major newer-minor
    // (ManifestDecodeOutcome::PreservationRequired). No document parsing result beyond the raw DOM
    // is inspected further. Save folds this into a SaveArchiveManifestDecodeFailure (behavior
    // unchanged from before this split); Open surfaces it as a distinct non-error outcome.
    ManifestPreservationRequired,
    // decodeDocumentEnvelope() classified the document as PreservedReadOnlyRequired (an unknown
    // core discriminator or an out-of-subset unknown number). Save folds this into a
    // SaveArchiveDocumentDecodeFailure (behavior unchanged from before this split); Open surfaces
    // it as a distinct non-error outcome.
    DocumentPreservedReadOnlyRequired,
    // The complete shared prefix succeeded; value() names the decoded/reconstructed state.
    Success,
};

// Everything the shared prefix decoded and reconstructed, with every reservation that charges its
// resident footprint transferred into this struct (see docs/architecture/project-format.md,
// "Resource Limits": "Moving storage between stages transfers its charge"). The caller decides how
// long to keep this alive; every reservation -- and the archive-entry buffers `zipRead` still
// backs -- releases when this struct is destroyed. Move-only (ZipContainerReadResult,
// ReconstructedDocument, and ProjectIoMemoryReservation are each move-only).
struct ReopenChainSuccess final {
    // Backs the raw manifest.json/document.json entry bytes (ZipContainerReadResult::document()).
    // Only verifySaveArchive()'s own byte-comparison stages need these; openProjectArchive() reads
    // the state below and lets this drop when it returns, per its documented contract that Open
    // does not copy or retain the archive bytes.
    ZipContainerReadResult zipRead;
    ReconstructedDocument document;
    // Copied (not moved) out of the local ManifestDecodeResult: DecodedManifest is an ordinary
    // copyable value (unlike RoundTripState, it holds no deliberately move-only/opaque payload
    // data), and ManifestDecodeResult -- unlike DocumentDecodeResult -- exposes no move-out
    // accessor to begin with, so this is the only way to carry it past this function's return.
    DecodedManifest manifest;
    // The document root's raw decoded {major, minor}, read once during VersionAgreement; nullopt
    // only when the root's schemaVersion member itself could not be lexically read (in which case
    // DocumentDecode below fails with a typed error before Success is ever reached with this still
    // nullopt -- see documentRootVersion() in save_archive.cpp).
    std::optional<document::SchemaVersion> documentRootVersion;
    // Moved out of the local DocumentDecodeResult via its takeRoundTrip() && accessor (the one
    // sanctioned addition to document_decode.hpp this task makes -- see its declaration). Present
    // only when classification() was EditableWithRoundTrip.
    std::optional<RoundTripState> roundTrip;

    ProjectIoMemoryReservation manifestReservation;
    ProjectIoMemoryReservation decodeReservation;
    ProjectIoMemoryReservation reconstructionReservation;
};

// Diagnostics for a manifest-side ManifestPreservationRequired outcome.
struct ReopenChainManifestPreservation final {
    SaveArchiveErrorPath path;
};

// Diagnostics for a document-side DocumentPreservedReadOnlyRequired outcome.
struct ReopenChainDocumentPreservation final {
    RoundTripPreservationReason reason = RoundTripPreservationReason::None;
    SaveArchiveErrorPath path;
};

// [[nodiscard]] four-way result (Failed / ManifestPreservationRequired /
// DocumentPreservedReadOnlyRequired / Success). Move-only, since ReopenChainSuccess is.
class [[nodiscard]] ReopenChainResult final {
  public:
    ReopenChainResult(ReopenChainResult&&) noexcept = default;
    ReopenChainResult& operator=(ReopenChainResult&&) noexcept = default;
    ReopenChainResult(const ReopenChainResult&) = delete;
    ReopenChainResult& operator=(const ReopenChainResult&) = delete;
    ~ReopenChainResult() = default;

    [[nodiscard]] static ReopenChainResult success(ReopenChainSuccess value);
    [[nodiscard]] static ReopenChainResult failure(SaveArchiveFailure failure);
    [[nodiscard]] static ReopenChainResult manifestPreservationRequired(SaveArchiveErrorPath path);
    [[nodiscard]] static ReopenChainResult
    documentPreservedReadOnlyRequired(RoundTripPreservationReason reason,
                                      SaveArchiveErrorPath path);

    [[nodiscard]] ReopenChainOutcome outcome() const noexcept { return outcome_; }

    [[nodiscard]] const SaveArchiveFailure* failure() const& noexcept {
        return failure_.has_value() ? &*failure_ : nullptr;
    }
    [[nodiscard]] const SaveArchiveFailure* failure() const&& = delete;
    [[nodiscard]] SaveArchiveFailure takeFailure() &&;

    [[nodiscard]] const ReopenChainManifestPreservation* manifestPreservation() const& noexcept {
        return manifestPreservation_.has_value() ? &*manifestPreservation_ : nullptr;
    }
    [[nodiscard]] const ReopenChainManifestPreservation* manifestPreservation() const&& = delete;

    [[nodiscard]] const ReopenChainDocumentPreservation* documentPreservation() const& noexcept {
        return documentPreservation_.has_value() ? &*documentPreservation_ : nullptr;
    }
    [[nodiscard]] const ReopenChainDocumentPreservation* documentPreservation() const&& = delete;

    // Valid only when outcome() == Success. Every reservation and buffer above transfers to the
    // caller through this move.
    [[nodiscard]] ReopenChainSuccess takeSuccess() &&;

  private:
    ReopenChainResult() = default;

    ReopenChainOutcome outcome_ = ReopenChainOutcome::Failed;
    std::optional<ReopenChainSuccess> success_;
    std::optional<SaveArchiveFailure> failure_;
    std::optional<ReopenChainManifestPreservation> manifestPreservation_;
    std::optional<ReopenChainDocumentPreservation> documentPreservation_;
};

// Runs the shared reopen/decode/reconstruct/validate prefix against `archive`.
// `capturedInputVersion` is verifySaveArchive()'s expected-content documentSchemaVersion leg of
// VersionAgreement; it is std::nullopt for openProjectArchive(), which has no captured-input
// version to compare against (see docs/architecture/project-format.md "Versions, Migrations, And
// Preservation" and this task's own design decision on Open's narrower VersionAgreement leg). Every
// allocation is charged through `operation`, which this routine only ever reads (every downstream
// call it drives -- readZipContainer, parseStrictJsonDom, reserve() -- takes or copies its own
// handle); unlike buildSaveArchiveEntries() (save_archive_internal.hpp), this routine never sinks a
// final move of it, so it takes it by const reference rather than by value. Never throws.
[[nodiscard]] ReopenChainResult
runReopenChain(std::span<const std::byte> archive, const SaveArchiveLimits& limits,
               std::optional<document::SchemaVersion> capturedInputVersion,
               const ProjectIoOperationMemory& operation) noexcept;

} // namespace bloom::project::detail

#endif // BLOOM_PROJECT_REOPEN_CHAIN_INTERNAL_HPP
