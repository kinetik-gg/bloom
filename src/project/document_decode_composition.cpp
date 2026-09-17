// Composition-interior typed decode: parameters, animation curves, and the canonical graph (see
// docs/architecture/project-format.md, "Parameters", "Animation", and "Canonical Graph"). Split
// out of document_decode.cpp -- which owns the document envelope, project, and composition scalar
// members -- purely to keep one translation unit from growing past this codebase's file-size
// convention; document_decode_internal.hpp is the private cross-file seam shared by both. The
// single entry point bloom::project::detail::decodeCompositionInterior() is called from
// document_decode.cpp's decodeComposition() once the composition object's exact seven-member shape
// (id/name/duration/format/parameters/animationCurves/graph) is confirmed.
//
// Every constant value and graph value is constructed through document's own plain
// aggregate/variant types (ParameterRecord, AnimationCurveRecord, NodeRecord, EdgeRecord,
// LayerOutputBoundary, ...) rather than through ParameterStore/AnimationCurveStore/CanonicalGraph:
// those store/graph types apply live-document invariants (schema/value agreement, curve ownership,
// expected node bindings, cycle freedom, ...) that are a later document-construction concern (see
// document_decode.hpp's DecodedComposition/DecodedGraph comments). This module's own
// cross-reference checks are narrower and explicit: every parameter binding's parameterId, every
// animation-curve source's curveId, and every edge/layerOutput/layerStack/compositionOutput node id
// must each name a record decoded elsewhere in this same composition; an unresolved reference is
// DanglingReference.

#include "document_decode_internal.hpp"

#include <bloom/core/color.hpp>
#include <bloom/document/animation.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/project/canonical_decimal.hpp>
#include <bloom/project/round_trip_state.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace bloom::project {

