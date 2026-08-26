#ifndef BLOOM_PROJECT_DOCUMENT_DECODE_INTERNAL_HPP
#define BLOOM_PROJECT_DOCUMENT_DECODE_INTERNAL_HPP

#include <bloom/core/rational_time.hpp>
#include <bloom/project/canonical_decimal.hpp>
#include <bloom/project/document_decode.hpp>
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
struct DecodeState final {
    DocumentDecodeError error = DocumentDecodeError::None;
    std::string path;

    void fail(DocumentDecodeError newError, std::string newPath) noexcept {
        if (error == DocumentDecodeError::None) {
            error = newError;
            path = std::move(newPath);
        }
    }
};

[[nodiscard]] std::string joinPath(const std::string& base, std::string_view segment);
[[nodiscard]] std::string joinPathIndex(const std::string& base, std::size_t index);

// Translates a bloom::project::CanonicalDecimalField into this module's slash-separated path
// convention, appended to `base`.
[[nodiscard]] std::string fieldPath(const std::string& base, CanonicalDecimalField field);
[[nodiscard]] DocumentDecodeError mapRationalError(CanonicalDecimalError error) noexcept;

// Checks that `object` is a JSON object whose leading members exactly match `expectedKeys` in
// order, filling `outValues` with pointers to each matched member's value. When
// `rejectExtraMembers` is true the object must contain exactly `expectedKeys.size()` members;
// otherwise trailing members beyond the matched prefix are accepted without inspection.
[[nodiscard]] bool matchOrderedMembers(const JsonValue& object,
                                       std::span<const std::string_view> expectedKeys,
                                       bool rejectExtraMembers, DecodeState& state,
                                       const std::string& basePath,
                                       std::vector<const JsonValue*>& outValues);

[[nodiscard]] bool decodeStringMember(const JsonValue& value, DecodeState& state,
                                      const std::string& path, std::string_view& out) noexcept;

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
