#pragma once

#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/project/strict_json_dom.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Typed decode of the document.json envelope from a parsed strict JSON DOM (see
// docs/architecture/project-format.md, "Canonical Document Shape", "Project And Composition", and
// "Project Color Settings And OCIO Reference"). This is the schema-specific validation layer the
// format contract requires: a generic JSON Schema validator can check structure and lexical
// bounds, but only typed decode establishes canonical acceptance (exact member order, canonical
// decimal/rational spellings, frozen v1 value domains, and cross-field agreement such as
// portability/locator family).
//
// Scope (R1): the document envelope and its non-graph durable values only -- schemaVersion,
// project id/name/colorSettings, and per-composition id/name/duration/format. Parameters,
// animation curves, the canonical graph, idAllocation content, and extension records are not
// decoded here; composition objects are matched only on their required id/name/duration/format
// prefix so a real writer-produced document (which always continues with parameters,
// animationCurves, graph) still decodes. idAllocation and extensions are checked only for
// presence, position, and JSON kind. This module deliberately does not construct
// bloom::document::Project or bloom::document::Document: reassembling the live model and
// restoring the id allocator from idAllocation.highestIssued is a later slice. The unknown-member
// round-trip overlay is also a later slice, so an unrecognized root member is a typed decode
// error rather than being preserved.
//
// Decoding constructs every typed value through its existing checked surface: canonical_decimal.hpp
// parsers for decimal-string ids/rationals and canonical JSON-number uint32 fields,
// core::RationalTime::create (via parseCanonicalRationalTime), core::PixelAspectRatio::create (via
// parseCanonicalPositiveRatio/parseCanonicalPixelAspectRatio), bloom::document::FrameRate::create,
// bloom::document::CompositionFormat::create, bloom::core::Sha256Digest::fromLowercaseHex, and
// bloom::document::ColorSettings::validate()/OcioConfigReference::validate() for the OCIO
// locator-family, portability-agreement, process-color-space, and context-variable domain rules.
// No value is bypassed by hand-rolling a duplicate domain check where a checked surface already
// exists.
//
// decodeDocumentEnvelope() is not noexcept: its intermediate std::string/std::vector-backed
// results can throw std::bad_alloc on allocation failure. Marking it noexcept while it allocates
// through a throwing surface would be an untruthful noexcept claim; callers that need a
// termination-free boundary should wrap the call themselves.

namespace bloom::project {

// One decoded composition's non-graph durable values.
struct DecodedComposition final {
    document::CompositionId id;
    std::string name;
    core::RationalTime duration;
    document::CompositionFormat format;

    friend bool operator==(const DecodedComposition&, const DecodedComposition&) = default;
};

// The decoded document.json envelope's non-graph durable values. Deliberately not a
// bloom::document::Project: reconstructing the live model (graph, parameters, animation curves,
// extension records) and restoring the id allocator are out of scope for this package.
struct DecodedDocumentEnvelope final {
    document::ProjectId projectId;
    std::string projectName;
    document::ColorSettings colorSettings;
    std::vector<DecodedComposition> compositions;