namespace {

using detail::AttachmentScope;
using detail::decodeKindDiscriminator;
using detail::decodeObjectId;
using detail::decodeRationalTimeValue;
using detail::DecodeState;
using detail::decodeStringMember;
using detail::decodeUInt32Member;
using detail::failUnknownDiscriminator;
using detail::fieldPath;
using detail::joinPath;
using detail::joinPathIndex;
using detail::mapRationalError;
using detail::matchOrderedMembers;

using document::AnimationComponent;
using document::AnimationCurveId;
using document::AnimationCurveRecord;
using document::Color4AnimationCurve;
using document::ComponentAnimationCurve;
using document::EdgeId;
using document::EdgeRecord;
using document::InputPortRef;
using document::KeyframeInterpolation;
using document::LayerId;
using document::LayerOutputBoundary;
using document::LayerSlotId;
using document::LayerStackEntry;
using document::LayerStackInputRef;
using document::NodeId;
using document::NodeInputRef;
using document::NodeRecord;
using document::OutputPortRef;
using document::ParameterBinding;
using document::ParameterId;
using document::ParameterRecord;
using document::ParameterValue;
using document::ScalarAnimationCurve;
using document::ScalarKeyframe;
using document::Vec2AnimationCurve;
using document::Vec2d;
using document::Vec3AnimationCurve;
using document::Vec3d;

// Not noexcept: DecodeState::fail() takes its path argument by value, so a failing call here copies
// `path` into that by-value parameter -- an allocation that can throw std::bad_alloc. Marking this
// noexcept while it allocates on the failure path would be an untruthful noexcept claim (the same
// pattern fixed for detail::decodeStringMember in
// document_decode_internal.hpp/document_decode.cpp).
[[nodiscard]] bool decodeBooleanMember(const JsonValue& value, DecodeState& state,
                                       const std::string& path, bool& out) {
    if (value.kind() != JsonValueKind::Boolean) {
        state.fail(DocumentDecodeError::WrongValueKind, path);
        return false;
    }
    const auto parsed = value.asBoolean();
    if (!parsed.has_value()) {
        state.fail(DocumentDecodeError::WrongValueKind, path);
        return false;
    }
    out = *parsed;
    return true;
}

// Known Float64 fields (constant float64/vec2/color4 values and keyframe values) accept every
// RFC 8259 number spelling that rounds to a finite binary64 value; parseKnownFloat64 is the same
// checked surface the format contract's "Float64" section requires. No hand-rolled std::isfinite
// check duplicates that surface, and overflow to infinity is reported as InvalidFloat64.
[[nodiscard]] bool decodeFloat64Member(const JsonValue& value, DecodeState& state,
                                       const std::string& path, double& out) {
    if (value.kind() != JsonValueKind::Number) {
        state.fail(DocumentDecodeError::WrongValueKind, path);
        return false;
    }
    const auto token = value.asNumberToken();
    if (!token.has_value()) {
        state.fail(DocumentDecodeError::WrongValueKind, path);
        return false;
    }
    const auto parsed = parseKnownFloat64(*token);
    if (!parsed) {
        state.fail(DocumentDecodeError::InvalidFloat64, path);
        return false;
    }
    out = *parsed.value();
    return true;
}

// ------------------------------------------------------------------------------------------
// Parameters (docs/architecture/project-format.md, "Parameters")
// ------------------------------------------------------------------------------------------

[[nodiscard]] bool decodeConstantValue(const JsonValue& node, DecodeState& state,
                                       const std::string& path, ParameterValue& out) {
    std::string_view kindText;
    if (!decodeKindDiscriminator(node, state, path, kindText)) {
        return false;
    }

    if (kindText == "bool") {
        static constexpr std::array<std::string_view, 2> keys{"kind", "value"};
        std::vector<const JsonValue*> members;
        if (!matchOrderedMembers(node, keys, true, state, path, members)) {
            return false;
        }
        bool value = false;
        if (!decodeBooleanMember(*members[1], state, joinPath(path, "value"), value)) {
            return false;
        }
        out = value;
        return true;
    }
    if (kindText == "int64") {
        static constexpr std::array<std::string_view, 2> keys{"kind", "value"};
        std::vector<const JsonValue*> members;
        if (!matchOrderedMembers(node, keys, true, state, path, members)) {
            return false;
        }
        const auto valuePath = joinPath(path, "value");
        std::string_view text;
        if (!decodeStringMember(*members[1], state, valuePath, text)) {
            return false;
        }
        const auto parsed = parseCanonicalInt64(text);
        if (!parsed) {
            state.fail(DocumentDecodeError::InvalidInt64Value, valuePath);
            return false;
        }
        out = *parsed.value();
        return true;
    }
    if (kindText == "float64") {
        static constexpr std::array<std::string_view, 2> keys{"kind", "value"};
        std::vector<const JsonValue*> members;
        if (!matchOrderedMembers(node, keys, true, state, path, members)) {
            return false;
        }
        double value = 0.0;
        if (!decodeFloat64Member(*members[1], state, joinPath(path, "value"), value)) {
            return false;
        }
        out = value;
        return true;
    }
    if (kindText == "vec2") {
        static constexpr std::array<std::string_view, 3> keys{"kind", "x", "y"};
        std::vector<const JsonValue*> members;
        if (!matchOrderedMembers(node, keys, true, state, path, members)) {
            return false;
        }
        Vec2d value;
        if (!decodeFloat64Member(*members[1], state, joinPath(path, "x"), value.x)) {
            return false;
        }
        if (!decodeFloat64Member(*members[2], state, joinPath(path, "y"), value.y)) {
            return false;
        }
        out = value;
        return true;
    }
    if (kindText == "vec3" && state.documentMinor >= 4) {
        // Gated on the minor exactly as the Color4 curve kind is: a 1.3 or older document cannot
        // contain this kind, so accepting it there would accept a file no 1.3 writer could have
        // produced.
        static constexpr std::array<std::string_view, 4> keys{"kind", "x", "y", "z"};
        std::vector<const JsonValue*> members;
        if (!matchOrderedMembers(node, keys, true, state, path, members)) {
            return false;
        }
        Vec3d value;
        if (!decodeFloat64Member(*members[1], state, joinPath(path, "x"), value.x)) {
            return false;
        }
        if (!decodeFloat64Member(*members[2], state, joinPath(path, "y"), value.y)) {
            return false;
        }
        if (!decodeFloat64Member(*members[3], state, joinPath(path, "z"), value.z)) {
            return false;
        }
        out = value;
        return true;
    }
    if (kindText == "path" && state.documentMinor < 14) {
        state.fail(DocumentDecodeError::InvalidConstantValueKind, joinPath(path, "kind"));
        return false;
    }
    if (kindText == "path") {
        static constexpr std::array<std::string_view, 3> keys{"kind", "anchors", "closed"};
        std::vector<const JsonValue*> members;
        if (!matchOrderedMembers(node, keys, true, state, path, members))
            return false;
        document::PathValue value;
        if (members[1]->kind() != JsonValueKind::Array ||
            members[1]->arrayElements().size() > document::kMaximumPathAnchors) {
            state.fail(DocumentDecodeError::InvalidConstantValueKind, joinPath(path, "anchors"));
            return false;
        }
        const auto point = [&](const JsonValue& object, const std::string& where, Vec2d& outPoint) {
            static constexpr std::array<std::string_view, 2> xy{"x", "y"};
            std::vector<const JsonValue*> coordinates;
            if (object.kind() == JsonValueKind::Object &&
                object.objectMembers().size() > xy.size()) {
                state.fail(DocumentDecodeError::UnknownMember,
                           joinPath(where, object.objectMembers()[xy.size()].key()));
                return false;
            }
            return matchOrderedMembers(object, xy, false, state, where, coordinates) &&
                   decodeFloat64Member(*coordinates[0], state, joinPath(where, "x"), outPoint.x) &&
                   decodeFloat64Member(*coordinates[1], state, joinPath(where, "y"), outPoint.y);
        };
        for (const auto& anchor : members[1]->arrayElements()) {
            std::vector<std::string_view> anchorKeys{"point"};
            if (anchor.findMember("inHandle"))
                anchorKeys.push_back("inHandle");
            if (anchor.findMember("outHandle"))
                anchorKeys.push_back("outHandle");
            std::vector<const JsonValue*> parts;
            const auto where =
                joinPath(path, "anchors") + "/" + std::to_string(value.anchors.size());
            if (anchor.kind() == JsonValueKind::Object &&
                anchor.objectMembers().size() > anchorKeys.size()) {
                state.fail(DocumentDecodeError::UnknownMember,
                           joinPath(where, anchor.objectMembers()[anchorKeys.size()].key()));
                return false;
            }
            if (!matchOrderedMembers(anchor, anchorKeys, false, state, where, parts))
                return false;
            document::PathAnchor decoded;
            if (!point(*parts[0], joinPath(where, "point"), decoded.point))
                return false;
            for (std::size_t index = 1; index < parts.size(); ++index) {
                Vec2d handle;
                if (!point(*parts[index], joinPath(where, anchorKeys[index]), handle))
                    return false;
                (anchorKeys[index] == "inHandle" ? decoded.inHandle : decoded.outHandle) = handle;
            }
            value.anchors.push_back(decoded);
        }
        if (!decodeBooleanMember(*members[2], state, joinPath(path, "closed"), value.closed))
            return false;
        out = std::move(value);
        return true;
    }
    if (kindText == "color4") {
        static constexpr std::array<std::string_view, 5> keys{"kind", "red", "green", "blue",
                                                              "alpha"};
        std::vector<const JsonValue*> members;
        if (!matchOrderedMembers(node, keys, true, state, path, members)) {
            return false;
        }
        core::Color4d value;
        if (!decodeFloat64Member(*members[1], state, joinPath(path, "red"), value.red)) {
            return false;
        }
        if (!decodeFloat64Member(*members[2], state, joinPath(path, "green"), value.green)) {
            return false;
        }
        if (!decodeFloat64Member(*members[3], state, joinPath(path, "blue"), value.blue)) {
            return false;
        }
        if (!decodeFloat64Member(*members[4], state, joinPath(path, "alpha"), value.alpha)) {
            return false;
        }
        out = value;
        return true;
    }
    if (kindText == "string") {
        static constexpr std::array<std::string_view, 2> keys{"kind", "value"};
        std::vector<const JsonValue*> members;
        if (!matchOrderedMembers(node, keys, true, state, path, members)) {
            return false;
        }
        std::string_view text;
        if (!decodeStringMember(*members[1], state, joinPath(path, "value"), text)) {
            return false;
        }
        out = std::string(text);
        return true;
    }
    if (kindText == "rational") {
        static constexpr std::array<std::string_view, 3> keys{"kind", "numerator", "denominator"};
        std::vector<const JsonValue*> members;
        if (!matchOrderedMembers(node, keys, true, state, path, members)) {
            return false;
        }
        std::string_view numeratorText;
        if (!decodeStringMember(*members[1], state, joinPath(path, "numerator"), numeratorText)) {
            return false;
        }
        std::string_view denominatorText;
        if (!decodeStringMember(*members[2], state, joinPath(path, "denominator"),
                                denominatorText)) {
            return false;
        }
        const auto parsed = parseCanonicalRationalTime(numeratorText, denominatorText);
        if (!parsed) {
            state.fail(mapRationalError(parsed.error()), fieldPath(path, parsed.field()));
            return false;
        }
        out = *parsed.value();
        return true;
    }

    return failUnknownDiscriminator(state, joinPath(path, "kind"),
                                    DocumentDecodeError::InvalidConstantValueKind);
}

[[nodiscard]] bool decodeParameterSource(const JsonValue& node, DecodeState& state,
                                         const std::string& path, document::ParameterSource& out) {
    std::string_view kindText;
    if (!decodeKindDiscriminator(node, state, path, kindText)) {
        return false;
    }

    if (kindText == "constant") {
        static constexpr std::array<std::string_view, 2> keys{"kind", "value"};
        std::vector<const JsonValue*> members;
        if (!matchOrderedMembers(node, keys, true, state, path, members)) {
            return false;
        }
        ParameterValue value{};
        {
            const AttachmentScope valueScope(state, "value");
            if (!decodeConstantValue(*members[1], state, joinPath(path, "value"), value)) {
                return false;
            }
        }
        out = document::ConstantValueSource{std::move(value)};
        return true;
    }
    if (kindText == "animation-curve") {
        static constexpr std::array<std::string_view, 2> legacyKeys{"kind", "curveId"};
        static constexpr std::array<std::string_view, 3> componentKeys{"kind", "curveId",
                                                                       "defaultValue"};
        std::vector<const JsonValue*> members;
        const bool hasDefault =
            state.documentMinor >= 9 && node.findMember("defaultValue") != nullptr;
        if (hasDefault) {
            if (!matchOrderedMembers(node, componentKeys, true, state, path, members))
                return false;
        } else if (!matchOrderedMembers(node, legacyKeys, true, state, path, members)) {
            return false;
        }
        AnimationCurveId curveId;
        if (!decodeObjectId(*members[1], state, joinPath(path, "curveId"), curveId)) {
            return false;
        }
        std::optional<ParameterValue> defaultValue;
        if (hasDefault) {
            ParameterValue value;
            if (!decodeConstantValue(*members[2], state, joinPath(path, "defaultValue"), value))
                return false;
            defaultValue = std::move(value);
        }
        out = document::AnimationCurveSource{curveId, std::move(defaultValue)};
        return true;
    }
    if (kindText == "driver" && state.documentMinor >= 4) {
        // Document 1.4. Decoded on exactly an edge source's terms -- an object id and a port name,
        // with no more checking than decodeOutputPortRef() does. Whether the name is valid
        // structural text, whether the node EXISTS, and whether its kind fits are all the graph's
        // own validation to answer once every node is decoded, which is the same order an edge's
        // endpoints follow.
        static constexpr std::array<std::string_view, 3> keys{"kind", "sourceNodeId", "outputPort"};
        std::vector<const JsonValue*> members;
        if (!matchOrderedMembers(node, keys, true, state, path, members)) {
            return false;
        }
        NodeId sourceNodeId;
        if (!decodeObjectId(*members[1], state, joinPath(path, "sourceNodeId"), sourceNodeId)) {
            return false;
        }
        std::string_view port;
        if (!decodeStringMember(*members[2], state, joinPath(path, "outputPort"), port)) {
            return false;
        }
        out = document::DriverBindingSource{sourceNodeId, std::string(port)};
        return true;
    }

    return failUnknownDiscriminator(state, joinPath(path, "kind"),
                                    DocumentDecodeError::UnsupportedParameterSource);
}

// A parameter record is a collection element (identity: numeric ParameterId); see
// document_decode.cpp's decodeComposition for why a collection element's own trailing capture
// must be deferred until its identity member is decoded.
[[nodiscard]] bool decodeParameter(const JsonValue& node, DecodeState& state,
                                   const std::string& path, ParameterRecord& out) {
    static constexpr std::array<std::string_view, 3> keys{"id", "schemaKey", "source"};
    std::vector<const JsonValue*> members;
    std::vector<RetainedJsonMember> trailing;
    if (!matchOrderedMembers(node, keys, true, state, path, members, trailing)) {
        return false;
    }

    ParameterId id;
    if (!decodeObjectId(*members[0], state, joinPath(path, "id"), id)) {
        return false;
    }

    const AttachmentScope parameterScope(state, RoundTripCollectionKind::Parameter,
                                         std::to_string(id.value()));
    if (!trailing.empty() && state.roundTrip != nullptr) {
        state.roundTrip->attach(state.attachmentPath, std::move(trailing));
    }

    std::string_view schemaKeyText;
    if (!decodeStringMember(*members[1], state, joinPath(path, "schemaKey"), schemaKeyText)) {
        return false;
    }
    document::ParameterSource source;
    {
        const AttachmentScope sourceScope(state, "source");
        if (!decodeParameterSource(*members[2], state, joinPath(path, "source"), source)) {
            return false;
        }
    }

    out.id = id;
    out.schemaKey = std::string(schemaKeyText);
    out.source = std::move(source);
    return true;
}

[[nodiscard]] bool decodeParameters(const JsonValue& node, DecodeState& state,
                                    const std::string& path, std::vector<ParameterRecord>& out) {
    if (node.kind() != JsonValueKind::Array) {
        state.fail(DocumentDecodeError::WrongValueKind, path);
        return false;
    }
    const auto elements = node.arrayElements();
    out.clear();
    out.reserve(elements.size());
    std::uint64_t previousId = 0;
    bool hasPrevious = false;
    for (std::size_t index = 0; index < elements.size(); ++index) {
        ParameterRecord record;
        const auto elementPath = joinPathIndex(path, index);
        if (!decodeParameter(elements[index], state, elementPath, record)) {
            return false;
        }
        const auto currentId = record.id.value();
        if (hasPrevious) {
            if (currentId == previousId) {
                state.fail(DocumentDecodeError::DuplicateParameter, joinPath(elementPath, "id"));
                return false;
            }
            if (currentId < previousId) {
                state.fail(DocumentDecodeError::UnsortedParameters, joinPath(elementPath, "id"));
                return false;
            }
        }
        previousId = currentId;
        hasPrevious = true;
        out.push_back(std::move(record));
    }
    return true;
}

// ------------------------------------------------------------------------------------------
// Animation curves (docs/architecture/project-format.md, "Animation")
// ------------------------------------------------------------------------------------------

[[nodiscard]] bool decodeInterpolation(const JsonValue& value, DecodeState& state,
                                       const std::string& path, KeyframeInterpolation& out) {
    std::string_view text;
    if (!decodeStringMember(value, state, path, text)) {
        return false;
    }
    if (text == "hold") {
        out = KeyframeInterpolation::Hold;
        return true;
    }
    if (text == "linear") {
        out = KeyframeInterpolation::Linear;
        return true;
    }
    // Gated on the minor that DECLARES it, exactly as the composition's own member count is
    // (document_decode.cpp's decodeComposition): a file claiming 1.2 carrying a 1.3 token is a
    // malformed 1.2 file, not data to accept silently. A real older file never reaches here with
    // this token -- the migration ladder rewrites the version before trusted decode -- and an
    // OLDER build reading a 1.3 file preserves the unknown token read-only through
    // failUnknownDiscriminator()'s own route.
    if (text == "ease-in-out" && state.documentMinor >= 3) {
        out = KeyframeInterpolation::EaseInOut;
        return true;
    }
    state.fail(DocumentDecodeError::InvalidInterpolation, path);
    return false;
}

// The minor that DECLARES the ease-handle members. A file claiming an older minor that carries one
// is a malformed file of that minor, not data to accept silently -- the same rule the
// "ease-in-out" interpolation token follows.
inline constexpr std::uint32_t kKeyframeHandleMinor = 12;

// A handle object is CLOSED: exactly {time, value}, in that order, and nothing else. It is not a
// collection element and carries no identity, so it is not a round-trip attachment point either;
// an unknown member inside one is an error rather than something to preserve.
[[nodiscard]] bool decodeKeyframeHandle(const JsonValue& node, DecodeState& state,
                                        const std::string& path, document::KeyframeHandle& out) {
    static constexpr std::array<std::string_view, 2> keys{"time", "value"};
    if (node.kind() != JsonValueKind::Object) {
        state.fail(DocumentDecodeError::WrongValueKind, path);
        return false;
    }
    const auto members = node.objectMembers();
    if (members.size() > keys.size()) {
        state.fail(DocumentDecodeError::UnknownMember, joinPath(path, members[keys.size()].key()));
        return false;
    }
    std::vector<const JsonValue*> values;
    if (!matchOrderedMembers(node, keys, false, state, path, values)) {
        return false;
    }
    document::KeyframeHandle handle;
    if (!decodeFloat64Member(*values[0], state, joinPath(path, "time"), handle.time) ||
        !decodeFloat64Member(*values[1], state, joinPath(path, "value"), handle.value)) {
        return false;
    }
    if (!document::isValidKeyframeHandle(handle)) {
        state.fail(DocumentDecodeError::InvalidKeyframeHandle, path);
        return false;
    }
    out = handle;
    return true;
}

// The two handle members are OPTIONAL and omitted whenever they are default, so the exact key list
// a keyframe object is matched against depends on which of them it actually carries. Peeking at the
// members in declared order keeps matchOrderedMembers' strict ordering, unknown-member and
// round-trip-capture rules exactly as they are for every other object: an out-of-order or
// unexpected member still fails, and a genuinely unknown trailing member is still captured.
void appendPresentHandleKeys(const JsonValue& node, const DecodeState& state,
                             const std::size_t knownCount, std::vector<std::string_view>& keys) {
    if (state.documentMinor < kKeyframeHandleMinor || node.kind() != JsonValueKind::Object) {
        return;
    }
    const auto members = node.objectMembers();
    auto index = knownCount;
    if (members.size() > index && members[index].key() == "outgoingHandle") {
        keys.emplace_back("outgoingHandle");
        ++index;
    }
    if (members.size() > index && members[index].key() == "incomingHandle") {
        keys.emplace_back("incomingHandle");
    }
}

[[nodiscard]] bool decodeHandleMembers(const std::vector<const JsonValue*>& members,
                                       const std::vector<std::string_view>& keys,
                                       const std::size_t knownCount, DecodeState& state,
                                       const std::string& path, ScalarKeyframe& out) {
    for (auto index = knownCount; index < keys.size(); ++index) {
        auto& handle = keys[index] == "outgoingHandle" ? out.outgoingHandle : out.incomingHandle;
        if (!decodeKeyframeHandle(*members[index], state, joinPath(path, keys[index]), handle)) {
            return false;
        }
    }
    return true;
}

// A keyframe is a collection element (identity: numeric KeyframeId); see decodeComposition's own
// comment in document_decode.cpp for why a collection element's own trailing capture must be
// deferred until its identity member is decoded.
[[nodiscard]] bool decodeScalarKeyframe(const JsonValue& node, DecodeState& state,
                                        const std::string& path, ScalarKeyframe& out) {
    std::vector<std::string_view> keys{"id", "time", "value", "outgoingInterpolation"};
    const auto knownCount = keys.size();
    appendPresentHandleKeys(node, state, knownCount, keys);
    std::vector<const JsonValue*> members;
    std::vector<RetainedJsonMember> trailing;
    if (!matchOrderedMembers(node, keys, true, state, path, members, trailing)) {
        return false;
    }

    document::KeyframeId id;
    if (!decodeObjectId(*members[0], state, joinPath(path, "id"), id)) {
        return false;
    }

    const AttachmentScope keyframeScope(state, RoundTripCollectionKind::Keyframe,
                                        std::to_string(id.value()));
    if (!trailing.empty() && state.roundTrip != nullptr) {
        state.roundTrip->attach(state.attachmentPath, std::move(trailing));
    }

    core::RationalTime time;
    {
        const AttachmentScope timeScope(state, "time");
        if (!decodeRationalTimeValue(*members[1], state, joinPath(path, "time"), time)) {
            return false;
        }
    }
    double value = 0.0;
    if (!decodeFloat64Member(*members[2], state, joinPath(path, "value"), value)) {
        return false;
    }
    KeyframeInterpolation interpolation = KeyframeInterpolation::Linear;
    if (!decodeInterpolation(*members[3], state, joinPath(path, "outgoingInterpolation"),
                             interpolation)) {
        return false;
    }

    out.id = id;
    out.time = time;
    out.value = value;
    out.outgoingInterpolation = interpolation;
    return decodeHandleMembers(members, keys, knownCount, state, path, out);
}

[[nodiscard]] std::optional<AnimationComponent>
componentFromText(const std::string_view text) noexcept {
    if (text == "x")
        return AnimationComponent::X;
    if (text == "y")
        return AnimationComponent::Y;
    if (text == "z")
        return AnimationComponent::Z;
    if (text == "red")
        return AnimationComponent::Red;
    if (text == "green")
        return AnimationComponent::Green;
    if (text == "blue")
        return AnimationComponent::Blue;
    if (text == "alpha")
        return AnimationComponent::Alpha;
    return std::nullopt;
}

template <std::size_t Count>
[[nodiscard]] bool decodeComponentKeyframe(const JsonValue& node, DecodeState& state,
                                           const std::string& path,
                                           const std::array<AnimationComponent, Count>& allowed,
                                           AnimationComponent& component, ScalarKeyframe& out) {
    std::vector<std::string_view> keys{"id", "component", "time", "value", "outgoingInterpolation"};
    const auto knownCount = keys.size();
    appendPresentHandleKeys(node, state, knownCount, keys);
    std::vector<const JsonValue*> members;
    std::vector<RetainedJsonMember> trailing;
    if (!matchOrderedMembers(node, keys, true, state, path, members, trailing))
        return false;

    document::KeyframeId id;
    if (!decodeObjectId(*members[0], state, joinPath(path, "id"), id))
        return false;
    std::string_view componentText;
    if (!decodeStringMember(*members[1], state, joinPath(path, "component"), componentText))
        return false;
    const auto parsedComponent = componentFromText(componentText);
    if (!parsedComponent.has_value() ||
        std::find(allowed.begin(), allowed.end(), *parsedComponent) == allowed.end()) {
        state.fail(DocumentDecodeError::UnsupportedParameterSource, joinPath(path, "component"));
        return false;
    }
    core::RationalTime time;
    if (!decodeRationalTimeValue(*members[2], state, joinPath(path, "time"), time))
        return false;
    double value = 0.0;
    if (!decodeFloat64Member(*members[3], state, joinPath(path, "value"), value))
        return false;
    KeyframeInterpolation interpolation = KeyframeInterpolation::Linear;
    if (!decodeInterpolation(*members[4], state, joinPath(path, "outgoingInterpolation"),
                             interpolation))
        return false;

    const AttachmentScope keyframeScope(state, RoundTripCollectionKind::Keyframe,
                                        std::to_string(id.value()));
    if (!trailing.empty() && state.roundTrip != nullptr)
        state.roundTrip->attach(state.attachmentPath, std::move(trailing));
    component = *parsedComponent;
    out = ScalarKeyframe{id, time, value, interpolation};
    return decodeHandleMembers(members, keys, knownCount, state, path, out);
}

template <std::size_t Count>
[[nodiscard]] bool
decodeComponentKeyframeArray(const JsonValue& node, DecodeState& state, const std::string& path,
                             const std::array<AnimationComponent, Count>& allowed,
                             std::array<ComponentAnimationCurve, Count>& out) {
    if (node.kind() != JsonValueKind::Array) {
        state.fail(DocumentDecodeError::WrongValueKind, path);
        return false;
    }
    const auto elements = node.arrayElements();
    if (elements.empty()) {
        state.fail(DocumentDecodeError::EmptyKeyframes, path);
        return false;
    }
    for (auto& component : out)
        component.keyframes.clear();
    for (std::size_t index = 0; index < elements.size(); ++index) {
        AnimationComponent component;
        ScalarKeyframe keyframe;
        if (!decodeComponentKeyframe(elements[index], state, joinPathIndex(path, index), allowed,
                                     component, keyframe))
            return false;
        const auto componentIndex = static_cast<std::size_t>(
            std::find(allowed.begin(), allowed.end(), component) - allowed.begin());
        auto& keys = out[componentIndex].keyframes;
        if (!keys.empty() && !(keys.back().time < keyframe.time)) {
            state.fail(DocumentDecodeError::NonIncreasingKeyframeTime,
                       joinPathIndex(path, index) + ".time");
            return false;
        }
        keys.push_back(std::move(keyframe));
    }
    for (const auto& component : out) {
        if (!component.keyframes.empty() &&
            component.keyframes.back().outgoingInterpolation != KeyframeInterpolation::Linear) {
            state.fail(DocumentDecodeError::FinalKeyframeNotLinear, path);
            return false;
        }
    }
    return true;
}

// Shared keyframe-array shape: non-empty, strictly increasing exact rational time, and a
// canonical Linear final interpolation (docs/architecture/project-format.md, "Animation").
template <typename Keyframe, typename DecodeOne>
[[nodiscard]] bool decodeKeyframeArray(const JsonValue& node, DecodeState& state,
                                       const std::string& path, std::vector<Keyframe>& out,
                                       DecodeOne decodeOne) {
    if (node.kind() != JsonValueKind::Array) {
        state.fail(DocumentDecodeError::WrongValueKind, path);
        return false;
    }
    const auto elements = node.arrayElements();
    if (elements.empty()) {
        state.fail(DocumentDecodeError::EmptyKeyframes, path);
        return false;
    }
    out.clear();
    out.reserve(elements.size());
    for (std::size_t index = 0; index < elements.size(); ++index) {
        Keyframe keyframe;
        const auto elementPath = joinPathIndex(path, index);
        if (!decodeOne(elements[index], state, elementPath, keyframe)) {
            return false;
        }
        if (!out.empty() && !(out.back().time < keyframe.time)) {
            state.fail(DocumentDecodeError::NonIncreasingKeyframeTime,
                       joinPath(elementPath, "time"));
            return false;
        }
        out.push_back(std::move(keyframe));
    }
    if (out.back().outgoingInterpolation != KeyframeInterpolation::Linear) {
        const auto finalElementPath = joinPathIndex(path, elements.size() - 1);
        state.fail(DocumentDecodeError::FinalKeyframeNotLinear,
                   joinPath(finalElementPath, "outgoingInterpolation"));
        return false;
    }
    return true;
}

[[nodiscard]] bool decodeAnimationCurve(const JsonValue& node, DecodeState& state,
                                        const std::string& path, AnimationCurveRecord& out) {
    static constexpr std::array<std::string_view, 2> prefixKeys{"id", "kind"};
    std::vector<const JsonValue*> prefix;
    if (!matchOrderedMembers(node, prefixKeys, false, state, path, prefix)) {
        return false;
    }
    document::AnimationCurveId id;
    if (!decodeObjectId(*prefix[0], state, joinPath(path, "id"), id)) {
        return false;
    }
    std::string_view kindText;
    if (!decodeStringMember(*prefix[1], state, joinPath(path, "kind"), kindText)) {
        return false;
    }

    // Unlike composition/parameter/node/edge/extensionRecord/keyframe above, an animation curve's
    // identity (numeric AnimationCurveId) is already decoded from the id/kind prefix peek above,
    // so its own AttachmentScope can be pushed before the full closed-shape match below and this
    // collection element can use the ordinary six-argument (auto-attaching) matchOrderedMembers.
    const AttachmentScope animationCurveScope(state, RoundTripCollectionKind::AnimationCurve,
                                              std::to_string(id.value()));

    if (kindText == "scalar") {
        static constexpr std::array<std::string_view, 3> keys{"id", "kind", "keyframes"};
        std::vector<const JsonValue*> members;
        if (!matchOrderedMembers(node, keys, true, state, path, members)) {
            return false;
        }
        ScalarAnimationCurve curve;
        curve.id = id;
        if (!decodeKeyframeArray(*members[2], state, joinPath(path, "keyframes"), curve.keyframes,
                                 decodeScalarKeyframe)) {
            return false;
        }
        out = std::move(curve);
        return true;
    }
    if (kindText == "vec2") {
        static constexpr std::array<std::string_view, 3> keys{"id", "kind", "keyframes"};
        std::vector<const JsonValue*> members;
        if (!matchOrderedMembers(node, keys, true, state, path, members)) {
            return false;
        }
        Vec2AnimationCurve curve;
        curve.id = id;
        if (!decodeComponentKeyframeArray(*members[2], state, joinPath(path, "keyframes"),
                                          std::array{AnimationComponent::X, AnimationComponent::Y},
                                          curve.components)) {
            return false;
        }
        out = std::move(curve);
        return true;
    }
    if (kindText == "vec3") {
        static constexpr std::array<std::string_view, 3> keys{"id", "kind", "keyframes"};
        std::vector<const JsonValue*> members;
        if (!matchOrderedMembers(node, keys, true, state, path, members))
            return false;
        Vec3AnimationCurve curve;
        curve.id = id;
        if (!decodeComponentKeyframeArray(
                *members[2], state, joinPath(path, "keyframes"),
                std::array{AnimationComponent::X, AnimationComponent::Y, AnimationComponent::Z},
                curve.components))
            return false;
        out = std::move(curve);
        return true;
    }
    if (kindText == "color4") {
        static constexpr std::array<std::string_view, 3> keys{"id", "kind", "keyframes"};
        std::vector<const JsonValue*> members;
        if (!matchOrderedMembers(node, keys, true, state, path, members)) {
            return false;
        }
        Color4AnimationCurve curve;
        curve.id = id;
        if (!decodeComponentKeyframeArray(
                *members[2], state, joinPath(path, "keyframes"),
                std::array{AnimationComponent::Red, AnimationComponent::Green,
                           AnimationComponent::Blue, AnimationComponent::Alpha},
                curve.components)) {
            return false;
        }
        out = std::move(curve);
        return true;
    }

    return failUnknownDiscriminator(state, joinPath(path, "kind"),
                                    DocumentDecodeError::InvalidAnimationCurveKind);
}

[[nodiscard]] bool decodeAnimationCurves(const JsonValue& node, DecodeState& state,
                                         const std::string& path,
                                         std::vector<AnimationCurveRecord>& out) {
    if (node.kind() != JsonValueKind::Array) {
        state.fail(DocumentDecodeError::WrongValueKind, path);
        return false;
    }
    const auto elements = node.arrayElements();
    out.clear();
    out.reserve(elements.size());
    std::uint64_t previousId = 0;
    bool hasPrevious = false;
    for (std::size_t index = 0; index < elements.size(); ++index) {
        AnimationCurveRecord record;
        const auto elementPath = joinPathIndex(path, index);
        if (!decodeAnimationCurve(elements[index], state, elementPath, record)) {
            return false;
        }
        const auto currentId = document::animationCurveId(record).value();
        if (hasPrevious) {
            if (currentId == previousId) {
                state.fail(DocumentDecodeError::DuplicateAnimationCurve,
                           joinPath(elementPath, "id"));
                return false;
            }
            if (currentId < previousId) {
                state.fail(DocumentDecodeError::UnsortedAnimationCurves,
                           joinPath(elementPath, "id"));
                return false;
            }
        }
        previousId = currentId;
        hasPrevious = true;
        out.push_back(std::move(record));
    }
    return true;
}

// A parameter's AnimationCurveSource.curveId can name a curve declared anywhere in the
// composition's animationCurves array, including after the referencing parameter -- both arrays
// must be fully decoded before this check runs.
[[nodiscard]] bool checkParameterCurveReferences(DecodeState& state,
                                                 const std::string& parametersPath,
                                                 const std::vector<ParameterRecord>& parameters,
                                                 const std::vector<AnimationCurveRecord>& curves) {
    for (std::size_t index = 0; index < parameters.size(); ++index) {
        const auto* source = std::get_if<document::AnimationCurveSource>(&parameters[index].source);
        if (source == nullptr) {
            continue;
        }
        const bool found = std::ranges::any_of(curves, [&](const AnimationCurveRecord& record) {
            return document::animationCurveId(record) == source->curveId;
        });
        if (!found) {
            const auto elementPath = joinPathIndex(parametersPath, index);
            state.fail(DocumentDecodeError::DanglingReference,
                       joinPath(joinPath(elementPath, "source"), "curveId"));
            return false;
        }
    }
    return true;
}

// ------------------------------------------------------------------------------------------
// Canonical graph (docs/architecture/project-format.md, "Canonical Graph")
// ------------------------------------------------------------------------------------------

[[nodiscard]] bool nodeExists(const std::vector<NodeRecord>& nodes, const NodeId id) noexcept {
    return std::ranges::any_of(nodes, [id](const NodeRecord& node) { return node.id == id; });
}

[[nodiscard]] bool parameterExists(const std::vector<ParameterRecord>& parameters,
                                   const ParameterId id) noexcept {
    return std::ranges::any_of(parameters,
                               [id](const ParameterRecord& record) { return record.id == id; });
}

[[nodiscard]] bool decodeOutputPortRef(const JsonValue& node, DecodeState& state,
                                       const std::string& path, OutputPortRef& out) {
    static constexpr std::array<std::string_view, 2> keys{"nodeId", "port"};
    std::vector<const JsonValue*> members;
    if (!matchOrderedMembers(node, keys, true, state, path, members)) {
        return false;
    }
    NodeId nodeId;
    if (!decodeObjectId(*members[0], state, joinPath(path, "nodeId"), nodeId)) {
        return false;
    }
    std::string_view portText;
    if (!decodeStringMember(*members[1], state, joinPath(path, "port"), portText)) {
        return false;
    }
    out.nodeId = nodeId;
    out.port = std::string(portText);
    return true;
}

[[nodiscard]] bool decodeEdgeDestination(const JsonValue& node, DecodeState& state,
                                         const std::string& path, InputPortRef& out) {
    std::string_view kindText;
    if (!decodeKindDiscriminator(node, state, path, kindText)) {
        return false;
    }

    if (kindText == "node-input") {
        static constexpr std::array<std::string_view, 3> keys{"kind", "nodeId", "port"};
        std::vector<const JsonValue*> members;
        if (!matchOrderedMembers(node, keys, true, state, path, members)) {
            return false;
        }
        NodeId nodeId;
        if (!decodeObjectId(*members[1], state, joinPath(path, "nodeId"), nodeId)) {
            return false;
        }
        std::string_view portText;
        if (!decodeStringMember(*members[2], state, joinPath(path, "port"), portText)) {
            return false;
        }
        out = NodeInputRef{nodeId, std::string(portText)};
        return true;
    }
    if (kindText == "layer-stack-input") {
        static constexpr std::array<std::string_view, 4> keys{"kind", "stackNodeId", "slotId",
                                                              "role"};
        std::vector<const JsonValue*> members;
        if (!matchOrderedMembers(node, keys, true, state, path, members)) {
            return false;
        }
        NodeId stackNodeId;
        if (!decodeObjectId(*members[1], state, joinPath(path, "stackNodeId"), stackNodeId)) {
            return false;
        }
        LayerSlotId slotId;
        if (!decodeObjectId(*members[2], state, joinPath(path, "slotId"), slotId)) {
            return false;
        }
        std::string_view roleText;
        if (!decodeStringMember(*members[3], state, joinPath(path, "role"), roleText)) {
            return false;
        }
        out = LayerStackInputRef{stackNodeId, slotId, std::string(roleText)};
        return true;
    }

    return failUnknownDiscriminator(state, joinPath(path, "kind"),
                                    DocumentDecodeError::InvalidEdgeDestinationKind);
}

// An edge is a collection element (identity: numeric EdgeId); see decodeComposition's own comment
// in document_decode.cpp for why a collection element's own trailing capture must be deferred
// until its identity member is decoded.
[[nodiscard]] bool decodeEdge(const JsonValue& node, DecodeState& state, const std::string& path,
                              EdgeRecord& out) {
    static constexpr std::array<std::string_view, 3> keys{"id", "source", "destination"};
    std::vector<const JsonValue*> members;
    std::vector<RetainedJsonMember> trailing;
    if (!matchOrderedMembers(node, keys, true, state, path, members, trailing)) {
        return false;
    }

    EdgeId id;
    if (!decodeObjectId(*members[0], state, joinPath(path, "id"), id)) {
        return false;
    }

    const AttachmentScope edgeScope(state, RoundTripCollectionKind::Edge,
                                    std::to_string(id.value()));
    if (!trailing.empty() && state.roundTrip != nullptr) {
        state.roundTrip->attach(state.attachmentPath, std::move(trailing));
    }

    OutputPortRef source;
    {
        const AttachmentScope sourceScope(state, "source");
        if (!decodeOutputPortRef(*members[1], state, joinPath(path, "source"), source)) {
            return false;
        }
    }
    InputPortRef destination;
    {
        const AttachmentScope destinationScope(state, "destination");
        if (!decodeEdgeDestination(*members[2], state, joinPath(path, "destination"),
                                   destination)) {
            return false;
        }
    }

    out.id = id;
    out.source = std::move(source);
    out.destination = std::move(destination);
    return true;
}

[[nodiscard]] bool decodeEdges(const JsonValue& node, DecodeState& state, const std::string& path,
                               std::vector<EdgeRecord>& out) {
    if (node.kind() != JsonValueKind::Array) {
        state.fail(DocumentDecodeError::WrongValueKind, path);
        return false;
    }
    const auto elements = node.arrayElements();
    out.clear();
    out.reserve(elements.size());
    std::uint64_t previousId = 0;
    bool hasPrevious = false;
    for (std::size_t index = 0; index < elements.size(); ++index) {
        EdgeRecord record;
        const auto elementPath = joinPathIndex(path, index);
        if (!decodeEdge(elements[index], state, elementPath, record)) {
            return false;
        }
        const auto currentId = record.id.value();
        if (hasPrevious) {
            if (currentId == previousId) {
                state.fail(DocumentDecodeError::DuplicateEdge, joinPath(elementPath, "id"));
                return false;
            }
            if (currentId < previousId) {
                state.fail(DocumentDecodeError::UnsortedEdges, joinPath(elementPath, "id"));
                return false;
            }
        }
        previousId = currentId;
        hasPrevious = true;
        out.push_back(std::move(record));
    }
    return true;
}

[[nodiscard]] bool checkEdgeNodeReferences(DecodeState& state, const std::string& edgesPath,
                                           const std::vector<EdgeRecord>& edges,
                                           const std::vector<NodeRecord>& nodes) {
    for (std::size_t index = 0; index < edges.size(); ++index) {
        const auto& edge = edges[index];
        const auto edgePath = joinPathIndex(edgesPath, index);
        if (!nodeExists(nodes, edge.source.nodeId)) {
            state.fail(DocumentDecodeError::DanglingReference,
                       joinPath(joinPath(edgePath, "source"), "nodeId"));
            return false;
        }
        if (const auto* nodeInput = std::get_if<NodeInputRef>(&edge.destination)) {
            if (!nodeExists(nodes, nodeInput->nodeId)) {
                state.fail(DocumentDecodeError::DanglingReference,
                           joinPath(joinPath(edgePath, "destination"), "nodeId"));
                return false;
            }
        } else {
            const auto& stackInput = std::get<LayerStackInputRef>(edge.destination);
            if (!nodeExists(nodes, stackInput.stackNodeId)) {
                state.fail(DocumentDecodeError::DanglingReference,
                           joinPath(joinPath(edgePath, "destination"), "stackNodeId"));
                return false;
            }
        }
    }
    return true;
}

// A node's parameter binding is a collection element identified by its `role`, scoped to its
// owning node (see docs/architecture/project-format.md, "Versions, Migrations, And Preservation":
// "binding role within its node"). Its identity is one of its own known members, decoded only
// after this closed shape's own match returns, so its own trailing capture must be deferred like
// every other collection element's own shape.
[[nodiscard]] bool decodeParameterBinding(const JsonValue& node, DecodeState& state,
                                          const std::string& path, ParameterBinding& out) {
    static constexpr std::array<std::string_view, 2> keys{"role", "parameterId"};
    std::vector<const JsonValue*> members;
    std::vector<RetainedJsonMember> trailing;
    if (!matchOrderedMembers(node, keys, true, state, path, members, trailing)) {
        return false;
    }
    std::string_view roleText;
    if (!decodeStringMember(*members[0], state, joinPath(path, "role"), roleText)) {
        return false;
    }

    const AttachmentScope bindingScope(state, RoundTripCollectionKind::ParameterBinding,
                                       std::string(roleText));
    if (!trailing.empty() && state.roundTrip != nullptr) {
        state.roundTrip->attach(state.attachmentPath, std::move(trailing));
    }

    ParameterId parameterId;
    if (!decodeObjectId(*members[1], state, joinPath(path, "parameterId"), parameterId)) {
        return false;
    }
    out.role = std::string(roleText);
    out.parameterId = parameterId;
    return true;
}

[[nodiscard]] bool decodeBindings(const JsonValue& node, DecodeState& state,
                                  const std::string& path, std::vector<ParameterBinding>& out) {
    if (node.kind() != JsonValueKind::Array) {
        state.fail(DocumentDecodeError::WrongValueKind, path);
        return false;
    }
    const auto elements = node.arrayElements();
    out.clear();
    out.reserve(elements.size());
    bool hasPrevious = false;
    std::string previousRole;
    for (std::size_t index = 0; index < elements.size(); ++index) {
        ParameterBinding binding;
        const auto elementPath = joinPathIndex(path, index);
        if (!decodeParameterBinding(elements[index], state, elementPath, binding)) {
            return false;
        }
        if (hasPrevious) {
            if (binding.role == previousRole) {
                state.fail(DocumentDecodeError::DuplicateBinding, joinPath(elementPath, "role"));
                return false;
            }
            if (!(previousRole < binding.role)) {
                state.fail(DocumentDecodeError::UnsortedBindings, joinPath(elementPath, "role"));
                return false;
            }
        }
        previousRole = binding.role;
        hasPrevious = true;
        out.push_back(std::move(binding));
    }
    return true;
}

// A node is a collection element (identity: numeric NodeId); see decodeComposition's own comment
// in document_decode.cpp for why a collection element's own trailing capture must be deferred
// until its identity member is decoded.
[[nodiscard]] bool decodeNode(const JsonValue& node, DecodeState& state, const std::string& path,
                              NodeRecord& out) {
    static constexpr std::array<std::string_view, 4> keys{"id", "typeId", "schemaVersion",
                                                          "parameters"};
    std::vector<const JsonValue*> members;
    std::vector<RetainedJsonMember> trailing;
    if (!matchOrderedMembers(node, keys, true, state, path, members, trailing)) {
        return false;
    }

    NodeId id;
    if (!decodeObjectId(*members[0], state, joinPath(path, "id"), id)) {
        return false;
    }

    const AttachmentScope nodeScope(state, RoundTripCollectionKind::Node,
                                    std::to_string(id.value()));
    if (!trailing.empty() && state.roundTrip != nullptr) {
        state.roundTrip->attach(state.attachmentPath, std::move(trailing));
    }

    std::string_view typeIdText;
    if (!decodeStringMember(*members[1], state, joinPath(path, "typeId"), typeIdText)) {
        return false;
    }
    const auto schemaVersionPath = joinPath(path, "schemaVersion");
    std::uint32_t schemaVersion = 0;
    if (!decodeUInt32Member(*members[2], state, schemaVersionPath,
                            std::numeric_limits<std::uint32_t>::max(), schemaVersion)) {
        return false;
    }
    if (schemaVersion == 0) {
        state.fail(DocumentDecodeError::DomainViolation, schemaVersionPath);
        return false;
    }
    std::vector<ParameterBinding> bindings;
    if (!decodeBindings(*members[3], state, joinPath(path, "parameters"), bindings)) {
        return false;
    }

    out.id = id;
    out.typeId = std::string(typeIdText);
    out.schemaVersion = schemaVersion;
    out.parameters = std::move(bindings);
    return true;
}

[[nodiscard]] bool decodeNodes(const JsonValue& node, DecodeState& state, const std::string& path,
                               std::vector<NodeRecord>& out) {
    if (node.kind() != JsonValueKind::Array) {
        state.fail(DocumentDecodeError::WrongValueKind, path);
        return false;
    }
    const auto elements = node.arrayElements();
    out.clear();
    out.reserve(elements.size());
    std::uint64_t previousId = 0;
    bool hasPrevious = false;
    for (std::size_t index = 0; index < elements.size(); ++index) {
        NodeRecord record;
        const auto elementPath = joinPathIndex(path, index);
        if (!decodeNode(elements[index], state, elementPath, record)) {
            return false;
        }
        const auto currentId = record.id.value();
        if (hasPrevious) {
            if (currentId == previousId) {
                state.fail(DocumentDecodeError::DuplicateNode, joinPath(elementPath, "id"));
                return false;
            }
            if (currentId < previousId) {
                state.fail(DocumentDecodeError::UnsortedNodes, joinPath(elementPath, "id"));
                return false;
            }
        }
        previousId = currentId;
        hasPrevious = true;
        out.push_back(std::move(record));
    }
    return true;
}

[[nodiscard]] bool checkNodeParameterBindings(DecodeState& state, const std::string& nodesPath,
                                              const std::vector<NodeRecord>& nodes,
                                              const std::vector<ParameterRecord>& parameters) {
    for (std::size_t nodeIndex = 0; nodeIndex < nodes.size(); ++nodeIndex) {
        const auto& node = nodes[nodeIndex];
        const auto bindingsPath = joinPath(joinPathIndex(nodesPath, nodeIndex), "parameters");
        for (std::size_t bindingIndex = 0; bindingIndex < node.parameters.size(); ++bindingIndex) {
            if (!parameterExists(parameters, node.parameters[bindingIndex].parameterId)) {
                state.fail(DocumentDecodeError::DanglingReference,
                           joinPath(joinPathIndex(bindingsPath, bindingIndex), "parameterId"));
                return false;
            }
        }
    }
    return true;
}

// A Layer Output boundary is a collection element identified by `layerId` (see
// docs/architecture/project-format.md, "Versions, Migrations, And Preservation": "layer ID for a
// Layer Output"). `layerId` is its second member, so its own trailing capture must be deferred
// until this closed shape's own match returns, like every other collection element's own shape.
[[nodiscard]] bool decodeLayerOutputBoundary(const JsonValue& node, DecodeState& state,
                                             const std::string& path, LayerOutputBoundary& out) {
    std::vector<std::string_view> keys{"nodeId", "layerId", "name", "outputPort"};
    if (state.documentMinor >= 5) {
        for (const auto* key : {"inPoint", "outPoint", "enabled", "solo", "locked", "labelColor"})
            if (node.findMember(key))
                keys.push_back(key);
    }
    if (state.documentMinor >= 13 && node.findMember("parent"))
        keys.push_back("parent");
    std::vector<const JsonValue*> members;
    std::vector<RetainedJsonMember> trailing;
    if (!matchOrderedMembers(node, keys, true, state, path, members, trailing)) {
        return false;
    }
    NodeId nodeId;
    if (!decodeObjectId(*members[0], state, joinPath(path, "nodeId"), nodeId)) {
        return false;
    }
    LayerId layerId;
    if (!decodeObjectId(*members[1], state, joinPath(path, "layerId"), layerId)) {
        return false;
    }

    const AttachmentScope layerOutputScope(state, RoundTripCollectionKind::LayerOutput,
                                           std::to_string(layerId.value()));
    if (!trailing.empty() && state.roundTrip != nullptr) {
        state.roundTrip->attach(state.attachmentPath, std::move(trailing));
    }
    std::string_view nameText;
    if (!decodeStringMember(*members[2], state, joinPath(path, "name"), nameText)) {
        return false;
    }
    std::string_view outputPortText;
    if (!decodeStringMember(*members[3], state, joinPath(path, "outputPort"), outputPortText)) {
        return false;
    }
    if (const auto* value = node.findMember("parent"); value && state.documentMinor >= 13) {
        LayerId parent;
        if (!decodeObjectId(*value, state, joinPath(path, "parent"), parent))
            return false;
        out.parent = parent;
    }
    out.nodeId = nodeId;
    out.layerId = layerId;
    out.name = std::string(nameText);
    out.outputPort = std::string(outputPortText);
    if (const auto* color = node.findMember("labelColor"); color && state.documentMinor >= 5) {
        const auto values = color->arrayElements();
        if (color->kind() != JsonValueKind::Array || values.size() != 3) {
            state.fail(DocumentDecodeError::WrongValueKind, joinPath(path, "labelColor"));
            return false;
        }
        std::array<std::uint8_t, 3> rgb{};
        for (std::size_t i = 0; i < rgb.size(); ++i) {
            std::uint32_t channel = 0;
            if (!decodeUInt32Member(values[i], state,
                                    joinPathIndex(joinPath(path, "labelColor"), i), 255, channel))
                return false;
            if (channel > 255) {
                state.fail(DocumentDecodeError::DomainViolation, joinPath(path, "labelColor"));
                return false;
            }
            rgb[i] = static_cast<std::uint8_t>(channel);
        }
        out.labelColor = rgb;
    }
    for (const auto& [key, target] :
         {std::pair{"enabled", &out.enabled}, std::pair{"solo", &out.solo},
          std::pair{"locked", &out.locked}}) {
        if (const auto* value = node.findMember(key); value && state.documentMinor >= 5) {
            const auto flag = value->asBoolean();
            if (!flag) {
                state.fail(DocumentDecodeError::WrongValueKind, joinPath(path, key));
                return false;
            }
            *target = *flag;
        }
    }
    for (const auto* key : {"inPoint", "outPoint"}) {
        if (const auto* value = node.findMember(key); value && state.documentMinor >= 5) {
            const AttachmentScope rangeScope(state, key);
            auto& time = std::string_view(key) == "inPoint" ? out.inPoint : out.outPoint;
            if (!decodeRationalTimeValue(*value, state, joinPath(path, key), time))
                return false;
        }
    }
    return true;
}

[[nodiscard]] bool decodeLayerOutputs(const JsonValue& node, DecodeState& state,
                                      const std::string& path,
                                      std::vector<LayerOutputBoundary>& out) {
    if (node.kind() != JsonValueKind::Array) {
        state.fail(DocumentDecodeError::WrongValueKind, path);
        return false;
    }
    const auto elements = node.arrayElements();
    out.clear();
    out.reserve(elements.size());
    bool hasPrevious = false;
    std::pair<std::uint64_t, std::uint64_t> previousKey{0, 0};
    for (std::size_t index = 0; index < elements.size(); ++index) {
        LayerOutputBoundary boundary;
        const auto elementPath = joinPathIndex(path, index);
        if (!decodeLayerOutputBoundary(elements[index], state, elementPath, boundary)) {
            return false;
        }
        const std::pair<std::uint64_t, std::uint64_t> currentKey{boundary.layerId.value(),
                                                                 boundary.nodeId.value()};
        if (hasPrevious) {
            if (currentKey == previousKey) {
                state.fail(DocumentDecodeError::DuplicateLayerOutput,
                           joinPath(elementPath, "layerId"));
                return false;
            }
            if (currentKey < previousKey) {
                state.fail(DocumentDecodeError::UnsortedLayerOutputs,
                           joinPath(elementPath, "layerId"));
                return false;
            }
        }
        previousKey = currentKey;
        hasPrevious = true;
        out.push_back(std::move(boundary));
    }
    return true;
}

[[nodiscard]] bool
checkLayerOutputNodeReferences(DecodeState& state, const std::string& layerOutputsPath,
                               const std::vector<LayerOutputBoundary>& boundaries,
                               const std::vector<NodeRecord>& nodes) {
    for (std::size_t index = 0; index < boundaries.size(); ++index) {
        if (!nodeExists(nodes, boundaries[index].nodeId)) {
            state.fail(DocumentDecodeError::DanglingReference,
                       joinPath(joinPathIndex(layerOutputsPath, index), "nodeId"));
            return false;
        }
    }
    return true;
}

// A Layer Stack entry is a collection element identified by `slotId` (see
// docs/architecture/project-format.md, "Versions, Migrations, And Preservation": "slot ID for a
// Layer Stack entry"); see decodeComposition's own comment in document_decode.cpp for why a
// collection element's own trailing capture must be deferred until its identity member is
// decoded.
[[nodiscard]] bool decodeLayerStackEntry(const JsonValue& node, DecodeState& state,
                                         const std::string& path, LayerStackEntry& out) {
    static constexpr std::array<std::string_view, 2> keys{"slotId", "layerId"};
    std::vector<const JsonValue*> members;
    std::vector<RetainedJsonMember> trailing;
    if (!matchOrderedMembers(node, keys, true, state, path, members, trailing)) {
        return false;
    }
    LayerSlotId slotId;
    if (!decodeObjectId(*members[0], state, joinPath(path, "slotId"), slotId)) {
        return false;
    }

    const AttachmentScope layerStackEntryScope(state, RoundTripCollectionKind::LayerStackEntry,
                                               std::to_string(slotId.value()));
    if (!trailing.empty() && state.roundTrip != nullptr) {
        state.roundTrip->attach(state.attachmentPath, std::move(trailing));
    }

    LayerId layerId;
    if (!(state.documentMinor >= 6 && members[1]->kind() == JsonValueKind::Null) &&
        !decodeObjectId(*members[1], state, joinPath(path, "layerId"), layerId)) {
        return false;
    }
    out.slotId = slotId;
    out.layerId = layerId;
    return true;
}

// Layer Stack entries are canonical SOURCE order, never numerically sorted (see
// docs/architecture/project-format.md, "Canonical Graph": "Layer Stack entries remain in semantic
// top-to-bottom authoring order and are never ID-sorted"), so this deliberately applies no order or
// duplicate check beyond each entry's own wire shape.
[[nodiscard]] bool decodeLayerStackEntries(const JsonValue& node, DecodeState& state,
                                           const std::string& path,
                                           std::vector<LayerStackEntry>& out) {
    if (node.kind() != JsonValueKind::Array) {
        state.fail(DocumentDecodeError::WrongValueKind, path);
        return false;
    }
    const auto elements = node.arrayElements();
    out.clear();
    out.reserve(elements.size());
    for (std::size_t index = 0; index < elements.size(); ++index) {
        LayerStackEntry entry;
        if (!decodeLayerStackEntry(elements[index], state, joinPathIndex(path, index), entry)) {
            return false;
        }
        out.push_back(entry);
    }
    return true;
}

[[nodiscard]] bool decodeLayerStack(const JsonValue& node, DecodeState& state,
                                    const std::string& path, DecodedLayerStack& out) {
    std::vector<std::string_view> keys{"nodeId", "entries"};
    if (state.documentMinor >= 6 && node.findMember("enabled"))
        keys.push_back("enabled");
    std::vector<const JsonValue*> members;
    if (!matchOrderedMembers(node, keys, true, state, path, members)) {
        return false;
    }
    NodeId nodeId;
    if (!decodeObjectId(*members[0], state, joinPath(path, "nodeId"), nodeId)) {
        return false;
    }
    std::vector<LayerStackEntry> entries;
    if (!decodeLayerStackEntries(*members[1], state, joinPath(path, "entries"), entries)) {
        return false;
    }
    if (const auto* enabled = node.findMember("enabled"); enabled && state.documentMinor >= 6) {
        if (enabled->kind() != JsonValueKind::Boolean) {
            state.fail(DocumentDecodeError::WrongValueKind, joinPath(path, "enabled"));
            return false;
        }
        out.enabled = enabled->asBoolean().value_or(true);
    }
    out.nodeId = nodeId;
    out.entries = std::move(entries);
    return true;
}

[[nodiscard]] bool decodeGraph(const JsonValue& node, DecodeState& state, const std::string& path,
                               const std::vector<ParameterRecord>& parameters, DecodedGraph& out) {
    std::vector<std::string_view> keys{"nodes", "edges", "layerOutputs", "layerStack",
                                       "compositionOutput"};
    if (state.documentMinor >= 6 && node.findMember("merges"))
        keys.push_back("merges");
    std::vector<const JsonValue*> members;
    if (!matchOrderedMembers(node, keys, true, state, path, members)) {
        return false;
    }

    const auto nodesPath = joinPath(path, "nodes");
    if (!decodeNodes(*members[0], state, nodesPath, out.nodes)) {
        return false;
    }
    if (!checkNodeParameterBindings(state, nodesPath, out.nodes, parameters)) {
        return false;
    }

    const auto edgesPath = joinPath(path, "edges");
    if (!decodeEdges(*members[1], state, edgesPath, out.edges)) {
        return false;
    }
    if (!checkEdgeNodeReferences(state, edgesPath, out.edges, out.nodes)) {
        return false;
    }

    const auto layerOutputsPath = joinPath(path, "layerOutputs");
    if (!decodeLayerOutputs(*members[2], state, layerOutputsPath, out.layerOutputs)) {
        return false;
    }
    if (!checkLayerOutputNodeReferences(state, layerOutputsPath, out.layerOutputs, out.nodes)) {
        return false;
    }

    const auto layerStackPath = joinPath(path, "layerStack");
    {
        const AttachmentScope layerStackScope(state, "layerStack");
        if (!(state.documentMinor >= 6 && members[3]->kind() == JsonValueKind::Null) &&
            !decodeLayerStack(*members[3], state, layerStackPath, out.layerStack)) {
            return false;
        }
    }
    if (out.layerStack.nodeId.isValid() && !nodeExists(out.nodes, out.layerStack.nodeId)) {
        state.fail(DocumentDecodeError::DanglingReference, joinPath(layerStackPath, "nodeId"));
        return false;
    }

    if (const auto* merges = node.findMember("merges"); merges && state.documentMinor >= 6) {
        if (merges->kind() != JsonValueKind::Array) {
            state.fail(DocumentDecodeError::WrongValueKind, joinPath(path, "merges"));
            return false;
        }
        const AttachmentScope scope(state, "merges");
        for (const auto& value : merges->arrayElements()) {
            DecodedLayerStack stack;
            NodeId id;
            const auto* member = value.findMember("nodeId");
            if (!member || !decodeObjectId(*member, state, joinPath(path, "merges.nodeId"), id))
                return false;
            const AttachmentScope mergeScope(state, RoundTripCollectionKind::Node,
                                             std::to_string(id.value()));
            if (!decodeLayerStack(value, state, joinPath(path, "merges"), stack))
                return false;
            if (!nodeExists(out.nodes, stack.nodeId) || stack.nodeId == out.layerStack.nodeId ||
                std::ranges::any_of(
                    out.merges, [&](const auto& other) { return other.nodeId == stack.nodeId; })) {
                state.fail(DocumentDecodeError::DanglingReference, joinPath(path, "merges.nodeId"));
                return false;
            }
            out.merges.push_back(std::move(stack));
        }
    }
    const auto compositionOutputPath = joinPath(path, "compositionOutput");
    {
        const AttachmentScope compositionOutputScope(state, "compositionOutput");
        if (!decodeOutputPortRef(*members[4], state, compositionOutputPath,
                                 out.compositionOutput)) {
            return false;
        }
    }
    if (!nodeExists(out.nodes, out.compositionOutput.nodeId)) {
        state.fail(DocumentDecodeError::DanglingReference,
                   joinPath(compositionOutputPath, "nodeId"));
        return false;
    }

    return true;
}

} // namespace

namespace detail {

bool decodeCompositionInterior(const JsonValue& parametersNode,
                               const JsonValue& animationCurvesNode, const JsonValue& graphNode,
                               DecodeState& state, const std::string& parametersPath,
                               const std::string& curvesPath, const std::string& graphPath,
                               std::vector<document::ParameterRecord>& parameters,
                               std::vector<document::AnimationCurveRecord>& curves,
                               DecodedGraph& graph) {
    if (!decodeParameters(parametersNode, state, parametersPath, parameters)) {
        return false;
    }
    if (!decodeAnimationCurves(animationCurvesNode, state, curvesPath, curves)) {
        return false;
    }
    if (!checkParameterCurveReferences(state, parametersPath, parameters, curves)) {
        return false;
    }
    const AttachmentScope graphScope(state, "graph");
    return decodeGraph(graphNode, state, graphPath, parameters, graph);
}

} // namespace detail

} // namespace bloom::project
