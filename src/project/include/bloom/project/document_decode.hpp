#pragma once

#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/animation.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/extension_records.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/project/round_trip_state.hpp>
#include <bloom/project/strict_json_dom.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Typed decode of the document.json envelope from a parsed strict JSON DOM (see
// docs/architecture/project-format.md, "Canonical Document Shape", "Project And Composition",
// "Project Color Settings And OCIO Reference", "Parameters", "Animation", and "Canonical Graph").
// This is the schema-specific validation layer the format contract requires: a generic JSON
// Schema validator can check structure and lexical bounds, but only typed decode establishes
// canonical acceptance (exact member order, canonical decimal/rational spellings, frozen v1 value
// domains, and cross-field agreement such as portability/locator family).
//
// Scope (R2): the document envelope and every durable value inside a composition -- schemaVersion,
// project id/name/colorSettings, and per-composition id/name/duration/format/parameters/
// animationCurves/graph. A composition object is now closed: it must contain exactly those seven
// members in exact order, and an unrecognized or trailing member is a typed decode error. Within a
// composition, cross-references are checked against records decoded elsewhere in that same
// composition -- a parameter binding's parameterId, an animation-curve source's curveId, and every
// edge/Layer Output/Layer Stack/compositionOutput node id must each name a record this module
// itself decoded; an unresolved reference is DanglingReference. Cross-composition and
// project-level references (e.g. a future extension-record subject) remain out of scope.
// idAllocation.highestIssued and every extension record are also fully decoded (R3): the closed
// ten-member highestIssued object into document::IdAllocatorHighWater, and the extensions array --
// sorted, duplicate-free by numeric ExtensionRecordId -- into document::ExtensionRecord values
// (typed subject/target kinds, all three reference-policy shapes, and the base64 payload decoded
// through canonical_base64.hpp). This module still deliberately does not construct
// bloom::document::Project or bloom::document::Document: reassembling the live model (which
// additionally enforces document-construction invariants such as expected node/schema bindings,
// Layer Stack membership, cycle freedom, extension subject/reference-target existence -- see
// bloom::document::CanonicalGraph::validate() and bloom::document::validateExtensionRecords() --
// and restoring the id allocator from the decoded highWater) is bloom::project::reconstructDocument
// in document_reconstruct.hpp.
//
// RT1: unknown-additive-member capture for a same-major newer-minor document (schemaVersion
// {1, minor > 0}) is implemented; the writer-side overlay that reconciles retained data back onto
// a canonical rewrite remains a later slice (see docs/architecture/project-format.md, "Versions,
// Migrations, And Preservation"). An exact {1,0} document keeps the original strict behavior: an
// unrecognized member anywhere is a typed UnknownMember/MemberOutOfOrder decode error and no
// RoundTripState is ever produced. A {1, minor > 0} document additionally accepts a trailing
// unknown member on any closed object -- but only strictly after every known member of that same
// object, in ascending UTF-8 key order -- and captures it into bloom::project::RoundTripState
// (round_trip_state.hpp) keyed by its schema path or, for a collection element, the contract's
// fixed stable identity. An unknown member positioned before or between known members remains a
// hard decode error at every minor version: a conforming writer could not have produced it, so
// RT1 does not attempt to preserve it. An unknown *discriminator* (e.g. a `kind` string outside a
// closed vocabulary such as an OCIO locator/parameter-source/constant-value/animation-curve/
// edge-destination/extension-target/reference-policy kind) and an unknown JSON number outside the
// lossless subset in unknown_json_number.hpp are not decode errors either: they classify the
// whole document as DocumentDecodeOutcome::PreservedReadOnlyRequired instead of producing a
// DecodedDocumentEnvelope (see decodeDocumentEnvelope()'s three-way result below).
//
// Decoding constructs every typed value through its existing checked surface: canonical_decimal.hpp
// parsers for decimal-string ids/rationals/int64s, canonical JSON-number uint32 fields, and known
// Float64 members (parseKnownFloat64, which accepts every RFC 8259 spelling that rounds to a
// finite binary64 value and rejects overflow to infinity); core::RationalTime::create (via
// parseCanonicalRationalTime), core::PixelAspectRatio::create (via
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

// One decoded composition's canonical graph's Layer Stack: the stable node id owning it, and its
// entries in exact source order (Layer Stack entries are never canonically sorted; see
// docs/architecture/project-format.md, "Canonical Graph"). Deliberately a plain mirror of
// bloom::document::LayerStack rather than that class: LayerStack's constructor and append() apply
// live-document invariants (duplicate slot/layer rejection) that are a later
// document-construction concern, not this package's wire-shape decode.
struct DecodedLayerStack final {
    document::NodeId nodeId;
    std::vector<document::LayerStackEntry> entries;