    friend bool operator==(const DecodedDocumentEnvelope&,
                           const DecodedDocumentEnvelope&) = default;
};

enum class DocumentDecodeError : std::uint8_t {
    None,
    // A member's JSON value kind did not match the schema (e.g. a number where a string was
    // required, or an object where an array was required).
    WrongValueKind,
    // A required member was absent from an otherwise well-formed object.
    MissingMember,
    // An object member's key is not a member this schema/position recognizes.
    UnknownMember,
    // An object member's key is known to this schema but appeared in the wrong position; exact
    // member order is part of canonical acceptance.
    MemberOutOfOrder,
    // A typed 64-bit object id's decimal-string spelling is non-canonical (leading zero, a `+`
    // sign, non-digit characters, out of uint64 range) or is the invalid zero value.
    InvalidId,
    // A JSON-number uint32 field's token is not a canonical non-negative integer spelling
    // (fraction, exponent, leading zero, or a `+`/`-` sign).
    InvalidJsonUInt32,
    // A rational component's decimal-string spelling is non-canonical or does not fit its
    // declared signed/unsigned range.
    InvalidRationalComponent,
    // A rational pair's terms are not already reduced (gcd != 1).
    UnreducedRational,
    // A composition duration parsed successfully but is zero or negative.
    NonPositiveDuration,
    // An OCIO content-revision digest is not exactly 64 lowercase hexadecimal ASCII characters.
    InvalidDigestSpelling,
    // An OCIO locator's `kind` discriminator is not one of the four known v1 locator families.
    InvalidOcioLocatorKind,
    // Compositions are not sorted by strictly ascending numeric CompositionId.
    UnsortedCompositions,
    // Two compositions declare the same numeric CompositionId.
    DuplicateComposition,
    // A value is lexically well-formed but violates a frozen v1 domain or cross-field rule:
    // schemaVersion != 1.0, composition width/height out of 1..1048576 or zero, checked pixel
    // product > 2^32, processColorSpaceId != "lin_rec709_scene", a built-in OCIO URI other than
    // the exact immutable identity, a malformed project-relative or external OCIO locator,
    // portability disagreeing with the locator family, an OCIO revision algorithm other than
    // "sha256", or an OCIO context variable with an invalid name/value or an unsorted/duplicate
    // name. These are reported through bloom::document::ColorSettings::validate() and
    // OcioConfigReference::validate() rather than being re-implemented here.
    DomainViolation,
};

// A bounded diagnostic path to one JSON member, formatted as `/key/0/key`, mirroring
// StrictJsonDomPathText's shape and truncation contract.
class DocumentDecodePathText final {
  public:
    constexpr DocumentDecodePathText() noexcept = default;

    [[nodiscard]] static DocumentDecodePathText from(std::string_view text) noexcept;

    [[nodiscard]] std::string_view view() const& noexcept { return {chars_.data(), size_}; }
    [[nodiscard]] std::string_view view() const&& = delete;
    [[nodiscard]] bool truncated() const noexcept { return truncated_; }

  private:
    std::array<char, 512> chars_{};
    std::uint16_t size_ = 0;
    bool truncated_ = false;
};

// [[nodiscard]] failure-aware result. On success, value() names the decoded envelope; on
// failure, error() names the typed cause and path() names the exact offending member.
class [[nodiscard]] DocumentDecodeResult final {
  public:
    [[nodiscard]] static DocumentDecodeResult success(DecodedDocumentEnvelope envelope);
    [[nodiscard]] static DocumentDecodeResult failure(DocumentDecodeError error,
                                                      std::string_view path);

    [[nodiscard]] explicit operator bool() const noexcept {
        return error_ == DocumentDecodeError::None;
    }
    [[nodiscard]] DocumentDecodeError error() const noexcept { return error_; }
    [[nodiscard]] std::string_view path() const noexcept { return path_.view(); }
    [[nodiscard]] bool pathTruncated() const noexcept { return path_.truncated(); }
    [[nodiscard]] const DecodedDocumentEnvelope* value() const& noexcept {
        return envelope_.has_value() ? &*envelope_ : nullptr;
    }
    [[nodiscard]] const DecodedDocumentEnvelope* value() const&& = delete;

  private:
    DocumentDecodeResult() = default;

    std::optional<DecodedDocumentEnvelope> envelope_;
    DocumentDecodeError error_ = DocumentDecodeError::None;
    DocumentDecodePathText path_;
};

// Decodes `root` (the document.json root value from a parsed StrictJsonDomDocument) into a
// DecodedDocumentEnvelope. `root` must outlive the call; nothing is retained past return. See the
// file-level comment above for exact scope. May throw std::bad_alloc; never throws otherwise.
[[nodiscard]] DocumentDecodeResult decodeDocumentEnvelope(const JsonValue& root);

} // namespace bloom::project
