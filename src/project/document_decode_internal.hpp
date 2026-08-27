#ifndef BLOOM_PROJECT_DOCUMENT_DECODE_INTERNAL_HPP
#define BLOOM_PROJECT_DOCUMENT_DECODE_INTERNAL_HPP

#include <bloom/core/rational_time.hpp>
#include <bloom/project/canonical_decimal.hpp>
#include <bloom/project/document_decode.hpp>
#include <bloom/project/round_trip_state.hpp>
#include <bloom/project/strict_json_dom.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Decode plumbing shared between document_decode.cpp (the document envelope, project, and
// composition scalar members) and document_decode_composition.cpp (the composition interior:
// parameters, animation curves, and the canonical graph -- split out because document_decode.cpp
// would otherwise exceed this codebase's file-size convention). Not a public Project I/O contract:
// only these two translation units include this header, matching the existing
// canonical_json_string_detail.hpp precedent for a private cross-file implementation seam.
namespace bloom::project::detail {

// Threads a first-failure-wins typed error and its exact member path through the recursive decode
// walk, mirroring the WalkState/EmitState pattern used by the canonical document writer.
//
// RT1: also threads the same-major newer-minor unknown-additive-member capture machinery. This
// is deliberately a *second*, independent first-wins latch (preservedReadOnlyRequired) rather
// than reusing `error`/`fail()`: a decode function that hits an unknown discriminator or an
// unknown out-of-subset number returns false to unwind exactly like a hard decode error (so every
// existing `if (!decodeX(...)) return false;` call site needs no change), but
// decodeDocumentEnvelope() must be able to tell the two latches apart afterward to build the right
// three-way DocumentDecodeResult. Only one of the two latches is ever set for one decode call:
// every site that sets one returns false immediately, so the recursive walk never reaches a second
// failure site once either has fired.
struct DecodeState final {
    DocumentDecodeError error = DocumentDecodeError::None;
    std::string path;

    // RT1 capture state. `documentMinor` is set once (right after schemaVersion is confirmed
    // major == 1) and never changes afterward; it gates every capture decision below ("when, and
    // only when, the document minor is > 0"). `roundTrip` is non-null exactly when
    // documentMinor > 0, and every capture/attach call below is a no-op unless it is.
    std::uint32_t documentMinor = 0;
    RoundTripState* roundTrip = nullptr;
    // The running attachment path from the document root to whatever object is currently being
    // decoded, in the same identity-keyed shape RoundTripState::attach() expects (see
    // round_trip_state.hpp's file comment). Pushed/popped by AttachmentScope (RAII) at every
    // nested singleton member and collection-element decode site.
    RoundTripAttachmentPath attachmentPath;

    bool preservedReadOnlyRequired = false;
    RoundTripPreservationReason preservationReason = RoundTripPreservationReason::None;
    std::string preservationPath;

    void fail(DocumentDecodeError newError, std::string newPath) noexcept {
        if (error == DocumentDecodeError::None) {
            error = newError;
            path = std::move(newPath);
        }
    }

    // Latches an unknown-core-discriminator or unknown-out-of-subset-number classification
    // (first-wins, and only if no hard error already latched -- see the struct comment above for
    // why the two never both apply to the same decode outcome in practice).
    void requirePreservedReadOnly(RoundTripPreservationReason reason,
                                  std::string newPath) noexcept {
        if (error == DocumentDecodeError::None && !preservedReadOnlyRequired) {
            preservedReadOnlyRequired = true;
            preservationReason = reason;
            preservationPath = std::move(newPath);
        }
    }
};

// RAII scope that pushes one attachment-path segment onto `state.attachmentPath` for the
// duration of decoding one nested singleton member or collection element, and pops it back off
// on scope exit (including every early-return failure path, since this is destructor-driven).
// Constructing a scope is always safe even when documentMinor == 0 / capture is inactive: the
// attachment path is simply unused in that case.
class AttachmentScope final {
  public:
    // Pushes a named (schema-path) segment, e.g. "colorSettings", "ocioConfig", "format".
    AttachmentScope(DecodeState& state, std::string_view name);
    // Pushes one collection element's fixed stable-identity segment, e.g.
    // (Composition, "7") or (ParameterBinding, "opacity").
    AttachmentScope(DecodeState& state, RoundTripCollectionKind kind, std::string identity);