    friend bool operator==(const DecodedLayerStack&, const DecodedLayerStack&) = default;
};

// One decoded composition's canonical graph. A plain mirror of bloom::document::CanonicalGraph
// rather than that class: CanonicalGraph has no default constructor (it always owns a Layer Stack
// node id) and its addNode()/addEdge()/addLayerOutput() apply live-document invariants
// (schema-specific expected bindings, Layer Stack/Layer Output membership, cycle freedom, ...)
// that are a later document-construction concern. bloom::document::NodeRecord, EdgeRecord, and
// LayerOutputBoundary are plain aggregate structs with no such invariants in their own
// construction, so they are reused directly.
struct DecodedGraph final {
    std::vector<document::NodeRecord> nodes;
    std::vector<document::EdgeRecord> edges;
    std::vector<document::LayerOutputBoundary> layerOutputs;
    DecodedLayerStack layerStack;
    document::OutputPortRef compositionOutput;

    friend bool operator==(const DecodedGraph&, const DecodedGraph&) = default;
};

// One decoded composition's durable values.
struct DecodedComposition final {
    document::CompositionId id;
    std::string name;
    core::RationalTime duration;
    document::CompositionFormat format;
    // bloom::document::ParameterRecord and bloom::document::AnimationCurveRecord are plain
    // aggregate/variant value types with no invariant-enforcing constructor of their own, so they
    // are reused directly rather than through bloom::document::ParameterStore/AnimationCurveStore
    // (whose insert() applies live-document invariants -- schema/value agreement, curve ownership,
    // ... -- that are a later document-construction concern).
    std::vector<document::ParameterRecord> parameters;
    std::vector<document::AnimationCurveRecord> animationCurves;
    DecodedGraph graph;

    friend bool operator==(const DecodedComposition&, const DecodedComposition&) = default;
};

// The decoded document.json envelope's durable values. Deliberately not a
// bloom::document::Project: reconstructing the live model (restoring the id allocator and applying
// document-construction invariants beyond this module's wire-shape and within-composition
// cross-reference checks) is bloom::project::reconstructDocument's job (see
// document_reconstruct.hpp), not this module's. idAllocation.highestIssued and every extension
// record are fully decoded here (R3): highWater mirrors document::IdAllocatorHighWater exactly, and
// extensionRecords reuses document::ExtensionRecord directly -- like
// ParameterRecord/NodeRecord/..., ExtensionRecord is a plain aggregate/variant value type with no
// invariant-enforcing constructor of its own, so this module's decode only checks wire shape
// (closed member order, canonical id/base64 spelling, sorted+unique record ids) and leaves
// cross-reference/subject-existence/reference-policy target-existence checks to
// bloom::document::Project::validate() during reconstruction.
struct DecodedDocumentEnvelope final {
    document::ProjectId projectId;
    std::string projectName;
    document::ColorSettings colorSettings;
    std::vector<DecodedComposition> compositions;
    document::IdAllocatorHighWater highWater;
    std::vector<document::ExtensionRecord> extensionRecords;

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
    // "sha256", an OCIO context variable with an invalid name/value or an unsorted/duplicate name,
    // or a graph node schemaVersion of zero. These are reported through
    // bloom::document::ColorSettings::validate()/OcioConfigReference::validate() or an inline
    // domain check rather than being re-implemented as a dedicated typed error.
    DomainViolation,
    // A constant value's numeric JSON number does not round to a finite binary64 value (exact
    // overflow to infinity) or is otherwise not an accepted Float64 spelling.
    InvalidFloat64,
    // A constant int64 value's decimal-string spelling is non-canonical or does not fit int64.
    InvalidInt64Value,
    // A parameter source's `kind` discriminator is not "constant" or "animation-curve" -- for
    // example the deferred "driver-binding" wire vocabulary (see
    // docs/architecture/project-format.md, "Parameters").
    UnsupportedParameterSource,
    // A constant value's `kind` discriminator is not one of the seven known v1 value kinds.
    InvalidConstantValueKind,
    // Parameters are not sorted by strictly ascending numeric ParameterId.
    UnsortedParameters,
    // Two parameters declare the same numeric ParameterId.
    DuplicateParameter,
    // Animation curves are not sorted by strictly ascending numeric AnimationCurveId.
    UnsortedAnimationCurves,
    // Two animation curves declare the same numeric AnimationCurveId.
    DuplicateAnimationCurve,
    // An animation curve's `kind` discriminator is not "scalar" or "vec2".
    InvalidAnimationCurveKind,
    // An animation curve's keyframes array is empty.
    EmptyKeyframes,
    // Consecutive keyframes are not in strictly increasing exact rational time.
    NonIncreasingKeyframeTime,
    // A keyframe's `outgoingInterpolation` is not "hold" or "linear".
    InvalidInterpolation,
    // An animation curve's final keyframe interpolation is not canonical "linear".
    FinalKeyframeNotLinear,
    // Graph nodes are not sorted by strictly ascending numeric NodeId.
    UnsortedNodes,
    // Two graph nodes declare the same numeric NodeId.
    DuplicateNode,
    // A node's parameter bindings are not sorted by UTF-8 role then numeric ParameterId.
    UnsortedBindings,
    // Two parameter bindings on the same node declare the same role.
    DuplicateBinding,
    // Graph edges are not sorted by strictly ascending numeric EdgeId.
    UnsortedEdges,
    // Two graph edges declare the same numeric EdgeId.
    DuplicateEdge,
    // An edge destination's `kind` discriminator is not "node-input" or "layer-stack-input".
    InvalidEdgeDestinationKind,
    // Layer Output boundaries are not sorted by numeric LayerId then numeric NodeId.
    UnsortedLayerOutputs,
    // Two Layer Output boundaries declare the same (LayerId, NodeId) pair.
    DuplicateLayerOutput,
    // A parameter binding's parameterId, an animation-curve source's curveId, or an
    // edge/Layer Output/Layer Stack/compositionOutput node id does not name a record decoded
    // elsewhere in this same composition.
    DanglingReference,
    // An idAllocation.highestIssued member's decimal-string spelling is non-canonical (leading
    // zero, a `+`/`-` sign, non-digit characters) or out of uint64 range. Unlike InvalidId, zero is
    // a valid high-water spelling here (see docs/architecture/project-format.md, "Inclusive
    // Allocator State").
    InvalidAllocatorHighWater,
    // Extension records are not sorted by strictly ascending numeric ExtensionRecordId.
    UnsortedExtensionRecords,
    // Two extension records declare the same numeric ExtensionRecordId.
    DuplicateExtensionRecord,
    // An extension record subject's or a host-table reference target's `kind` discriminator is not
    // one of the nine known v1 typed target kinds.
    InvalidExtensionTargetKind,
    // An extension record's `referencePolicy.kind` discriminator is not "none", "host-table", or
    // "owner-remapper".
    InvalidReferencePolicyKind,
    // An extension record's `payload` is not a canonical RFC 4648 standard-alphabet base64 spelling
    // with required `=` padding, correct length, and zero tail bits.
    InvalidBase64Payload,
    // (RT1, minor > 0 only) A closed object's trailing unknown additive members are not in strictly
    // ascending UTF-8 key order (or, structurally impossible through the strict JSON reader's own
    // duplicate-key rejection, repeat a key) -- see docs/architecture/project-format.md, "Canonical
    // Document Shape" and "Versions, Migrations, And Preservation": "retained unknown object
    // members" sort by "UTF-8 key after known members". Such input could not have come from a
    // conforming writer.
    UnsortedUnknownMember,
};

