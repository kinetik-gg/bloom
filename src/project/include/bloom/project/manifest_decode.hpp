#pragma once

#include <bloom/document/schema_version.hpp>
#include <bloom/project/manifest_requirements.hpp>
#include <bloom/project/strict_json_dom.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Typed decode of the manifest.json envelope from a parsed strict JSON DOM (see
// docs/architecture/project-format.md, "Manifest Shape", "Version 1 Constants", "Canonical JSON
// Primitives" -- "Decimal Strings And JSON Integers" -- and "Resource Limits" for the namespaced-ID
// lexical domain). This mirrors decodeDocumentEnvelope's idiom (document_decode.hpp) at manifest
// scale: exact member order over a closed shape, checked value surfaces, member-path diagnostics on
// failure, and a three-way outcome distinguishing a hard decode error from a same-major newer-minor
// containerVersion that cannot yet be handled losslessly.
//
// Scope: the manifest root (format/containerVersion/document/requirements) and every requirement
// record, decoded into a shape bloom::project::validateManifestRequirements() (see
// manifest_requirements.hpp) can consume directly -- ManifestRequirement is reused verbatim,
// exactly as document_decode.hpp reuses bloom::document::ParameterRecord/NodeRecord/... rather than
// reinventing a parallel decode-only shape. Only the lexical/structural requirement invariants that
// do not need decoded project truth are checked here (canonical member order, exact `format`/
// `document.path`, requirement sort order, duplicate provider/capability pairs, provided-node-type
// sort order and uniqueness, and namespaced-ID lexical domains); coverage against a decoded project
// (exact non-foundation node-type coverage, extension-owner coverage) stays in
// validateManifestRequirements() and is deliberately not duplicated here.
//
// Identity before version: `format` is checked (exact kind and value) immediately after the
// tolerant first pass over the root, before containerVersion is even peeked at. A manifest that
// does not declare itself as this format is a typed WrongValueKind/InvalidFormat decode error
// outright -- it never reaches containerVersion classification below, so a file that explicitly
// declares itself NOT a Bloom manifest (wrong or non-string `format`) can never be misclassified as
// a newer *Bloom* manifest just because its containerVersion happens to parse as {1, minor > 0}.
//
// Version gates: containerVersion.major or document.schemaVersion.major != 1 is a typed
// UnsupportedMajorVersion decode error (migration is out of scope of this module). An exact
// containerVersion {1,0} decodes strictly: an unrecognized member anywhere in the manifest (root,
// either version object, the document object, or a requirement) is a typed UnknownMember/
// MemberOutOfOrder decode error. A same-major newer-minor containerVersion ({1, minor > 0}) is
// classified as PreservationRequired instead -- a distinct outcome from both Decoded and Failed --
// unconditionally, whether or not the manifest actually contains an unrecognized member: manifest
// round-trip capture (this module's analogue of document_decode.hpp's RT1 unknown-member capture
// into RoundTripState) is a later slice, exactly as the document decoder staged its own R1/R2 (full
// decode, no capture) before RT1 added capture for a same-major newer-minor document. Until that
// slice lands, this module cannot tell "safely re-encodable extra content" apart from "content that
// would be silently dropped," so it refuses to guess either way and reports PreservationRequired
// without inspecting the rest of the manifest beyond the `format` identity check already performed
// above. A newer-minor *document* `schemaVersion` value embedded in an otherwise-{1,0} manifest is
// NOT a manifest-level refusal: it is decoded and exposed like any other value. Manifest/document
// schema agreement and the document side's own newer-minor handling belong to a later chain-layer
// slice, not this module.
//
// Decoding reuses canonical_decimal.hpp's parseCanonicalJsonUInt32 for every `major`/`minor` field
// and bloom::document::isValidNamespacedIdentifier (persisted_text.hpp) for every providerId/
// capabilityId/providedNodeTypeIds lexical check -- the same checked surfaces
// canonical_manifest.cpp's writer-side validator uses -- rather than hand-rolling a duplicate
// regex-shaped check. This module does not include document_decode_internal.hpp: that header is a
// private two-translation-unit seam typed to DocumentDecodeError/RoundTripState (see its own file
// comment) and reusing it here would be a layering violation, so the shared *shape* of its
// matchOrderedMembers/decodeStringMember/decodeUInt32Member helpers is mirrored locally in
// manifest_decode.cpp instead, typed to ManifestDecodeError.
//
// decodeManifestEnvelope() is not noexcept, for the same reason as decodeDocumentEnvelope(): its
// intermediate std::string/std::vector-backed results can throw std::bad_alloc on allocation
// failure, so marking it noexcept while it allocates through a throwing surface would be an
// untruthful noexcept claim. Callers that need a termination-free boundary wrap the call
// themselves.

namespace bloom::project {

// The decoded manifest.json envelope's durable values: the container version, the declared document
// path and schema version, and the requirement set in the exact shape
// validateManifestRequirements() consumes.
struct DecodedManifest final {
    document::SchemaVersion containerVersion;
    std::string documentPath;
    document::SchemaVersion documentSchemaVersion;
    std::vector<ManifestRequirement> requirements;