    AttachmentScope(const AttachmentScope&) = delete;
    AttachmentScope& operator=(const AttachmentScope&) = delete;
    AttachmentScope(AttachmentScope&&) = delete;
    AttachmentScope& operator=(AttachmentScope&&) = delete;
    ~AttachmentScope();

  private:
    DecodeState& state_;
};

[[nodiscard]] std::string joinPath(const std::string& base, std::string_view segment);
[[nodiscard]] std::string joinPathIndex(const std::string& base, std::size_t index);

// Translates a bloom::project::CanonicalDecimalField into this module's slash-separated path
// convention, appended to `base`.
[[nodiscard]] std::string fieldPath(const std::string& base, CanonicalDecimalField field);
[[nodiscard]] DocumentDecodeError mapRationalError(CanonicalDecimalError error) noexcept;

// Checks that `object` is a JSON object whose leading members exactly match `expectedKeys` in
// order, filling `outValues` with pointers to each matched member's value. When
// `rejectExtraMembers` is false, trailing members beyond the matched prefix are accepted without
// inspection (used only by decodeAnimationCurve's id/kind discriminator prefix match).
//
// When `rejectExtraMembers` is true and trailing members beyond the matched prefix exist: at
// documentMinor == 0 (or exact schemaVersion {1,0}), this is always the pre-RT1 hard
// DocumentDecodeError::UnknownMember at the first trailing member's path -- a 1.0 writer never
// emits one. At documentMinor > 0, the trailing members are instead a capture candidate: this
// function copies each one into a bounded RetainedJsonValue (routing every nested JSON number
// through parseUnknownJsonNumber -- see unknown_json_number.hpp -- and calling
// state.requirePreservedReadOnly(UnknownNumberOutOfSubset, ...) the first time one falls outside
// the lossless subset), and requires the trailing keys to already be in strictly ascending UTF-8
// order (state.fail(UnsortedUnknownMember, ...) otherwise -- a conforming newer-minor writer
// always emits retained members in that order, see "Canonical Document Shape"). On success with a
// non-empty capture, the retained members are appended to `outCapturedTrailing` in that same
// ascending order; the caller (which alone knows this object's exact attachment point -- a
// schema-path AttachmentScope is already active for a singleton, or a collection element's
// stable identity is decoded from `outValues` immediately after this call returns) is responsible
// for calling state.roundTrip->attach() once its attachment path is complete. This function never
// attaches on its own: at the moment it runs for a collection element's own closed shape (e.g. a
// composition object's trailing members), that element's own identity has not been decoded yet.
[[nodiscard]] bool matchOrderedMembers(const JsonValue& object,
                                       std::span<const std::string_view> expectedKeys,
                                       bool rejectExtraMembers, DecodeState& state,
                                       const std::string& basePath,
                                       std::vector<const JsonValue*>& outValues,
                                       std::vector<RetainedJsonMember>& outCapturedTrailing);

// Convenience overload for the common case: this object's attachment point is already exactly
// `state.attachmentPath` (an AttachmentScope for it -- named or collection-element -- was already
// pushed by the caller before this call). Any non-empty capture is attached immediately at that
// current path; the caller has nothing further to do. Not usable for a collection element's own
// closed shape (id/role/key/... has not been decoded yet at this point, so no scope for *this*
// element can exist yet) -- those sites must use the six-argument overload above instead.
[[nodiscard]] bool matchOrderedMembers(const JsonValue& object,
                                       std::span<const std::string_view> expectedKeys,
                                       bool rejectExtraMembers, DecodeState& state,
                                       const std::string& basePath,
                                       std::vector<const JsonValue*>& outValues);

// Not noexcept: DecodeState::fail() takes its path argument by value, so a failing call here copies
// `path` into that by-value parameter -- an allocation that can throw std::bad_alloc. Marking this
// noexcept while it allocates on the failure path would be an untruthful noexcept claim.
[[nodiscard]] bool decodeStringMember(const JsonValue& value, DecodeState& state,
                                      const std::string& path, std::string_view& out);

[[nodiscard]] bool decodeUInt32Member(const JsonValue& value, DecodeState& state,
                                      const std::string& path, std::uint32_t maximum,
                                      std::uint32_t& out);

// The general canonical rational-time wire shape ({"numerator","denominator"} signed decimal
// strings, reduced, positive denominator, canonical zero) used both by a composition's `duration`
// and, unadorned, by a `rational` constant value and a keyframe's `time` (see
// docs/architecture/project-format.md, "Decimal Strings And JSON Integers" and "Animation").
[[nodiscard]] bool decodeRationalTimeValue(const JsonValue& node, DecodeState& state,
                                           const std::string& path, core::RationalTime& out);

// Reads an object's first member, requires it to be literally named "kind", and decodes its
// string value as the discriminator for a branch the caller resolves afterward (constant value,
// parameter source, edge destination, OCIO locator).
[[nodiscard]] bool decodeKindDiscriminator(const JsonValue& node, DecodeState& state,
                                           const std::string& path, std::string_view& kindText);

// Shared fallback for every closed-vocabulary discriminator's "no branch matched" case (OCIO
// locator kind, parameter-source kind, constant-value kind, animation-curve kind, edge-destination
// kind, extension target/subject kind, extension reference-policy kind). At documentMinor == 0 (or
// exact schemaVersion {1,0}) this is always the pre-RT1 hard `exactVersionError` at `kindPath` --
// the discriminator vocabulary is fully known and closed for 1.0. At documentMinor > 0, an unknown
// discriminator string is not a decode error at all: the string itself is core vocabulary (never
// captured as a retained value -- see docs/architecture/project-format.md, "Versions, Migrations,
// And Preservation": "unknown core discriminators are never guessed"), so this instead latches
// state.requirePreservedReadOnly(UnknownDiscriminatorKind, kindPath). Always returns false so
// every call site can `return failUnknownDiscriminator(...);`.
[[nodiscard]] bool failUnknownDiscriminator(DecodeState& state, std::string kindPath,
                                            DocumentDecodeError exactVersionError);

// Every typed 64-bit object id in schema `1.0` shares one wire shape: an unsigned decimal string
// matching `[1-9][0-9]*` that fits uint64, with zero invalid (see
// docs/architecture/project-format.md, "Decimal Strings And JSON Integers"). One template covers
// every ProjectId/CompositionId/NodeId/EdgeId/LayerId/LayerSlotId/ParameterId/AnimationCurveId/
// KeyframeId decode site.
template <typename IdType>
[[nodiscard]] bool decodeObjectId(const JsonValue& value, DecodeState& state,
                                  const std::string& path, IdType& out) {
    std::string_view text;
    if (!decodeStringMember(value, state, path, text)) {
        return false;
    }
    const auto parsed = parseCanonicalObjectId(text);
    if (!parsed) {
        state.fail(DocumentDecodeError::InvalidId, path);
        return false;
    }
    out = IdType::fromRaw(*parsed.value());
    return true;
}

// Decodes a composition's already-order-checked `parameters`, `animationCurves`, and `graph`
// members (see document_decode.cpp's decodeComposition, which matches the composition object's
// exact seven-member shape before delegating here). Cross-reference checks that need more than
// one decoded collection (a parameter's curveId, a node binding's parameterId, and every
// edge/layerOutput/layerStack/compositionOutput node id) run here too, once every collection they
// reference is fully decoded. Defined in document_decode_composition.cpp.
[[nodiscard]] bool
decodeCompositionInterior(const JsonValue& parametersNode, const JsonValue& animationCurvesNode,
                          const JsonValue& graphNode, DecodeState& state,
                          const std::string& parametersPath, const std::string& curvesPath,
                          const std::string& graphPath,
                          std::vector<document::ParameterRecord>& parameters,
                          std::vector<document::AnimationCurveRecord>& curves, DecodedGraph& graph);

} // namespace bloom::project::detail

#endif // BLOOM_PROJECT_DOCUMENT_DECODE_INTERNAL_HPP