// (RT1) Why a same-major newer-minor document could not be opened editable even though every
// unknown member appeared in a position a conforming writer could have produced (see
// docs/architecture/project-format.md, "Versions, Migrations, And Preservation": "unknown core
// discriminators are never guessed").
enum class RoundTripPreservationReason : std::uint8_t {
    // No PreservedReadOnlyRequired outcome occurred; not a valid reason on its own.
    None,
    // A `kind` (or other closed-vocabulary discriminator) string did not match any family this
    // schema major recognizes -- e.g. a future OCIO locator kind, parameter-source kind (a future
    // driver declaration), constant-value kind, animation-curve kind, edge-destination kind,
    // extension target/subject kind, or extension reference-policy kind. The discriminator string
    // itself is core vocabulary, never captured as a retained value.
    UnknownDiscriminatorKind,
    // A JSON number reachable from an unknown additive member's value is outside the lossless
    // editable subset in unknown_json_number.hpp (see docs/architecture/project-format.md,
    // "Unknown JSON Numbers"): an out-of-range integer, a non-canonical exponent/decimal spelling,
    // or a decimal that would round or normalize on rewrite.
    UnknownNumberOutOfSubset,
};

// (RT1) The outcome of decoding a document.json envelope whose schemaVersion is {1, minor}: a
// distinct classification from "hard decode error" for a same-major newer-minor document that is
// structurally valid but cannot (yet, or ever, for that exact archive) be opened editable. See
// docs/architecture/project-format.md, "Project I/O Boundary" (Editable/DegradedEditable/
// PreservedReadOnly) -- this decode-layer enum is narrower: it only distinguishes whether this
// module could construct a DecodedDocumentEnvelope at all, not the full application-level
// editability determination (which also depends on unavailable-provider state this module never
// sees).
enum class DocumentDecodeOutcome : std::uint8_t {
    // decodeDocumentEnvelope() found a typed decode error; error()/path() are valid.
    Failed,
    // A DecodedDocumentEnvelope was constructed; value() is non-null. classification()
    // distinguishes an exact {1,0} document (no RoundTripState; roundTrip() is nullptr) from a
    // same-major newer-minor document with only safely additive unknown members
    // (EditableWithRoundTrip; roundTrip() is non-null, though it may be empty()).
    Decoded,
    // A same-major newer-minor document contains an unknown core discriminator or an unknown
    // number outside the lossless subset. No DecodedDocumentEnvelope is constructed; value() is
    // null. preservationReason()/path() are valid.
    PreservedReadOnlyRequired,
};