    friend bool operator==(const DecodedManifest&, const DecodedManifest&) = default;
};

enum class ManifestDecodeError : std::uint8_t {
    None,
    // A member's JSON value kind did not match the schema (e.g. a number where a string was
    // required, or an array where an object was required).
    WrongValueKind,
    // A required member was absent from an otherwise well-formed object.
    MissingMember,
    // An object member's key is not a member this schema/position recognizes.
    UnknownMember,
    // An object member's key is known to this schema but appeared in the wrong position; exact
    // member order is part of canonical acceptance.
    MemberOutOfOrder,
    // A version object's `major`/`minor` JSON-number token is not a canonical non-negative integer
    // spelling (fraction, exponent, leading zero, or a `+`/`-` sign) or does not fit uint32.
    InvalidJsonUInt32,
    // containerVersion.major or document.schemaVersion.major is not 1 (decode refuses; migration is
    // out of scope of this module).
    UnsupportedMajorVersion,
    // `format` is not exactly "org.kinetik.bloom.project".
    InvalidFormat,
    // `document.path` is not exactly "document.json".
    InvalidDocumentPath,
    // A requirement's `providerId` does not match the namespaced-ID lexical domain
    // `[a-z0-9][a-z0-9._-]{0,127}`.
    InvalidProviderId,
    // A requirement's `capabilityId` does not match the namespaced-ID lexical domain.
    InvalidCapabilityId,
    // Requirements are not sorted by providerId, then capabilityId, then schema major/minor.
    InvalidRequirementOrder,
    // Two requirements declare the same (providerId, capabilityId) pair.
    DuplicateRequirementIdentity,
    // A `providedNodeTypeIds` entry does not match the namespaced-ID lexical domain.
    InvalidProvidedNodeTypeId,
    // `providedNodeTypeIds` is not sorted by UTF-8 bytes.
    InvalidProvidedNodeTypeOrder,
    // Two entries in the same requirement's `providedNodeTypeIds` are identical.
    DuplicateProvidedNodeTypeId,
};

enum class ManifestDecodeOutcome : std::uint8_t {
    // decodeManifestEnvelope() found a typed decode error; error()/path() are valid.
    Failed,
    // A DecodedManifest was constructed; value() is non-null. Only possible for an exact
    // containerVersion {1,0}.
    Decoded,
    // containerVersion is same-major newer-minor ({1, minor > 0}); no DecodedManifest is
    // constructed (value() is null) and no typed error is reported (error() is None). path() names
    // "/containerVersion/minor". See the file comment above for why this is unconditional rather
    // than contingent on whether an unrecognized member is actually present.
    PreservationRequired,
};

// A bounded diagnostic path to one JSON member, formatted as `/key/0/key`, mirroring
// DocumentDecodePathText's shape and truncation contract.
class ManifestDecodePathText final {
  public:
    constexpr ManifestDecodePathText() noexcept = default;

    [[nodiscard]] static ManifestDecodePathText from(std::string_view text) noexcept;

    [[nodiscard]] std::string_view view() const& noexcept { return {chars_.data(), size_}; }
    [[nodiscard]] std::string_view view() const&& = delete;
    [[nodiscard]] bool truncated() const noexcept { return truncated_; }

  private:
    std::array<char, 512> chars_{};
    std::uint16_t size_ = 0;
    bool truncated_ = false;
};

// [[nodiscard]] failure-aware result. outcome() names which of the three shapes is populated:
//   - Failed: error()/path() are valid.
//   - Decoded: value() is non-null.
//   - PreservationRequired: value() is null; error() is None; path() is valid.
// operator bool() is true only for Decoded, so every `if (!decoded)` / `if (decoded)` caller treats
// PreservationRequired like Failed (neither produces value()) without needing to know the outcome
// enum exists.
class [[nodiscard]] ManifestDecodeResult final {
  public:
    [[nodiscard]] static ManifestDecodeResult success(DecodedManifest manifest);
    [[nodiscard]] static ManifestDecodeResult failure(ManifestDecodeError error,
                                                      std::string_view path);
    [[nodiscard]] static ManifestDecodeResult preservationRequired(std::string_view path);

    [[nodiscard]] explicit operator bool() const noexcept {
        return outcome_ == ManifestDecodeOutcome::Decoded;
    }
    [[nodiscard]] ManifestDecodeOutcome outcome() const noexcept { return outcome_; }
    [[nodiscard]] ManifestDecodeError error() const noexcept { return error_; }
    [[nodiscard]] std::string_view path() const noexcept { return path_.view(); }
    [[nodiscard]] bool pathTruncated() const noexcept { return path_.truncated(); }
    [[nodiscard]] const DecodedManifest* value() const& noexcept {
        return manifest_.has_value() ? &*manifest_ : nullptr;
    }
    [[nodiscard]] const DecodedManifest* value() const&& = delete;

  private:
    ManifestDecodeResult() = default;

    std::optional<DecodedManifest> manifest_;
    ManifestDecodeOutcome outcome_ = ManifestDecodeOutcome::Failed;
    ManifestDecodeError error_ = ManifestDecodeError::None;
    ManifestDecodePathText path_;
};

// Decodes `root` (the manifest.json root value from a parsed StrictJsonDomDocument) into a
// DecodedManifest. `root` must outlive the call; nothing is retained past return. See the
// file-level comment above for exact scope. May throw std::bad_alloc; never throws otherwise.
[[nodiscard]] ManifestDecodeResult decodeManifestEnvelope(const JsonValue& root);

} // namespace bloom::project