enum class DocumentClassification : std::uint8_t {
    // Not meaningful unless outcome() == Decoded.
    None,
    // schemaVersion is exactly {1, 0}; no unknown additive members are possible (the 1.0 writer
    // never emits them) and no RoundTripState is produced.
    ExactSchemaV1_0,
    // schemaVersion is {1, minor > 0} and every unknown additive member encountered was safely
    // captured; roundTrip() names the (possibly empty) retained state.
    EditableWithRoundTrip,
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

// [[nodiscard]] failure-aware result (RT1: now three-way, see DocumentDecodeOutcome above).
// outcome() names which of the three shapes is populated:
//   - Failed: error()/path() are valid.
//   - Decoded: value() is non-null; classification() distinguishes ExactSchemaV1_0 (roundTrip()
//     is nullptr) from EditableWithRoundTrip (roundTrip() is non-null).
//   - PreservedReadOnlyRequired: value() is null; preservationReason()/path() are valid.
// operator bool() keeps its pre-RT1 meaning ("a trusted envelope was constructed") so every
// existing `if (!decoded)` / `if (decoded)` caller keeps working unchanged for the two outcomes
// it already knew about; PreservedReadOnlyRequired is also "falsy" like Failed, since neither
// produces value().
class [[nodiscard]] DocumentDecodeResult final {
  public:
    [[nodiscard]] static DocumentDecodeResult success(DecodedDocumentEnvelope envelope);
    [[nodiscard]] static DocumentDecodeResult successWithRoundTrip(DecodedDocumentEnvelope envelope,
                                                                   RoundTripState roundTrip);
    [[nodiscard]] static DocumentDecodeResult failure(DocumentDecodeError error,
                                                      std::string_view path);
    [[nodiscard]] static DocumentDecodeResult
    preservedReadOnlyRequired(RoundTripPreservationReason reason, std::string_view path);

    [[nodiscard]] explicit operator bool() const noexcept {
        return outcome_ == DocumentDecodeOutcome::Decoded;
    }
    [[nodiscard]] DocumentDecodeOutcome outcome() const noexcept { return outcome_; }
    [[nodiscard]] DocumentClassification classification() const noexcept { return classification_; }
    [[nodiscard]] DocumentDecodeError error() const noexcept { return error_; }
    [[nodiscard]] RoundTripPreservationReason preservationReason() const noexcept {
        return preservationReason_;
    }
    [[nodiscard]] std::string_view path() const noexcept { return path_.view(); }
    [[nodiscard]] bool pathTruncated() const noexcept { return path_.truncated(); }
    [[nodiscard]] const DecodedDocumentEnvelope* value() const& noexcept {
        return envelope_.has_value() ? &*envelope_ : nullptr;
    }
    [[nodiscard]] const DecodedDocumentEnvelope* value() const&& = delete;
    // Non-null only when classification() == EditableWithRoundTrip (may then still be empty()
    // when the document had no unknown additive members at all).
    [[nodiscard]] const RoundTripState* roundTrip() const& noexcept {
        return roundTrip_.has_value() ? &*roundTrip_ : nullptr;
    }
    [[nodiscard]] const RoundTripState* roundTrip() const&& = delete;

  private:
    DocumentDecodeResult() = default;

    std::optional<DecodedDocumentEnvelope> envelope_;
    std::optional<RoundTripState> roundTrip_;
    DocumentDecodeOutcome outcome_ = DocumentDecodeOutcome::Failed;
    DocumentClassification classification_ = DocumentClassification::None;
    DocumentDecodeError error_ = DocumentDecodeError::None;
    RoundTripPreservationReason preservationReason_ = RoundTripPreservationReason::None;
    DocumentDecodePathText path_;
};

// Decodes `root` (the document.json root value from a parsed StrictJsonDomDocument) into a
// DecodedDocumentEnvelope. `root` must outlive the call; nothing is retained past return. See the
// file-level comment above for exact scope. May throw std::bad_alloc; never throws otherwise.
[[nodiscard]] DocumentDecodeResult decodeDocumentEnvelope(const JsonValue& root);

} // namespace bloom::project
