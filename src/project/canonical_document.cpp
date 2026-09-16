#include <bloom/project/canonical_document.hpp>

#include <algorithm>
#include <bloom/core/utf8.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/persisted_text.hpp>
#include <bloom/project/canonical_base64.hpp>
#include <bloom/project/canonical_decimal.hpp>
#include <bloom/project/round_trip_state.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

using bloom::document::AnimationCurveRecord;
using bloom::document::Composition;
using bloom::document::ExtensionRecord;
using bloom::project::CanonicalDecimalText;
using bloom::project::CanonicalDocumentError;
using bloom::project::CanonicalDocumentLimits;
using bloom::project::CanonicalDocumentV1;
using bloom::project::CanonicalJsonWriter;
using bloom::project::CanonicalJsonWriterError;
using bloom::project::CanonicalJsonWriterResult;
using bloom::project::kCanonicalDocumentNoIndex;
using bloom::project::RetainedJsonMember;
using bloom::project::RetainedJsonValue;
using bloom::project::RetainedJsonValueKind;
using bloom::project::RoundTripAttachmentPath;
using bloom::project::RoundTripCollectionKind;
using bloom::project::RoundTripState;

constexpr auto kV1SchemaVersion = bloom::project::kCanonicalDocumentSchemaVersionV1;

// One canonical emission walk. Every collection ordered by a semantic value is materialized as an
// index order inside one of three disjoint scratch windows so nested orders never overlap: window
// zero holds the composition order, window one holds one composition-scoped collection at a time
// (parameters, curves, nodes, edges, boundaries) plus extension records, and window two holds one
// nested sub-order at a time (node bindings, curve keyframes).
struct SortPlan final {
    std::size_t window0 = 0;
    std::size_t window1 = 0;
    std::size_t window2 = 0;

    [[nodiscard]] std::size_t total() const noexcept { return window0 + window1 + window2; }
};

struct SortWindows final {
    std::span<std::size_t> window0{};
    std::span<std::size_t> window1{};
    std::span<std::size_t> window2{};
};

[[nodiscard]] std::size_t maximumOf(const std::size_t left, const std::size_t right) noexcept {
    return left > right ? left : right;
}

[[nodiscard]] SortPlan measureSortPlan(const bloom::document::Project& project) noexcept {
    SortPlan plan;
    plan.window0 = project.compositions().size();
    for (const auto& composition : project.compositions()) {
        plan.window1 = maximumOf(plan.window1, composition.parameters().records().size());
        plan.window1 = maximumOf(plan.window1, composition.animationCurves().records().size());
        plan.window1 = maximumOf(plan.window1, composition.graph().nodes().size());
        plan.window1 = maximumOf(plan.window1, composition.graph().edges().size());
        plan.window1 = maximumOf(plan.window1, composition.graph().layerOutputs().size());
        for (const auto& node : composition.graph().nodes()) {
            plan.window2 = maximumOf(plan.window2, node.parameters.size());
        }
        for (const auto& record : composition.animationCurves().records()) {
            if (const auto* scalar = std::get_if<bloom::document::ScalarAnimationCurve>(&record)) {
                plan.window2 = maximumOf(plan.window2, scalar->keyframes.size());
            } else if (const auto* vector =
                           std::get_if<bloom::document::Vec2AnimationCurve>(&record)) {
                plan.window2 = maximumOf(plan.window2, vector->keyframes.size());
                for (const auto& component : vector->components)
                    plan.window2 = maximumOf(plan.window2, component.keyframes.size());
            } else if (const auto* vector3 =
                           std::get_if<bloom::document::Vec3AnimationCurve>(&record)) {
                plan.window2 = maximumOf(plan.window2, vector3->keyframes.size());
                for (const auto& component : vector3->components)
                    plan.window2 = maximumOf(plan.window2, component.keyframes.size());
            } else if (const auto* color =
                           std::get_if<bloom::document::Color4AnimationCurve>(&record)) {
                plan.window2 = maximumOf(plan.window2, color->keyframes.size());
                for (const auto& component : color->components)
                    plan.window2 = maximumOf(plan.window2, component.keyframes.size());
            }
        }
    }
    plan.window1 = maximumOf(plan.window1, project.extensionRecords().size());
    return plan;
}

[[nodiscard]] SortWindows splitSortScratch(const std::span<std::size_t> scratch,
                                           const SortPlan& plan) noexcept {
    return {scratch.first(plan.window0), scratch.subspan(plan.window0, plan.window1),
            scratch.subspan(plan.window0 + plan.window1, plan.window2)};
}

// Fills the leading range.size() entries of window with the ascending index order of range by key.
// Duplicate keys are rejected because canonical collections are identity-unique.
template <typename Range, typename KeyOf>
[[nodiscard]] bool makeOrder(const Range& range, KeyOf key, const std::span<std::size_t> window,
                             std::span<const std::size_t>& order,
                             CanonicalDocumentError& error) noexcept {
    const auto count = range.size();
    if (count > window.size()) {
        error = CanonicalDocumentError::SortBufferTooSmall;
        return false;
    }
    for (std::size_t index = 0; index < count; ++index) {
        window[index] = index;
    }
    const auto byKey = [&range, &key](const std::size_t left, const std::size_t right) noexcept {
        return key(range[left]) < key(range[right]);
    };
    std::sort(window.begin(), window.begin() + static_cast<std::ptrdiff_t>(count), byKey);
    for (std::size_t index = 1; index < count; ++index) {
        if (!byKey(window[index - 1], window[index])) {
            error = CanonicalDocumentError::InvalidCollectionIdentity;
            return false;
        }
    }
    order = window.first(count);
    return true;
}

// RT2: a lightweight, allocation-free mirror of one bloom::project::RoundTripPathSegment used only
// to build the *current* attachment path during emission (see round_trip_state.hpp's file comment
// for the path/identity model this walk reproduces). `text` never owns storage: for a named
// segment it points at a string literal or an already-alive live-document string (e.g. a parameter
// binding's `role`); for a collection element it points at a caller-local decimal spelling
// (bloom::project::formatCanonicalUInt64's fixed-buffer result) that outlives the PathScope
// referencing it. Kept trivially copyable/assignable so a fixed std::array can hold the whole path
// with no heap allocation at any point in RT2's overlay walk.
struct EmitPathSegment final {
    bool isCollectionElement = false;
    RoundTripCollectionKind kind{};
    std::string_view text;
};

// Deepest attachment path in the fixed v1 schema tree is five segments (for example a vec2
// keyframe's own "value" object: project/Composition/AnimationCurve/Keyframe/value). This budget
// keeps generous headroom without needing a heap-backed path.
constexpr std::size_t kMaxAttachmentDepth = 12;

struct WalkState final {
    CanonicalDocumentError error = CanonicalDocumentError::None;
    std::size_t compositionIndex = kCanonicalDocumentNoIndex;
    std::size_t elementIndex = kCanonicalDocumentNoIndex;

    void fail(const CanonicalDocumentError failure,
              const std::size_t failedCompositionIndex = kCanonicalDocumentNoIndex,
              const std::size_t failedElementIndex = kCanonicalDocumentNoIndex) noexcept {
        if (error == CanonicalDocumentError::None) {
            error = failure;
            compositionIndex = failedCompositionIndex;
            elementIndex = failedElementIndex;
        }
    }
};

struct EmitState final {
    CanonicalJsonWriter& writer;
    const bloom::document::Project& project;
    const bloom::document::IdAllocatorHighWater highWater;
    const bloom::document::ColorSettings& colorSettings;
    SortWindows sort;
    std::span<char> payloadScratch;
    WalkState walk{};
    // RT2 overlay: null (the default) reproduces the plain writer exactly -- findRetained() below
    // short-circuits before ever touching attachmentPath, so a plain write pushes/pops path
    // segments but never performs a lookup or allocates.
    const RoundTripState* roundTrip = nullptr;
    std::uint32_t schemaMinor = 0;
    std::array<EmitPathSegment, kMaxAttachmentDepth> attachmentPath{};
    std::size_t attachmentDepth = 0;
    // Count of attachment points this walk actually found and re-emitted from *roundTrip. Compared
    // against roundTrip->entries().size() once the walk finishes successfully (see
    // emitDocumentRoot): every entry in a RoundTripState captured against this exact document is
    // visited exactly once by construction, so a leftover entry means the caller passed state that
    // does not describe the document actually being written.
    std::size_t retainedConsumed = 0;

    [[nodiscard]] bool ok(const CanonicalJsonWriterResult result) noexcept {
        if (result) {
            return true;
        }
        if (walk.error == CanonicalDocumentError::None) {
            switch (result.error()) {
            case CanonicalJsonWriterError::ValueLimitExceeded:
                walk.error = CanonicalDocumentError::ValueCountExceeded;
                break;
            case CanonicalJsonWriterError::ContainerLimitExceeded:
                walk.error = CanonicalDocumentError::ContainerEntryCountExceeded;
                break;
            default:
                // Validation proved every other writer failure impossible for trusted snapshots.
                std::terminate();
            }
        }
        return false;
    }
};

// RAII scope that pushes one attachment-path segment for the duration of emitting one nested
// singleton member or collection element, and pops it back off on destruction -- the writer-side
// mirror of document_decode_internal.hpp's detail::AttachmentScope. Pushing/popping is
// unconditional (cheap array writes) regardless of whether an overlay is active; only
// findRetained() below is conditioned on state.roundTrip being non-null.
class PathScope final {
  public:
    PathScope(EmitState& state, const std::string_view name) noexcept : state_(state) {
        push(EmitPathSegment{false, {}, name});
    }
    PathScope(EmitState& state, const RoundTripCollectionKind kind,
              const std::string_view identity) noexcept
        : state_(state) {
        push(EmitPathSegment{true, kind, identity});
    }
    PathScope(const PathScope&) = delete;
    PathScope& operator=(const PathScope&) = delete;
    PathScope(PathScope&&) = delete;
    PathScope& operator=(PathScope&&) = delete;
    ~PathScope() { --state_.attachmentDepth; }

  private:
    void push(const EmitPathSegment segment) noexcept {
        // The fixed v1 schema tree never nests this deep (see kMaxAttachmentDepth's comment); a
        // request that somehow did would be a programming error in this file, not a document
        // condition to report through a typed CanonicalDocumentError.
        if (state_.attachmentDepth >= kMaxAttachmentDepth) {
            std::terminate();
        }
        state_.attachmentPath[state_.attachmentDepth] = segment;
        ++state_.attachmentDepth;
    }

    EmitState& state_;
};

[[nodiscard]] bool pathsMatch(const std::span<const EmitPathSegment> current,
                              const RoundTripAttachmentPath& entryPath) noexcept {
    if (current.size() != entryPath.size()) {
        return false;
    }
    for (std::size_t index = 0; index < current.size(); ++index) {
        const auto& a = current[index];
        const auto& b = entryPath[index];
        if (a.isCollectionElement != b.isCollectionElement()) {
            return false;
        }
        if (a.isCollectionElement) {
            if (a.kind != b.collectionKind() || a.text != b.identity()) {
                return false;
            }
        } else if (a.text != b.name()) {
            return false;
        }
    }
    return true;
}

// Linear scan over the (bounded) retained-entry list, mirroring RoundTripState::find()'s own
// linear lookup but comparing against the caller-local EmitPathSegment path rather than
// constructing a heap-owned RoundTripAttachmentPath -- keeping every RT2 overlay lookup
// allocation-free.
[[nodiscard]] const std::vector<RetainedJsonMember>* findRetained(EmitState& state) noexcept {
    if (state.roundTrip == nullptr) {
        return nullptr;
    }
    const std::span<const EmitPathSegment> current(state.attachmentPath.data(),
                                                   state.attachmentDepth);
    for (const auto& entry : state.roundTrip->entries()) {
        if (pathsMatch(current, entry.path)) {
            return &entry.members;
        }
    }
    return nullptr;
}

// Recursively re-emits one retained value exactly as RT1 captured it: null/bool as literals,
// strings through the ordinary canonical string writer, numbers through UnknownJsonNumber's
// canonical spelling surface, and arrays/objects recursively with retained member order preserved
// verbatim (opaque content -- see round_trip_state.hpp's documented rule that a value nested
// inside an already-unknown member carries no schema of its own and is never re-sorted).
[[nodiscard]] bool emitRetainedValue(EmitState& state, const RetainedJsonValue& value) noexcept {
    auto& writer = state.writer;
    switch (value.kind()) {
    case RetainedJsonValueKind::Null:
        return state.ok(writer.nullValue());
    case RetainedJsonValueKind::Boolean:
        return state.ok(writer.booleanValue(value.asBoolean()));
    case RetainedJsonValueKind::Number:
        return state.ok(writer.unknownNumberValue(value.asNumber()));
    case RetainedJsonValueKind::String:
        return state.ok(writer.stringValue(value.asString()));
    case RetainedJsonValueKind::Array: {
        if (!state.ok(writer.beginArray())) {
            return false;
        }
        for (const auto& element : value.elements()) {
            if (!emitRetainedValue(state, element)) {
                return false;
            }
        }
        return state.ok(writer.endArray());
    }
    case RetainedJsonValueKind::Object: {
        if (!state.ok(writer.beginObject())) {
            return false;
        }
        for (const auto& member : value.members()) {
            if (!state.ok(writer.memberName(member.key())) ||
                !emitRetainedValue(state, member.value())) {
                return false;
            }
        }
        return state.ok(writer.endObject());
    }
    }
    return false;
}

// Looks up the current attachment path and, if RoundTripState retained members there, re-emits
// each one (key then recursively its value) in the stored order -- already ascending UTF-8 by
// RT1's capture guarantee, so no re-sort happens here. A no-op (returns true without writing
// anything) when there is no overlay or no entry at this exact path, which is the overwhelmingly
// common case for every attachment point in a plain or lightly-annotated document.
[[nodiscard]] bool emitRetainedTrailing(EmitState& state) noexcept {
    const auto* members = findRetained(state);
    if (members == nullptr) {
        return true;
    }
    ++state.retainedConsumed;
    for (const auto& member : *members) {
        if (!state.ok(state.writer.memberName(member.key())) ||
            !emitRetainedValue(state, member.value())) {
            return false;
        }
    }
    return true;
}

// `attach` is true only at the four attachment-point schemaVersion sites (colorSettings.
// schemaVersion, ocioConfig.schemaVersion, an extension record's schemaVersion, and an
// owner-remapper's version); the document root's own schemaVersion is written with the default
// `attach = false` and no PathScope pushed, matching the decode-side bootstrap constraint that the
// root schemaVersion object is always decoded before documentMinor/roundTrip are established and so
// can never itself retain a trailing member (see document_decode.cpp's decodeDocumentEnvelope).
[[nodiscard]] bool emitVersion(EmitState& state, const bloom::document::SchemaVersion version,
                               const bool attach = false) noexcept {
    auto& writer = state.writer;
    if (!state.ok(writer.beginObject())) {
        return false;
    }
    if (!state.ok(writer.memberName("major")) || !state.ok(writer.integerValue(version.major))) {
        return false;
    }
    if (!state.ok(writer.memberName("minor")) || !state.ok(writer.integerValue(version.minor))) {
        return false;
    }
    if (attach && !emitRetainedTrailing(state)) {
        return false;
    }
    return state.ok(writer.endObject());
}

[[nodiscard]] bool emitNamedId(EmitState& state, const std::string_view name,
                               const std::uint64_t value) noexcept {
    const auto text = bloom::project::formatCanonicalUInt64(value);
    return state.ok(state.writer.memberName(name)) &&
           state.ok(state.writer.stringValue(text.view()));
}

[[nodiscard]] bool emitNamedSigned(EmitState& state, const std::string_view name,
                                   const std::int64_t value) noexcept {
    const auto text = bloom::project::formatCanonicalInt64(value);
    return state.ok(state.writer.memberName(name)) &&
           state.ok(state.writer.stringValue(text.view()));
}

// Every call site of this shape (composition duration, format pixelAspect/frameRate, a keyframe's
// time) is a genuine attachment point -- unlike emitVersion, there is no "never attach" caller --
// so the caller only needs to push the right PathScope first; this always looks up and re-emits.
[[nodiscard]] bool emitRational(EmitState& state, const std::int64_t numerator,
                                const std::int64_t denominator) noexcept {
    if (!state.ok(state.writer.beginObject())) {
        return false;
    }
    if (!emitNamedSigned(state, "numerator", numerator)) {
        return false;
    }
    if (!emitNamedSigned(state, "denominator", denominator)) {
        return false;
    }
    if (!emitRetainedTrailing(state)) {
        return false;
    }
    return state.ok(state.writer.endObject());
}

// Every call site (edge source, compositionOutput) is a genuine attachment point; the caller pushes
// the right PathScope first.
[[nodiscard]] bool emitOutputPortRef(EmitState& state,
                                     const bloom::document::OutputPortRef& reference) noexcept {
    if (!state.ok(state.writer.beginObject())) {
        return false;
    }
    if (!emitNamedId(state, "nodeId", reference.nodeId.value())) {
        return false;
    }
    if (!state.ok(state.writer.memberName("port")) ||
        !state.ok(state.writer.stringValue(reference.port))) {
        return false;
    }
    if (!emitRetainedTrailing(state)) {
        return false;
    }
    return state.ok(state.writer.endObject());
}

[[nodiscard]] std::string_view
extensionTargetKind(const bloom::document::ExtensionTarget& target) noexcept {
    using namespace bloom::document;
    if (std::holds_alternative<ProjectId>(target)) {
        return "project";
    }
    if (std::holds_alternative<CompositionId>(target)) {
        return "composition";
    }
    if (std::holds_alternative<NodeId>(target)) {
        return "node";
    }
    if (std::holds_alternative<EdgeId>(target)) {
        return "edge";
    }
    if (std::holds_alternative<LayerId>(target)) {
        return "layer";
    }
    if (std::holds_alternative<LayerSlotId>(target)) {
        return "layer-slot";
    }
    if (std::holds_alternative<ParameterId>(target)) {
        return "parameter";
    }
    if (std::holds_alternative<AnimationCurveId>(target)) {
        return "animation-curve";
    }
    return "keyframe";
}

[[nodiscard]] std::uint64_t
extensionTargetValue(const bloom::document::ExtensionTarget& target) noexcept {
    if (const auto* projectId = std::get_if<bloom::document::ProjectId>(&target)) {
        return projectId->value();
    }
    if (const auto* compositionId = std::get_if<bloom::document::CompositionId>(&target)) {
        return compositionId->value();
    }
    if (const auto* nodeId = std::get_if<bloom::document::NodeId>(&target)) {
        return nodeId->value();
    }
    if (const auto* edgeId = std::get_if<bloom::document::EdgeId>(&target)) {
        return edgeId->value();
    }
    if (const auto* layerId = std::get_if<bloom::document::LayerId>(&target)) {
        return layerId->value();
    }
    if (const auto* layerSlotId = std::get_if<bloom::document::LayerSlotId>(&target)) {
        return layerSlotId->value();
    }
    if (const auto* parameterId = std::get_if<bloom::document::ParameterId>(&target)) {
        return parameterId->value();
    }
    if (const auto* curveId = std::get_if<bloom::document::AnimationCurveId>(&target)) {
        return curveId->value();
    }
    const auto* keyframeId = std::get_if<bloom::document::KeyframeId>(&target);
    return keyframeId == nullptr ? 0 : keyframeId->value();
}

// Every call site (an extension record's non-null subject, a host reference's target) is a genuine
// attachment point; the caller pushes the right PathScope first.
[[nodiscard]] bool emitTypedTarget(EmitState& state,
                                   const bloom::document::ExtensionTarget& target) noexcept {
    if (!state.ok(state.writer.beginObject())) {
        return false;
    }
    if (!state.ok(state.writer.memberName("kind")) ||
        !state.ok(state.writer.stringValue(extensionTargetKind(target)))) {
        return false;
    }
    if (!emitNamedId(state, "id", extensionTargetValue(target))) {
        return false;
    }
    if (!emitRetainedTrailing(state)) {
        return false;
    }
    return state.ok(state.writer.endObject());
}

[[nodiscard]] bool emitConstantValue(EmitState& state,
                                     const bloom::document::ParameterValue& value) noexcept {
    using namespace bloom::document;
    auto& writer = state.writer;
    if (!state.ok(writer.beginObject())) {
        return false;
    }
    if (const auto* boolean = std::get_if<bool>(&value)) {
        return state.ok(writer.memberName("kind")) && state.ok(writer.stringValue("bool")) &&
               state.ok(writer.memberName("value")) && state.ok(writer.booleanValue(*boolean)) &&
               emitRetainedTrailing(state) && state.ok(writer.endObject());
    }
    if (const auto* integer = std::get_if<std::int64_t>(&value)) {
        return state.ok(writer.memberName("kind")) && state.ok(writer.stringValue("int64")) &&
               emitNamedSigned(state, "value", *integer) && emitRetainedTrailing(state) &&
               state.ok(writer.endObject());
    }
    if (const auto* floating = std::get_if<double>(&value)) {
        return state.ok(writer.memberName("kind")) && state.ok(writer.stringValue("float64")) &&
               state.ok(writer.memberName("value")) && state.ok(writer.float64Value(*floating)) &&
               emitRetainedTrailing(state) && state.ok(writer.endObject());
    }
    if (const auto* vector = std::get_if<Vec2d>(&value)) {
        return state.ok(writer.memberName("kind")) && state.ok(writer.stringValue("vec2")) &&
               state.ok(writer.memberName("x")) && state.ok(writer.float64Value(vector->x)) &&
               state.ok(writer.memberName("y")) && state.ok(writer.float64Value(vector->y)) &&
               emitRetainedTrailing(state) && state.ok(writer.endObject());
    }
    if (const auto* vector = std::get_if<Vec3d>(&value)) {
        // Document 1.4. Its own kind token rather than a third component appended to "vec2": the
        // format's discriminators name TYPES, and a three-component vector is a different type from
        // a two-component one -- which is also why an older minor refuses to decode this kind at
        // all rather than reading it as a vec2 with a stray member.
        return state.ok(writer.memberName("kind")) && state.ok(writer.stringValue("vec3")) &&
               state.ok(writer.memberName("x")) && state.ok(writer.float64Value(vector->x)) &&
               state.ok(writer.memberName("y")) && state.ok(writer.float64Value(vector->y)) &&
               state.ok(writer.memberName("z")) && state.ok(writer.float64Value(vector->z)) &&
               emitRetainedTrailing(state) && state.ok(writer.endObject());
    }
    if (const auto* color = std::get_if<bloom::core::Color4d>(&value)) {
        return state.ok(writer.memberName("kind")) && state.ok(writer.stringValue("color4")) &&
               state.ok(writer.memberName("red")) && state.ok(writer.float64Value(color->red)) &&
               state.ok(writer.memberName("green")) &&
               state.ok(writer.float64Value(color->green)) && state.ok(writer.memberName("blue")) &&
               state.ok(writer.float64Value(color->blue)) && state.ok(writer.memberName("alpha")) &&
               state.ok(writer.float64Value(color->alpha)) && emitRetainedTrailing(state) &&
               state.ok(writer.endObject());
    }
    if (const auto* text = std::get_if<std::string>(&value)) {
        return state.ok(writer.memberName("kind")) && state.ok(writer.stringValue("string")) &&
               state.ok(writer.memberName("value")) && state.ok(writer.stringValue(*text)) &&
               emitRetainedTrailing(state) && state.ok(writer.endObject());
    }
    if (const auto* rational = std::get_if<bloom::core::RationalTime>(&value)) {
        return state.ok(writer.memberName("kind")) && state.ok(writer.stringValue("rational")) &&
               emitNamedSigned(state, "numerator", rational->numerator()) &&
               emitNamedSigned(state, "denominator", rational->denominator()) &&
               emitRetainedTrailing(state) && state.ok(writer.endObject());
    }
    return false;
}

[[nodiscard]] bool emitParameter(EmitState& state, const bloom::document::ParameterRecord& record,
                                 const std::size_t parameterRank) noexcept {
    using namespace bloom::document;
    auto& writer = state.writer;
    const auto idText = bloom::project::formatCanonicalUInt64(record.id.value());
    const PathScope parameterScope(state, RoundTripCollectionKind::Parameter, idText.view());
    if (!state.ok(writer.beginObject())) {
        return false;
    }
    if (!emitNamedId(state, "id", record.id.value())) {
        return false;
    }
    if (!state.ok(writer.memberName("schemaKey")) ||
        !state.ok(writer.stringValue(record.schemaKey))) {
        return false;
    }
    if (!state.ok(writer.memberName("source")) || !state.ok(writer.beginObject())) {
        return false;
    }
    {
        const PathScope sourceScope(state, "source");
        if (const auto* constant = std::get_if<ConstantValueSource>(&record.source)) {
            if (!state.ok(writer.memberName("kind")) || !state.ok(writer.stringValue("constant"))) {
                return false;
            }
            if (!state.ok(writer.memberName("value"))) {
                return false;
            }
            const PathScope valueScope(state, "value");
            if (!emitConstantValue(state, constant->value)) {
                state.walk.fail(CanonicalDocumentError::InvalidParameter,
                                state.walk.compositionIndex, parameterRank);
                return false;
            }
        } else if (const auto* curve = std::get_if<AnimationCurveSource>(&record.source)) {
            if (!state.ok(writer.memberName("kind")) ||
                !state.ok(writer.stringValue("animation-curve"))) {
                return false;
            }
            if (!emitNamedId(state, "curveId", curve->curveId.value())) {
                return false;
            }
            if (curve->defaultValue.has_value()) {
                if (!state.ok(writer.memberName("defaultValue"))) {
                    return false;
                }
                const PathScope valueScope(state, "defaultValue");
                if (!emitConstantValue(state, *curve->defaultValue)) {
                    state.walk.fail(CanonicalDocumentError::InvalidParameter,
                                    state.walk.compositionIndex, parameterRank);
                    return false;
                }
            }
        } else if (const auto* driver = std::get_if<DriverBindingSource>(&record.source)) {
            // Document 1.4. The durable record is exactly the pair it addresses, written the same
            // way an edge's source is -- an object id and a port name -- because that is what a
            // driver is: an ordinary port reference that lands in parameter-address space.
            if (!state.ok(writer.memberName("kind")) || !state.ok(writer.stringValue("driver"))) {
                return false;
            }
            if (!emitNamedId(state, "sourceNodeId", driver->sourceNodeId.value())) {
                return false;
            }
            if (!state.ok(writer.memberName("outputPort")) ||
                !state.ok(writer.stringValue(driver->outputPort))) {
                return false;
            }
        } else {
            // Unreachable: ParameterSource is a closed three-alternative variant and all three are
            // handled above. Reported rather than assumed, matching how every other
            // cannot-occur branch in this writer behaves.
            state.walk.fail(CanonicalDocumentError::InvalidParameter, state.walk.compositionIndex,
                            parameterRank);
            return false;
        }
        if (!emitRetainedTrailing(state)) {
            return false;
        }
    }
    if (!state.ok(writer.endObject())) {
        return false;
    }
    if (!emitRetainedTrailing(state)) {
        return false;
    }
    return state.ok(writer.endObject());
}

// A 1.12 ease handle is emitted ONLY when it is non-default. That is what keeps an existing
// document byte-identical through a re-encode apart from its schema minor: every key written
// before handles existed holds the bitwise default, so nothing new appears in its object.
[[nodiscard]] bool emitKeyframeHandle(EmitState& state, const std::string_view name,
                                      const bloom::document::KeyframeHandle& handle) noexcept {
    if (bloom::document::isDefaultKeyframeHandle(handle)) {
        return true;
    }
    auto& writer = state.writer;
    return state.ok(writer.memberName(name)) && state.ok(writer.beginObject()) &&
           state.ok(writer.memberName("time")) && state.ok(writer.float64Value(handle.time)) &&
           state.ok(writer.memberName("value")) && state.ok(writer.float64Value(handle.value)) &&
           state.ok(writer.endObject());
}

[[nodiscard]] bool emitKeyframeHandles(EmitState& state,
                                       const bloom::document::ScalarKeyframe& key) noexcept {
    return emitKeyframeHandle(state, "outgoingHandle", key.outgoingHandle) &&
           emitKeyframeHandle(state, "incomingHandle", key.incomingHandle);
}

[[nodiscard]] bool
emitInterpolation(EmitState& state,
                  const bloom::document::KeyframeInterpolation interpolation) noexcept {
    switch (interpolation) {
    case bloom::document::KeyframeInterpolation::Hold:
        return state.ok(state.writer.stringValue("hold"));
    case bloom::document::KeyframeInterpolation::Linear:
        return state.ok(state.writer.stringValue("linear"));
    case bloom::document::KeyframeInterpolation::EaseInOut:
        return state.ok(state.writer.stringValue("ease-in-out"));
    }
    return false;
}

[[nodiscard]] bool emitScalarKeyframe(EmitState& state,
                                      const bloom::document::ScalarKeyframe& key) noexcept {
    auto& writer = state.writer;
    const auto idText = bloom::project::formatCanonicalUInt64(key.id.value());
    const PathScope keyframeScope(state, RoundTripCollectionKind::Keyframe, idText.view());
    if (!state.ok(writer.beginObject())) {
        return false;
    }
    if (!emitNamedId(state, "id", key.id.value())) {
        return false;
    }
    if (!state.ok(writer.memberName("time"))) {
        return false;
    }
    {
        const PathScope timeScope(state, "time");
        if (!emitRational(state, key.time.numerator(), key.time.denominator())) {
            return false;
        }
    }
    if (!state.ok(writer.memberName("value"))) {
        return false;
    }
    if (!state.ok(writer.float64Value(key.value))) {
        return false;
    }
    if (!state.ok(writer.memberName("outgoingInterpolation"))) {
        return false;
    }
    if (!emitInterpolation(state, key.outgoingInterpolation)) {
        return false;
    }
    if (!emitKeyframeHandles(state, key)) {
        return false;
    }
    if (!emitRetainedTrailing(state)) {
        return false;
    }
    return state.ok(writer.endObject());
}

[[nodiscard]] bool emitVec2Keyframe(EmitState& state,
                                    const bloom::document::Vec2Keyframe& key) noexcept {
    auto& writer = state.writer;
    const auto idText = bloom::project::formatCanonicalUInt64(key.id.value());
    const PathScope keyframeScope(state, RoundTripCollectionKind::Keyframe, idText.view());
    if (!state.ok(writer.beginObject())) {
        return false;
    }
    if (!emitNamedId(state, "id", key.id.value())) {
        return false;
    }
    if (!state.ok(writer.memberName("time"))) {
        return false;
    }
    {
        const PathScope timeScope(state, "time");
        if (!emitRational(state, key.time.numerator(), key.time.denominator())) {
            return false;
        }
    }
    if (!state.ok(writer.memberName("value")) || !state.ok(writer.beginObject())) {
        return false;
    }
    {
        const PathScope valueScope(state, "value");
        if (!state.ok(writer.memberName("x")) || !state.ok(writer.float64Value(key.value.x))) {
            return false;
        }
        if (!state.ok(writer.memberName("y")) || !state.ok(writer.float64Value(key.value.y))) {
            return false;
        }
        if (!emitRetainedTrailing(state)) {
            return false;
        }
    }
    if (!state.ok(writer.endObject())) {
        return false;
    }
    if (!state.ok(writer.memberName("outgoingInterpolation"))) {
        return false;
    }
    if (!emitInterpolation(state, key.outgoingInterpolation)) {
        return false;
    }
    if (!emitRetainedTrailing(state)) {
        return false;
    }
    return state.ok(writer.endObject());
}

[[nodiscard]] std::string_view
componentName(const bloom::document::AnimationComponent component) noexcept {
    switch (component) {
    case bloom::document::AnimationComponent::X:
        return "x";
    case bloom::document::AnimationComponent::Y:
        return "y";
    case bloom::document::AnimationComponent::Z:
        return "z";
    case bloom::document::AnimationComponent::Red:
        return "red";
    case bloom::document::AnimationComponent::Green:
        return "green";
    case bloom::document::AnimationComponent::Blue:
        return "blue";
    case bloom::document::AnimationComponent::Alpha:
        return "alpha";
    }
    return {};
}

[[nodiscard]] bool emitComponentKeyframe(EmitState& state,
                                         const bloom::document::AnimationComponent component,
                                         const bloom::document::ScalarKeyframe& key) noexcept {
    auto& writer = state.writer;
    const auto idText = bloom::project::formatCanonicalUInt64(key.id.value());
    const PathScope keyframeScope(state, RoundTripCollectionKind::Keyframe, idText.view());
    if (!state.ok(writer.beginObject()) || !emitNamedId(state, "id", key.id.value()) ||
        !state.ok(writer.memberName("component")) ||
        !state.ok(writer.stringValue(componentName(component))) ||
        !state.ok(writer.memberName("time"))) {
        return false;
    }
    {
        const PathScope timeScope(state, "time");
        if (!emitRational(state, key.time.numerator(), key.time.denominator()))
            return false;
    }
    if (!state.ok(writer.memberName("value")) || !state.ok(writer.float64Value(key.value)) ||
        !state.ok(writer.memberName("outgoingInterpolation")) ||
        !emitInterpolation(state, key.outgoingInterpolation) || !emitKeyframeHandles(state, key) ||
        !emitRetainedTrailing(state) || !state.ok(writer.endObject())) {
        return false;
    }
    return true;
}

[[nodiscard]] bool emitColor4Keyframe(EmitState& state,
                                      const bloom::document::Color4Keyframe& key) noexcept {
    auto& writer = state.writer;
    const auto idText = bloom::project::formatCanonicalUInt64(key.id.value());
    const PathScope keyframeScope(state, RoundTripCollectionKind::Keyframe, idText.view());
    if (!state.ok(writer.beginObject())) {
        return false;
    }
    if (!emitNamedId(state, "id", key.id.value())) {
        return false;
    }
    if (!state.ok(writer.memberName("time"))) {
        return false;
    }
    {
        const PathScope timeScope(state, "time");
        if (!emitRational(state, key.time.numerator(), key.time.denominator())) {
            return false;
        }
    }
    // Channel order is the authoring order core::Color4d declares and the constant colour value
    // already writes (red, green, blue, alpha) -- the same member order, so a colour key and a
    // colour constant read identically on the wire.
    if (!state.ok(writer.memberName("value")) || !state.ok(writer.beginObject())) {
        return false;
    }
    {
        const PathScope valueScope(state, "value");
        if (!state.ok(writer.memberName("red")) || !state.ok(writer.float64Value(key.value.red))) {
            return false;
        }
        if (!state.ok(writer.memberName("green")) ||
            !state.ok(writer.float64Value(key.value.green))) {
            return false;
        }
        if (!state.ok(writer.memberName("blue")) ||
            !state.ok(writer.float64Value(key.value.blue))) {
            return false;
        }
        if (!state.ok(writer.memberName("alpha")) ||
            !state.ok(writer.float64Value(key.value.alpha))) {
            return false;
        }
        if (!emitRetainedTrailing(state)) {
            return false;
        }
    }
    if (!state.ok(writer.endObject())) {
        return false;
    }
    if (!state.ok(writer.memberName("outgoingInterpolation"))) {
        return false;
    }
    if (!emitInterpolation(state, key.outgoingInterpolation)) {
        return false;
    }
    if (!emitRetainedTrailing(state)) {
        return false;
    }
    return state.ok(writer.endObject());
}

[[nodiscard]] bool emitAnimationCurves(EmitState& state, const Composition& composition,
                                       const std::size_t compositionIndex) noexcept {
    using namespace bloom::document;
    auto& writer = state.writer;
    if (!state.ok(writer.memberName("animationCurves")) || !state.ok(writer.beginArray())) {
        return false;
    }
    const auto& records = composition.animationCurves().records();
    std::span<const std::size_t> order;
    if (!makeOrder(
            records,
            [](const AnimationCurveRecord& record) noexcept {
                return animationCurveId(record).value();
            },
            state.sort.window1, order, state.walk.error)) {
        {
            state.walk.fail(CanonicalDocumentError::InvalidCollectionIdentity, compositionIndex);
            return false;
        }
    }
    for (const auto recordIndex : order) {
        const auto& record = records[recordIndex];
        const auto idText = bloom::project::formatCanonicalUInt64(animationCurveId(record).value());
        const PathScope curveScope(state, RoundTripCollectionKind::AnimationCurve, idText.view());
        if (!state.ok(writer.beginObject())) {
            return false;
        }
        if (!emitNamedId(state, "id", animationCurveId(record).value())) {
            return false;
        }
        if (const auto* scalar = std::get_if<ScalarAnimationCurve>(&record)) {
            if (!state.ok(writer.memberName("kind")) || !state.ok(writer.stringValue("scalar")) ||
                !state.ok(writer.memberName("keyframes")) || !state.ok(writer.beginArray())) {
                return false;
            }
            std::span<const std::size_t> keyOrder;
            if (!makeOrder(
                    scalar->keyframes, [](const ScalarKeyframe& key) noexcept { return key.time; },
                    state.sort.window2, keyOrder, state.walk.error)) {
                {
                    state.walk.fail(CanonicalDocumentError::InvalidCollectionIdentity,
                                    compositionIndex);
                    return false;
                }
            }
            for (const auto keyIndex : keyOrder) {
                if (!emitScalarKeyframe(state, scalar->keyframes[keyIndex])) {
                    {
                        state.walk.fail(CanonicalDocumentError::InvalidAnimationCurve,
                                        compositionIndex);
                        return false;
                    }
                }
            }
        } else if (const auto* vector = std::get_if<Vec2AnimationCurve>(&record)) {
            if (!state.ok(writer.memberName("kind")) || !state.ok(writer.stringValue("vec2")) ||
                !state.ok(writer.memberName("keyframes")) || !state.ok(writer.beginArray())) {
                return false;
            }
            const bool componentAware =
                std::ranges::any_of(vector->components, [](const auto& component) {
                    return !component.keyframes.empty();
                });
            if (componentAware) {
                constexpr std::array names{AnimationComponent::X, AnimationComponent::Y};
                for (std::size_t componentIndex = 0; componentIndex < names.size();
                     ++componentIndex) {
                    std::span<const std::size_t> keyOrder;
                    if (!makeOrder(
                            vector->components[componentIndex].keyframes,
                            [](const ScalarKeyframe& key) noexcept { return key.time; },
                            state.sort.window2, keyOrder, state.walk.error))
                        return false;
                    for (const auto keyIndex : keyOrder) {
                        if (!emitComponentKeyframe(
                                state, names[componentIndex],
                                vector->components[componentIndex].keyframes[keyIndex]))
                            return false;
                    }
                }
            } else {
                std::span<const std::size_t> keyOrder;
                if (!makeOrder(
                        vector->keyframes,
                        [](const Vec2Keyframe& key) noexcept { return key.time; },
                        state.sort.window2, keyOrder, state.walk.error))
                    return false;
                for (const auto keyIndex : keyOrder)
                    if (!emitVec2Keyframe(state, vector->keyframes[keyIndex]))
                        return false;
            }
        } else if (const auto* vector3 = std::get_if<Vec3AnimationCurve>(&record)) {
            if (!state.ok(writer.memberName("kind")) || !state.ok(writer.stringValue("vec3")) ||
                !state.ok(writer.memberName("keyframes")) || !state.ok(writer.beginArray()))
                return false;
            constexpr std::array names{AnimationComponent::X, AnimationComponent::Y,
                                       AnimationComponent::Z};
            const bool componentAware =
                std::ranges::any_of(vector3->components, [](const auto& component) {
                    return !component.keyframes.empty();
                });
            if (componentAware) {
                for (std::size_t componentIndex = 0; componentIndex < names.size();
                     ++componentIndex) {
                    std::span<const std::size_t> keyOrder;
                    if (!makeOrder(
                            vector3->components[componentIndex].keyframes,
                            [](const ScalarKeyframe& key) noexcept { return key.time; },
                            state.sort.window2, keyOrder, state.walk.error))
                        return false;
                    for (const auto keyIndex : keyOrder)
                        if (!emitComponentKeyframe(
                                state, names[componentIndex],
                                vector3->components[componentIndex].keyframes[keyIndex]))
                            return false;
                }
            } else {
                for (const auto& key : vector3->keyframes) {
                    const auto value = key.value;
                    constexpr std::array names3{AnimationComponent::X, AnimationComponent::Y,
                                                AnimationComponent::Z};
                    const std::array values{value.x, value.y, value.z};
                    for (std::size_t componentIndex = 0; componentIndex < names3.size();
                         ++componentIndex) {
                        if (!emitComponentKeyframe(state, names3[componentIndex],
                                                   ScalarKeyframe{key.id, key.time,
                                                                  values[componentIndex],
                                                                  key.outgoingInterpolation}))
                            return false;
                    }
                }
            }
        } else if (const auto* color = std::get_if<Color4AnimationCurve>(&record)) {
            if (!state.ok(writer.memberName("kind")) || !state.ok(writer.stringValue("color4")) ||
                !state.ok(writer.memberName("keyframes")) || !state.ok(writer.beginArray())) {
                return false;
            }
            const bool componentAware =
                std::ranges::any_of(color->components, [](const auto& component) {
                    return !component.keyframes.empty();
                });
            if (componentAware) {
                constexpr std::array names{AnimationComponent::Red, AnimationComponent::Green,
                                           AnimationComponent::Blue, AnimationComponent::Alpha};
                for (std::size_t componentIndex = 0; componentIndex < names.size();
                     ++componentIndex) {
                    std::span<const std::size_t> keyOrder;
                    if (!makeOrder(
                            color->components[componentIndex].keyframes,
                            [](const ScalarKeyframe& key) noexcept { return key.time; },
                            state.sort.window2, keyOrder, state.walk.error))
                        return false;
                    for (const auto keyIndex : keyOrder)
                        if (!emitComponentKeyframe(
                                state, names[componentIndex],
                                color->components[componentIndex].keyframes[keyIndex]))
                            return false;
                }
            } else {
                std::span<const std::size_t> keyOrder;
                if (!makeOrder(
                        color->keyframes,
                        [](const Color4Keyframe& key) noexcept { return key.time; },
                        state.sort.window2, keyOrder, state.walk.error))
                    return false;
                for (const auto keyIndex : keyOrder)
                    if (!emitColor4Keyframe(state, color->keyframes[keyIndex]))
                        return false;
            }
        } else {
            {
                state.walk.fail(CanonicalDocumentError::InvalidAnimationCurve, compositionIndex);
                return false;
            }
        }
        if (!state.ok(writer.endArray())) {
            return false;
        }
        if (!emitRetainedTrailing(state)) {
            return false;
        }
        if (!state.ok(writer.endObject())) {
            return false;
        }
    }
    return state.ok(writer.endArray());
}

[[nodiscard]] bool emitParameters(EmitState& state, const Composition& composition,
                                  const std::size_t compositionIndex) noexcept {
    using namespace bloom::document;
    auto& writer = state.writer;
    if (!state.ok(writer.memberName("parameters")) || !state.ok(writer.beginArray())) {
        return false;
    }
    const auto& records = composition.parameters().records();
    std::span<const std::size_t> order;
    if (!makeOrder(
            records, [](const ParameterRecord& record) noexcept { return record.id.value(); },
            state.sort.window1, order, state.walk.error)) {
        {
            state.walk.fail(CanonicalDocumentError::InvalidCollectionIdentity, compositionIndex);
            return false;
        }
    }
    for (const auto index : order) {
        if (!emitParameter(state, records[index], index)) {
            return false;
        }
    }
    return state.ok(writer.endArray());
}

[[nodiscard]] bool emitGraph(EmitState& state, const Composition& composition,
                             const std::size_t compositionIndex) noexcept {
    using namespace bloom::document;
    auto& writer = state.writer;
    const auto& graph = composition.graph();
    if (!state.ok(writer.memberName("graph")) || !state.ok(writer.beginObject())) {
        return false;
    }
    const PathScope graphScope(state, "graph");

    if (!state.ok(writer.memberName("nodes")) || !state.ok(writer.beginArray())) {
        return false;
    }
    std::span<const std::size_t> nodeOrder;
    if (!makeOrder(
            graph.nodes(), [](const NodeRecord& node) noexcept { return node.id.value(); },
            state.sort.window1, nodeOrder, state.walk.error)) {
        {
            state.walk.fail(CanonicalDocumentError::InvalidCollectionIdentity, compositionIndex);
            return false;
        }
    }
    for (const auto nodeIndex : nodeOrder) {
        const auto& node = graph.nodes()[nodeIndex];
        const auto nodeIdText = bloom::project::formatCanonicalUInt64(node.id.value());
        const PathScope nodeScope(state, RoundTripCollectionKind::Node, nodeIdText.view());
        if (!state.ok(writer.beginObject())) {
            return false;
        }
        if (!emitNamedId(state, "id", node.id.value())) {
            return false;
        }
        if (!state.ok(writer.memberName("typeId")) || !state.ok(writer.stringValue(node.typeId))) {
            return false;
        }
        if (!state.ok(writer.memberName("schemaVersion")) ||
            !state.ok(writer.integerValue(node.schemaVersion))) {
            return false;
        }
        if (!state.ok(writer.memberName("parameters")) || !state.ok(writer.beginArray())) {
            return false;
        }
        std::span<const std::size_t> bindingOrder;
        if (!makeOrder(
                node.parameters,
                [](const ParameterBinding& binding) noexcept
                    -> std::pair<std::string_view, std::uint64_t> {
                    return {binding.role, binding.parameterId.value()};
                },
                state.sort.window2, bindingOrder, state.walk.error)) {
            {
                state.walk.fail(CanonicalDocumentError::InvalidCollectionIdentity,
                                compositionIndex);
                return false;
            }
        }
        for (const auto bindingIndex : bindingOrder) {
            const auto& binding = node.parameters[bindingIndex];
            const PathScope bindingScope(state, RoundTripCollectionKind::ParameterBinding,
                                         std::string_view(binding.role));
            if (!state.ok(writer.beginObject())) {
                return false;
            }
            if (!state.ok(writer.memberName("role")) ||
                !state.ok(writer.stringValue(binding.role))) {
                return false;
            }
            if (!emitNamedId(state, "parameterId", binding.parameterId.value())) {
                return false;
            }
            if (!emitRetainedTrailing(state)) {
                return false;
            }
            if (!state.ok(writer.endObject())) {
                return false;
            }
        }
        if (!state.ok(writer.endArray())) {
            return false;
        }
        if (!emitRetainedTrailing(state)) {
            return false;
        }
        if (!state.ok(writer.endObject())) {
            return false;
        }
    }
    if (!state.ok(writer.endArray())) {
        return false;
    }

    if (!state.ok(writer.memberName("edges")) || !state.ok(writer.beginArray())) {
        return false;
    }
    std::span<const std::size_t> edgeOrder;
    if (!makeOrder(
            graph.edges(), [](const EdgeRecord& edge) noexcept { return edge.id.value(); },
            state.sort.window1, edgeOrder, state.walk.error)) {
        {
            state.walk.fail(CanonicalDocumentError::InvalidCollectionIdentity, compositionIndex);
            return false;
        }
    }
    for (const auto edgeIndex : edgeOrder) {
        const auto& edge = graph.edges()[edgeIndex];
        const auto edgeIdText = bloom::project::formatCanonicalUInt64(edge.id.value());
        const PathScope edgeScope(state, RoundTripCollectionKind::Edge, edgeIdText.view());
        if (!state.ok(writer.beginObject())) {
            return false;
        }
        if (!emitNamedId(state, "id", edge.id.value())) {
            return false;
        }
        if (!state.ok(writer.memberName("source"))) {
            return false;
        }
        {
            const PathScope sourceScope(state, "source");
            if (!emitOutputPortRef(state, edge.source)) {
                return false;
            }
        }
        if (!state.ok(writer.memberName("destination")) || !state.ok(writer.beginObject())) {
            return false;
        }
        {
            const PathScope destinationScope(state, "destination");
            if (const auto* nodeInput = std::get_if<NodeInputRef>(&edge.destination)) {
                if (!state.ok(writer.memberName("kind")) ||
                    !state.ok(writer.stringValue("node-input"))) {
                    return false;
                }
                if (!emitNamedId(state, "nodeId", nodeInput->nodeId.value())) {
                    return false;
                }
                if (!state.ok(writer.memberName("port")) ||
                    !state.ok(writer.stringValue(nodeInput->port))) {
                    return false;
                }
            } else if (const auto* stackInput =
                           std::get_if<LayerStackInputRef>(&edge.destination)) {
                if (!state.ok(writer.memberName("kind")) ||
                    !state.ok(writer.stringValue("layer-stack-input"))) {
                    return false;
                }
                if (!emitNamedId(state, "stackNodeId", stackInput->stackNodeId.value())) {
                    return false;
                }
                if (!emitNamedId(state, "slotId", stackInput->slotId.value())) {
                    return false;
                }
                if (!state.ok(writer.memberName("role")) ||
                    !state.ok(writer.stringValue(stackInput->role))) {
                    return false;
                }
            } else {
                state.walk.fail(CanonicalDocumentError::InvalidGraph, compositionIndex);
                return false;
            }
            if (!emitRetainedTrailing(state)) {
                return false;
            }
        }
        if (!state.ok(writer.endObject())) {
            return false;
        }
        if (!emitRetainedTrailing(state)) {
            return false;
        }
        if (!state.ok(writer.endObject())) {
            return false;
        }
    }
    if (!state.ok(writer.endArray())) {
        return false;
    }

    if (!state.ok(writer.memberName("layerOutputs")) || !state.ok(writer.beginArray())) {
        return false;
    }
    std::span<const std::size_t> boundaryOrder;
    if (!makeOrder(
            graph.layerOutputs(),
            [](const LayerOutputBoundary& boundary) noexcept
                -> std::pair<std::uint64_t, std::uint64_t> {
                return {boundary.layerId.value(), boundary.nodeId.value()};
            },
            state.sort.window1, boundaryOrder, state.walk.error)) {
        {
            state.walk.fail(CanonicalDocumentError::InvalidCollectionIdentity, compositionIndex);
            return false;
        }
    }
    for (const auto boundaryIndex : boundaryOrder) {
        const auto& boundary = graph.layerOutputs()[boundaryIndex];
        const auto layerIdText = bloom::project::formatCanonicalUInt64(boundary.layerId.value());
        const PathScope layerOutputScope(state, RoundTripCollectionKind::LayerOutput,
                                         layerIdText.view());
        if (!state.ok(writer.beginObject())) {
            return false;
        }
        if (!emitNamedId(state, "nodeId", boundary.nodeId.value())) {
            return false;
        }
        if (!emitNamedId(state, "layerId", boundary.layerId.value())) {
            return false;
        }
        if (!state.ok(writer.memberName("name")) || !state.ok(writer.stringValue(boundary.name))) {
            return false;
        }
        if (!state.ok(writer.memberName("outputPort")) ||
            !state.ok(writer.stringValue(boundary.outputPort))) {
            return false;
        }
        for (const auto& [key, time] :
             {std::pair{"inPoint", boundary.inPoint}, std::pair{"outPoint", boundary.outPoint}}) {
            if (time == bloom::core::RationalTime{})
                continue;
            const PathScope timeScope(state, key);
            if (!state.ok(writer.memberName(key)) ||
                !emitRational(state, time.numerator(), time.denominator()))
                return false;
        }
        if (!boundary.enabled &&
            (!state.ok(writer.memberName("enabled")) || !state.ok(writer.booleanValue(false))))
            return false;
        if (boundary.solo &&
            (!state.ok(writer.memberName("solo")) || !state.ok(writer.booleanValue(true))))
            return false;
        if (boundary.locked &&
            (!state.ok(writer.memberName("locked")) || !state.ok(writer.booleanValue(true))))
            return false;
        if (boundary.labelColor) {
            if (!state.ok(writer.memberName("labelColor")) || !state.ok(writer.beginArray()))
                return false;
            for (const auto channel : *boundary.labelColor)
                if (!state.ok(writer.integerValue(channel)))
                    return false;
            if (!state.ok(writer.endArray()))
                return false;
        }
        if (boundary.parent && !emitNamedId(state, "parent", boundary.parent->value()))
            return false;
        if (!emitRetainedTrailing(state)) {
            return false;
        }
        if (!state.ok(writer.endObject())) {
            return false;
        }
    }
    if (!state.ok(writer.endArray())) {
        return false;
    }

    const auto emitStack = [&](const bloom::document::LayerStack& stack) {
        if (!state.ok(writer.beginObject()))
            return false;
        if (!emitNamedId(state, "nodeId", stack.nodeId().value())) {
            return false;
        }
        if (!state.ok(writer.memberName("entries")) || !state.ok(writer.beginArray())) {
            return false;
        }
        for (const auto& entry : stack.entries()) {
            const auto slotIdText = bloom::project::formatCanonicalUInt64(entry.slotId.value());
            const PathScope entryScope(state, RoundTripCollectionKind::LayerStackEntry,
                                       slotIdText.view());
            if (!state.ok(writer.beginObject())) {
                return false;
            }
            if (!emitNamedId(state, "slotId", entry.slotId.value())) {
                return false;
            }
            if (entry.layerId.isValid()
                    ? !emitNamedId(state, "layerId", entry.layerId.value())
                    : (!state.ok(writer.memberName("layerId")) || !state.ok(writer.nullValue()))) {
                return false;
            }
            if (!emitRetainedTrailing(state)) {
                return false;
            }
            if (!state.ok(writer.endObject())) {
                return false;
            }
        }
        if (!state.ok(writer.endArray())) {
            return false;
        }
        if (!stack.enabled() &&
            (!state.ok(writer.memberName("enabled")) || !state.ok(writer.booleanValue(false))))
            return false;
        if (!emitRetainedTrailing(state)) {
            return false;
        }
        return state.ok(writer.endObject());
    };
    if (!state.ok(writer.memberName("layerStack")))
        return false;
    {
        const PathScope scope(state, "layerStack");
        if (graph.merges().empty()) {
            if (!state.ok(writer.nullValue()))
                return false;
        } else if (!emitStack(graph.merges().front()))
            return false;
    }
    if (!graph.compositionOutput().has_value()) {
        {
            state.walk.fail(CanonicalDocumentError::InvalidGraph, compositionIndex);
            return false;
        }
    }
    if (!state.ok(writer.memberName("compositionOutput"))) {
        return false;
    }
    {
        const PathScope compositionOutputScope(state, "compositionOutput");
        if (!emitOutputPortRef(state, *graph.compositionOutput())) {
            return false;
        }
    }
    if (graph.merges().size() > 1) {
        if (!state.ok(writer.memberName("merges")) || !state.ok(writer.beginArray()))
            return false;
        const PathScope scope(state, "merges");
        for (std::size_t i = 1; i < graph.merges().size(); ++i) {
            const PathScope mergeScope(state, RoundTripCollectionKind::Node,
                                       std::to_string(graph.merges()[i].nodeId().value()));
            if (!emitStack(graph.merges()[i]))
                return false;
        }
        if (!state.ok(writer.endArray()))
            return false;
    }
    if (!emitRetainedTrailing(state)) {
        return false;
    }
    return state.ok(writer.endObject());
}

[[nodiscard]] bool emitNodeLayout(EmitState& state, const Composition& composition) noexcept {
    auto& writer = state.writer;
    if (!state.ok(writer.memberName("nodeLayout")) || !state.ok(writer.beginArray()))
        return false;
    const PathScope layoutScope(state, "nodeLayout");
    for (const auto& [id, record] : composition.nodeLayout()) {
        const auto idText = bloom::project::formatCanonicalUInt64(id.value());
        const PathScope recordScope(state, RoundTripCollectionKind::NodeLayout, idText.view());
        if (!state.ok(writer.beginObject()) || !emitNamedId(state, "nodeId", id.value()) ||
            !state.ok(writer.memberName("position")) || !state.ok(writer.beginObject()))
            return false;
        {
            const PathScope positionScope(state, "position");
            if (!state.ok(writer.memberName("x")) ||
                !state.ok(writer.float64Value(record.position.x)) ||
                !state.ok(writer.memberName("y")) ||
                !state.ok(writer.float64Value(record.position.y)) || !emitRetainedTrailing(state))
                return false;
        }
        if (!state.ok(writer.endObject()) || !state.ok(writer.memberName("width")) ||
            !state.ok(writer.float64Value(record.width)) ||
            !state.ok(writer.memberName("collapsed")) ||
            !state.ok(writer.booleanValue(record.collapsed)) ||
            !state.ok(writer.memberName("muted")) || !state.ok(writer.booleanValue(record.muted)) ||
            !emitRetainedTrailing(state) || !state.ok(writer.endObject()))
            return false;
    }
    return state.ok(writer.endArray());
}

// The group frames, ascending by NodeGroupId because NodeGroups is keyed by it, each with its
// members ascending by NodeId for the same reason.
[[nodiscard]] bool emitNodeGroups(EmitState& state, const Composition& composition) noexcept {
    auto& writer = state.writer;
    if (!state.ok(writer.memberName("nodeGroups")) || !state.ok(writer.beginArray()))
        return false;
    const PathScope groupsScope(state, "nodeGroups");
    for (const auto& [id, record] : composition.nodeGroups()) {
        const auto idText = bloom::project::formatCanonicalUInt64(id.value());
        const PathScope recordScope(state, RoundTripCollectionKind::NodeGroup, idText.view());
        if (!state.ok(writer.beginObject()) || !emitNamedId(state, "groupId", id.value()) ||
            !state.ok(writer.memberName("name")) || !state.ok(writer.stringValue(record.name)) ||
            !state.ok(writer.memberName("members")) || !state.ok(writer.beginArray()))
            return false;
        for (const auto member : record.members) {
            const auto memberText = bloom::project::formatCanonicalUInt64(member.value());
            if (!state.ok(writer.stringValue(memberText.view())))
                return false;
        }
        if (!state.ok(writer.endArray()) || !state.ok(writer.memberName("padding")) ||
            !state.ok(writer.beginObject()))
            return false;
        {
            const PathScope paddingScope(state, "padding");
            if (!state.ok(writer.memberName("x")) ||
                !state.ok(writer.float64Value(record.padding.x)) ||
                !state.ok(writer.memberName("y")) ||
                !state.ok(writer.float64Value(record.padding.y)) || !emitRetainedTrailing(state))
                return false;
        }
        if (!state.ok(writer.endObject()) || !emitRetainedTrailing(state) ||
            !state.ok(writer.endObject()))
            return false;
    }
    return state.ok(writer.endArray());
}

[[nodiscard]] bool emitSafeAreas(EmitState& state, const Composition& composition) noexcept {
    auto& writer = state.writer;
    const auto settings = composition.safeAreas();
    const PathScope scope(state, "safeAreas");
    return state.ok(writer.memberName("safeAreas")) && state.ok(writer.beginObject()) &&
           state.ok(writer.memberName("action")) &&
           state.ok(writer.float64Value(settings.action)) && state.ok(writer.memberName("title")) &&
           state.ok(writer.float64Value(settings.title)) && emitRetainedTrailing(state) &&
           state.ok(writer.endObject());
}

[[nodiscard]] bool emitComposition(EmitState& state, const Composition& composition,
                                   const std::size_t compositionIndex) noexcept {
    auto& writer = state.writer;
    state.walk.compositionIndex = compositionIndex;
    state.walk.elementIndex = kCanonicalDocumentNoIndex;
    const auto compositionIdText = bloom::project::formatCanonicalUInt64(composition.id().value());
    const PathScope compositionScope(state, RoundTripCollectionKind::Composition,
                                     compositionIdText.view());
    if (!state.ok(writer.beginObject())) {
        return false;
    }
    if (!emitNamedId(state, "id", composition.id().value())) {
        return false;
    }
    if (!state.ok(writer.memberName("name")) || !state.ok(writer.stringValue(composition.name()))) {
        return false;
    }
    if (!state.ok(writer.memberName("duration"))) {
        return false;
    }
    {
        const PathScope durationScope(state, "duration");
        if (!emitRational(state, composition.duration().numerator(),
                          composition.duration().denominator())) {
            return false;
        }
    }
    if (!state.ok(writer.memberName("format")) || !state.ok(writer.beginObject())) {
        return false;
    }
    {
        const PathScope formatScope(state, "format");
        const auto format = composition.format();
        if (!state.ok(writer.memberName("width")) ||
            !state.ok(writer.integerValue(format.width()))) {
            return false;
        }
        if (!state.ok(writer.memberName("height")) ||
            !state.ok(writer.integerValue(format.height()))) {
            return false;
        }
        if (!state.ok(writer.memberName("pixelAspect"))) {
            return false;
        }
        {
            const PathScope pixelAspectScope(state, "pixelAspect");
            if (!emitRational(state, format.pixelAspect().numerator(),
                              format.pixelAspect().denominator())) {
                return false;
            }
        }
        if (!state.ok(writer.memberName("frameRate"))) {
            return false;
        }
        {
            const PathScope frameRateScope(state, "frameRate");
            if (!emitRational(state, format.frameRate().numerator(),
                              format.frameRate().denominator())) {
                return false;
            }
        }
        if (!emitRetainedTrailing(state)) {
            return false;
        }
    }
    if (!state.ok(writer.endObject())) {
        return false;
    }
    if (!emitParameters(state, composition, compositionIndex)) {
        return false;
    }
    if (!emitAnimationCurves(state, composition, compositionIndex)) {
        return false;
    }
    if (!emitGraph(state, composition, compositionIndex) || !emitNodeLayout(state, composition) ||
        !emitNodeGroups(state, composition)) {
        return false;
    }
    // The default safe-area pair is part of the 1.8 contract's implicit composition defaults. It
    // is omitted from canonical output until a composition carries a non-default value, keeping
    // newly written documents compact while still making every authored per-composition setting
    // durable. Older documents decode the same implicit pair through Composition's default.
    if (composition.safeAreas() != bloom::document::SafeAreaSettings{} &&
        !emitSafeAreas(state, composition)) {
        return false;
    }
    if (const auto area = composition.workArea()) {
        const PathScope scope(state, "workArea");
        if (!state.ok(writer.memberName("workArea")) || !state.ok(writer.beginObject()))
            return false;
        for (const auto& [key, time] :
             {std::pair{"start", area->start}, std::pair{"end", area->end}}) {
            const PathScope timeScope(state, key);
            if (!state.ok(writer.memberName(key)) ||
                !emitRational(state, time.numerator(), time.denominator()))
                return false;
        }
        if (!emitRetainedTrailing(state) || !state.ok(writer.endObject()))
            return false;
    }
    if (!state.ok(writer.memberName("backgroundColor")) || !state.ok(writer.beginArray()))
        return false;
    const auto background = composition.backgroundColor();
    for (const double channel :
         {background.red, background.green, background.blue, background.alpha})
        if (!state.ok(writer.float64Value(channel)))
            return false;
    if (!state.ok(writer.endArray()))
        return false;
    if (!emitRetainedTrailing(state)) {
        return false;
    }
    return state.ok(writer.endObject());
}

// Only call site (colorSettings.ocioConfig.locator) is a genuine attachment point; the caller
// pushes the "locator" PathScope first.
[[nodiscard]] bool emitLocator(EmitState& state,
                               const bloom::document::OcioConfigLocator& locator) noexcept {
    using namespace bloom::document;
    auto& writer = state.writer;
    if (!state.ok(writer.beginObject())) {
        return false;
    }
    bool emitted = false;
    if (const auto* builtIn = std::get_if<BuiltInOcioConfigLocator>(&locator)) {
        emitted = state.ok(writer.memberName("kind")) && state.ok(writer.stringValue("builtin")) &&
                  state.ok(writer.memberName("uri")) && state.ok(writer.stringValue(builtIn->uri));
    } else if (const auto* projectRelative = std::get_if<ProjectRelativeOciozLocator>(&locator)) {
        emitted = state.ok(writer.memberName("kind")) &&
                  state.ok(writer.stringValue("project-relative-ocioz")) &&
                  state.ok(writer.memberName("path")) &&
                  state.ok(writer.stringValue(projectRelative->path));
    } else if (const auto* externalOcioz = std::get_if<ExternalOciozLocator>(&locator)) {
        emitted =
            state.ok(writer.memberName("kind")) && state.ok(writer.stringValue("external-ocioz")) &&
            state.ok(writer.memberName("uri")) && state.ok(writer.stringValue(externalOcioz->uri));
    } else if (const auto* externalConfig = std::get_if<ExternalOcioConfigLocator>(&locator)) {
        emitted = state.ok(writer.memberName("kind")) &&
                  state.ok(writer.stringValue("external-config")) &&
                  state.ok(writer.memberName("uri")) &&
                  state.ok(writer.stringValue(externalConfig->uri));
    }
    if (!emitted) {
        return false;
    }
    if (!emitRetainedTrailing(state)) {
        return false;
    }
    return state.ok(writer.endObject());
}

[[nodiscard]] bool emitColorSettings(EmitState& state) noexcept {
    using namespace bloom::document;
    auto& writer = state.writer;
    const auto& settings = state.colorSettings;
    if (!state.ok(writer.beginObject())) {
        return false;
    }
    if (!state.ok(writer.memberName("schemaVersion"))) {
        return false;
    }
    {
        const PathScope schemaVersionScope(state, "schemaVersion");
        if (!emitVersion(state, settings.schemaVersion, true)) {
            return false;
        }
    }
    if (!state.ok(writer.memberName("processColorSpaceId")) ||
        !state.ok(writer.stringValue(settings.processColorSpaceId))) {
        return false;
    }
    if (!state.ok(writer.memberName("ocioConfig")) || !state.ok(writer.beginObject())) {
        return false;
    }
    {
        const PathScope ocioConfigScope(state, "ocioConfig");
        if (!state.ok(writer.memberName("schemaVersion"))) {
            return false;
        }
        {
            const PathScope ocioSchemaVersionScope(state, "schemaVersion");
            if (!emitVersion(state, settings.ocioConfig.schemaVersion, true)) {
                return false;
            }
        }
        if (!state.ok(writer.memberName("locator"))) {
            return false;
        }
        {
            const PathScope locatorScope(state, "locator");
            if (!emitLocator(state, settings.ocioConfig.locator)) {
                return false;
            }
        }
        if (!state.ok(writer.memberName("expectedRevision")) || !state.ok(writer.beginObject())) {
            return false;
        }
        {
            const PathScope expectedRevisionScope(state, "expectedRevision");
            if (!state.ok(writer.memberName("algorithm")) ||
                !state.ok(writer.stringValue("sha256"))) {
                return false;
            }
            const auto hex = settings.ocioConfig.expectedRevision.digest.toLowercaseHex();
            if (!state.ok(writer.memberName("digest")) ||
                !state.ok(writer.stringValue(std::string_view(hex.data(), hex.size())))) {
                return false;
            }
            if (!emitRetainedTrailing(state)) {
                return false;
            }
        }
        if (!state.ok(writer.endObject())) {
            return false;
        }
        std::string_view portability;
        switch (settings.ocioConfig.portability) {
        case OcioConfigPortability::BuiltIn:
            portability = "builtin";
            break;
        case OcioConfigPortability::ProjectRelative:
            portability = "project-relative";
            break;
        case OcioConfigPortability::External:
            portability = "external";
            break;
        default: {
            state.walk.fail(CanonicalDocumentError::OcioPortabilityMismatch);
            return false;
        }
        }
        if (!state.ok(writer.memberName("portability")) ||
            !state.ok(writer.stringValue(portability))) {
            return false;
        }
        if (!state.ok(writer.memberName("contextVariables")) || !state.ok(writer.beginArray())) {
            return false;
        }
        for (const auto& variable : settings.ocioConfig.contextVariables) {
            if (!state.ok(writer.beginObject())) {
                return false;
            }
            if (!state.ok(writer.memberName("name")) ||
                !state.ok(writer.stringValue(variable.name))) {
                return false;
            }
            if (!state.ok(writer.memberName("value")) ||
                !state.ok(writer.stringValue(variable.value))) {
                return false;
            }
            if (!state.ok(writer.endObject())) {
                return false;
            }
        }
        if (!state.ok(writer.endArray())) {
            return false;
        }
        if (!emitRetainedTrailing(state)) {
            return false;
        }
    }
    if (!state.ok(writer.endObject())) {
        return false;
    }
    if (!emitRetainedTrailing(state)) {
        return false;
    }
    return state.ok(writer.endObject());
}

[[nodiscard]] bool emitPayload(EmitState& state,
                               const bloom::document::OpaqueExtensionPayload& payload) noexcept {
    const auto encodedSize = bloom::project::canonicalBase64EncodedSize(payload.size());
    if (!encodedSize.hasValue() || *encodedSize.value() > state.payloadScratch.size()) {
        {
            state.walk.fail(CanonicalDocumentError::PayloadBufferTooSmall);
            return false;
        }
    }
    const auto encoded = bloom::project::encodeCanonicalBase64(
        payload.bytes(), state.payloadScratch.first(*encodedSize.value()));
    if (!encoded) {
        {
            state.walk.fail(CanonicalDocumentError::PayloadBufferTooSmall);
            return false;
        }
    }
    return state.ok(state.writer.stringValue(
        std::string_view(state.payloadScratch.data(), *encodedSize.value())));
}

[[nodiscard]] bool emitExtensionRecord(EmitState& state, const ExtensionRecord& record) noexcept {
    using namespace bloom::document;
    auto& writer = state.writer;
    const auto idText = bloom::project::formatCanonicalUInt64(record.id.value());
    const PathScope recordScope(state, RoundTripCollectionKind::ExtensionRecord, idText.view());
    if (!state.ok(writer.beginObject())) {
        return false;
    }
    if (!emitNamedId(state, "id", record.id.value())) {
        return false;
    }
    if (!state.ok(writer.memberName("ownerId")) || !state.ok(writer.stringValue(record.ownerId))) {
        return false;
    }
    if (!state.ok(writer.memberName("typeId")) || !state.ok(writer.stringValue(record.typeId))) {
        return false;
    }
    if (!state.ok(writer.memberName("schemaVersion"))) {
        return false;
    }
    {
        const PathScope schemaVersionScope(state, "schemaVersion");
        if (!emitVersion(state, record.schemaVersion, true)) {
            return false;
        }
    }
    if (!state.ok(writer.memberName("subject"))) {
        return false;
    }
    if (record.subject.has_value()) {
        const PathScope subjectScope(state, "subject");
        if (!emitTypedTarget(state, *record.subject)) {
            return false;
        }
    } else if (!state.ok(writer.nullValue())) {
        return false;
    }
    if (!state.ok(writer.memberName("mediaType")) ||
        !state.ok(writer.stringValue(record.mediaType))) {
        return false;
    }
    if (!state.ok(writer.memberName("referencePolicy")) || !state.ok(writer.beginObject())) {
        return false;
    }
    {
        const PathScope referencePolicyScope(state, "referencePolicy");
        if (std::holds_alternative<NoExtensionReferences>(record.referencePolicy)) {
            if (!state.ok(writer.memberName("kind")) || !state.ok(writer.stringValue("none"))) {
                return false;
            }
        } else if (const auto* table =
                       std::get_if<ExtensionHostReferenceTable>(&record.referencePolicy)) {
            if (!state.ok(writer.memberName("kind")) ||
                !state.ok(writer.stringValue("host-table"))) {
                return false;
            }
            if (!state.ok(writer.memberName("references")) || !state.ok(writer.beginArray())) {
                return false;
            }
            for (const auto& reference : table->references) {
                const PathScope hostReferenceScope(state, RoundTripCollectionKind::HostReference,
                                                   std::string_view(reference.key));
                if (!state.ok(writer.beginObject())) {
                    return false;
                }
                if (!state.ok(writer.memberName("key")) ||
                    !state.ok(writer.stringValue(reference.key))) {
                    return false;
                }
                if (!state.ok(writer.memberName("target"))) {
                    return false;
                }
                {
                    const PathScope targetScope(state, "target");
                    if (!emitTypedTarget(state, reference.target)) {
                        return false;
                    }
                }
                if (!emitRetainedTrailing(state)) {
                    return false;
                }
                if (!state.ok(writer.endObject())) {
                    return false;
                }
            }
            if (!state.ok(writer.endArray())) {
                return false;
            }
        } else if (const auto* remapper =
                       std::get_if<ExtensionOwnerRemapper>(&record.referencePolicy)) {
            if (!state.ok(writer.memberName("kind")) ||
                !state.ok(writer.stringValue("owner-remapper"))) {
                return false;
            }
            if (!state.ok(writer.memberName("remapperId")) ||
                !state.ok(writer.stringValue(remapper->remapperId))) {
                return false;
            }
            if (!state.ok(writer.memberName("version"))) {
                return false;
            }
            {
                const PathScope versionScope(state, "version");
                if (!emitVersion(state, remapper->version, true)) {
                    return false;
                }
            }
        } else {
            state.walk.fail(CanonicalDocumentError::InvalidExtensionRecord);
            return false;
        }
        if (!emitRetainedTrailing(state)) {
            return false;
        }
    }
    if (!state.ok(writer.endObject())) {
        return false;
    }
    if (!state.ok(writer.memberName("payload"))) {
        return false;
    }
    if (!emitPayload(state, record.payload)) {
        return false;
    }
    if (!emitRetainedTrailing(state)) {
        return false;
    }
    return state.ok(writer.endObject());
}

[[nodiscard]] bool emitHighWaterMember(EmitState& state, const std::string_view name,
                                       const std::uint64_t value) noexcept {
    return emitNamedId(state, name, value);
}

#include "canonical_assets.ipp"

[[nodiscard]] bool emitDocumentRoot(EmitState& state) noexcept {
    using namespace bloom::document;
    auto& writer = state.writer;
    if (!state.ok(writer.beginObject())) {
        return false;
    }
    if (!state.ok(writer.memberName("schemaVersion"))) {
        return false;
    }
    // No PathScope pushed: the root schemaVersion object is never itself an attachment point (see
    // emitVersion's own comment). {1, schemaMinor} lets an overlay rewrite of a {1, minor > 0}
    // document reproduce the exact minor it was opened with; a plain write leaves schemaMinor at
    // its default 0.
    if (!emitVersion(
            state,
            SchemaVersion{kV1SchemaVersion.major,
                          std::max(state.schemaMinor,
                                   bloom::project::kCanonicalDocumentSchemaVersionV1.minor)})) {
        return false;
    }

    if (!state.ok(writer.memberName("project")) || !state.ok(writer.beginObject())) {
        return false;
    }
    {
        const PathScope projectScope(state, "project");
        if (!emitNamedId(state, "id", state.project.id().value())) {
            return false;
        }
        if (!state.ok(writer.memberName("name")) ||
            !state.ok(writer.stringValue(state.project.name()))) {
            return false;
        }
        if (!state.ok(writer.memberName("colorSettings"))) {
            return false;
        }
        {
            const PathScope colorSettingsScope(state, "colorSettings");
            if (!emitColorSettings(state)) {
                return false;
            }
        }
        if (!state.ok(writer.memberName("compositions")) || !state.ok(writer.beginArray())) {
            return false;
        }
        std::span<const std::size_t> compositionOrder;
        if (!makeOrder(
                state.project.compositions(),
                [](const Composition& composition) noexcept { return composition.id().value(); },
                state.sort.window0, compositionOrder, state.walk.error)) {
            {
                state.walk.fail(CanonicalDocumentError::InvalidCollectionIdentity);
                return false;
            }
        }
        for (const auto compositionIndex : compositionOrder) {
            if (!emitComposition(state, state.project.compositions()[compositionIndex],
                                 compositionIndex)) {
                return false;
            }
        }
        if (!state.ok(writer.endArray()) || !emitAssets(state)) {
            return false;
        }
        if (!emitRetainedTrailing(state)) {
            return false;
        }
    }
    if (!state.ok(writer.endObject())) {
        return false;
    }

    if (!state.ok(writer.memberName("idAllocation")) || !state.ok(writer.beginObject())) {
        return false;
    }
    {
        const PathScope idAllocationScope(state, "idAllocation");
        if (!state.ok(writer.memberName("highestIssued")) || !state.ok(writer.beginObject())) {
            return false;
        }
        {
            const PathScope highestIssuedScope(state, "highestIssued");
            const auto& water = state.highWater;
            if (!emitHighWaterMember(state, "composition", water.composition) ||
                !emitHighWaterMember(state, "node", water.node) ||
                !emitHighWaterMember(state, "edge", water.edge) ||
                !emitHighWaterMember(state, "layer", water.layer) ||
                !emitHighWaterMember(state, "layerSlot", water.layerSlot) ||
                !emitHighWaterMember(state, "parameter", water.parameter) ||
                !emitHighWaterMember(state, "animationCurve", water.animationCurve) ||
                !emitHighWaterMember(state, "keyframe", water.keyframe) ||
                !emitHighWaterMember(state, "driverBinding", water.driverBinding) ||
                !emitHighWaterMember(state, "extensionRecord", water.extensionRecord) ||
                !emitHighWaterMember(state, "nodeGroup", water.nodeGroup) ||
                !emitHighWaterMember(state, "asset", water.asset)) {
                return false;
            }
            if (!emitRetainedTrailing(state)) {
                return false;
            }
        }
        if (!state.ok(writer.endObject())) {
            return false;
        }
        if (!emitRetainedTrailing(state)) {
            return false;
        }
    }
    if (!state.ok(writer.endObject())) {
        return false;
    }

    if (!state.ok(writer.memberName("extensions")) || !state.ok(writer.beginArray())) {
        return false;
    }
    std::span<const std::size_t> recordOrder;
    if (!makeOrder(
            state.project.extensionRecords(),
            [](const ExtensionRecord& record) noexcept { return record.id.value(); },
            state.sort.window1, recordOrder, state.walk.error)) {
        {
            state.walk.fail(CanonicalDocumentError::InvalidCollectionIdentity);
            return false;
        }
    }
    for (const auto recordIndex : recordOrder) {
        if (!emitExtensionRecord(state, state.project.extensionRecords()[recordIndex])) {
            return false;
        }
    }
    if (!state.ok(writer.endArray())) {
        return false;
    }
    if (!emitRetainedTrailing(state)) {
        return false;
    }
    if (!state.ok(writer.endObject())) {
        return false;
    }
    if (!state.ok(writer.finish())) {
        return false;
    }
    // RT2: every attachment point in an overlay's RoundTripState must be consumed exactly once by
    // this walk (see docs/architecture/project-format.md, "Versions, Migrations, And
    // Preservation"). A leftover entry is state captured against a different document (or a stale
    // edit of this one), never silently dropped.
    if (state.roundTrip != nullptr && state.retainedConsumed != state.roundTrip->entries().size()) {
        state.walk.fail(CanonicalDocumentError::RoundTripStateMismatch);
        return false;
    }
    return true;
}

[[nodiscard]] bloom::document::OcioConfigPortability
locatorPortability(const bloom::document::OcioConfigLocator& locator) noexcept {
    using namespace bloom::document;
    if (locator.valueless_by_exception()) {
        return OcioConfigPortability::Unknown;
    }
    if (std::holds_alternative<BuiltInOcioConfigLocator>(locator)) {
        return OcioConfigPortability::BuiltIn;
    }
    if (std::holds_alternative<ProjectRelativeOciozLocator>(locator)) {
        return OcioConfigPortability::ProjectRelative;
    }
    if (std::holds_alternative<ExternalOciozLocator>(locator) ||
        std::holds_alternative<ExternalOcioConfigLocator>(locator)) {
        return OcioConfigPortability::External;
    }
    return OcioConfigPortability::Unknown;
}

[[nodiscard]] bool isValidPositiveReducedRational(const std::int64_t numerator,
                                                  const std::int64_t denominator) noexcept {
    return numerator >= 1 && denominator >= 1 && std::gcd(numerator, denominator) == 1;
}

[[nodiscard]] WalkState validateRequest(const CanonicalDocumentV1& document,
                                        const CanonicalDocumentLimits limits,
                                        SortPlan& plan) noexcept {
    using namespace bloom::document;
    WalkState walk;

    if (document.snapshot == nullptr || document.colorSettings == nullptr) {
        walk.fail(CanonicalDocumentError::MissingInput);
        return walk;
    }
    if (limits.maximumValues > bloom::project::kCanonicalJsonMaximumValues ||
        limits.maximumContainerEntries > bloom::project::kCanonicalJsonMaximumContainerEntries ||
        limits.maximumOutputBytes > bloom::project::kCanonicalDocumentMaximumBytes) {
        walk.fail(CanonicalDocumentError::InvalidLimits);
        return walk;
    }

    const auto& snapshot = *document.snapshot;
    const auto& project = snapshot.project();
    const auto& settings = *document.colorSettings;

    // Color settings are durable project truth that Project does not own, so this writer performs
    // their complete admission here, delegating lexical rules to the document-owned validator.
    if (settings.processColorSpaceId != kProcessColorSpaceIdV1) {
        walk.fail(CanonicalDocumentError::InvalidProcessColorSpaceId);
        return walk;
    }
    if (settings.schemaVersion != bloom::document::kColorSettingsSchemaVersionV1 ||
        settings.ocioConfig.schemaVersion != kOcioConfigReferenceSchemaVersionV1) {
        walk.fail(CanonicalDocumentError::InvalidColorSettings);
        return walk;
    }
    if (settings.ocioConfig.expectedRevision.algorithm != OcioRevisionAlgorithm::Sha256) {
        walk.fail(CanonicalDocumentError::InvalidOcioRevisionAlgorithm);
        return walk;
    }
    const auto expectedPortability = locatorPortability(settings.ocioConfig.locator);
    if (expectedPortability == OcioConfigPortability::Unknown ||
        settings.ocioConfig.portability != expectedPortability) {
        walk.fail(CanonicalDocumentError::OcioPortabilityMismatch);
        return walk;
    }
    if (!settings.validate().ok()) {
        walk.fail(CanonicalDocumentError::InvalidColorSettings);
        return walk;
    }

    // No parameter-source admission pass any more. Document 1.4 can write every alternative the
    // document layer can hold, and a driver source's own well-formedness -- a valid node id and
    // valid structural port text -- is already refused by ParameterStore on insert, so a second
    // check here could only ever be unreachable.
    for (std::size_t compositionIndex = 0; compositionIndex < project.compositions().size();
         ++compositionIndex) {
        const auto& composition = project.compositions()[compositionIndex];

        // Defensive domain re-checks for values whose owning types cannot always guarantee the
        // serialized domain (for example through setFormat).
        const auto duration = composition.duration();
        if (!isValidPositiveReducedRational(duration.numerator(), duration.denominator())) {
            walk.fail(CanonicalDocumentError::InvalidCompositionDuration, compositionIndex);
            return walk;
        }
        const auto format = composition.format();
        constexpr std::uint32_t kMaximumDimension = 1U << 20U;
        if (format.width() == 0 || format.height() == 0 || format.width() > kMaximumDimension ||
            format.height() > kMaximumDimension ||
            static_cast<std::uint64_t>(format.width()) *
                    static_cast<std::uint64_t>(format.height()) >
                (std::uint64_t{1} << 32U) ||
            format.pixelAspect().numerator() == 0 || format.pixelAspect().denominator() == 0 ||
            format.frameRate().numerator() == 0 || format.frameRate().denominator() == 0) {
            walk.fail(CanonicalDocumentError::InvalidCompositionFormat, compositionIndex);
            return walk;
        }
    }

    plan = measureSortPlan(project);
    if (plan.total() > document.sortScratch.size()) {
        walk.fail(CanonicalDocumentError::SortBufferTooSmall);
        return walk;
    }
    for (const auto& record : project.extensionRecords()) {
        const auto encodedSize = bloom::project::canonicalBase64EncodedSize(record.payload.size());
        if (!encodedSize.hasValue() || *encodedSize.value() > document.payloadScratch.size()) {
            walk.fail(CanonicalDocumentError::PayloadBufferTooSmall);
            return walk;
        }
    }
    return walk;
}

} // namespace

namespace bloom::project {

CanonicalDocumentSizeResult canonicalDocumentSize(const CanonicalDocumentV1& document,
                                                  const CanonicalDocumentLimits limits) noexcept {
    SortPlan plan;
    const auto validation = validateRequest(document, limits, plan);
    if (validation.error != CanonicalDocumentError::None) {
        return CanonicalDocumentSizeResult::failure(validation.error, validation.compositionIndex,
                                                    validation.elementIndex);
    }

    const auto& snapshot = *document.snapshot;
    auto counter =
        CanonicalJsonWriter::counting({.maximumDepth = kCanonicalDocumentMaximumDepth,
                                       .maximumValues = limits.maximumValues,
                                       .maximumContainerEntries = limits.maximumContainerEntries});
    EmitState state{counter,
                    snapshot.project(),
                    snapshot.ids().highWater(),
                    *document.colorSettings,
                    splitSortScratch(document.sortScratch, plan),
                    document.payloadScratch,
                    {},
                    document.roundTrip,
                    document.schemaMinor};
    if (!emitDocumentRoot(state)) {
        return CanonicalDocumentSizeResult::failure(state.walk.error, state.walk.compositionIndex,
                                                    state.walk.elementIndex);
    }
    const auto requiredSize = counter.bytesRequired();
    if (requiredSize > limits.maximumOutputBytes) {
        return CanonicalDocumentSizeResult::failure(CanonicalDocumentError::DocumentSizeExceeded);
    }
    return CanonicalDocumentSizeResult::success(requiredSize);
}

CanonicalDocumentWriteResult
encodeCanonicalDocument(const CanonicalDocumentV1& document, const std::span<char> output,
                        const CanonicalDocumentLimits limits) noexcept {
    const auto sizeResult = canonicalDocumentSize(document, limits);
    if (!sizeResult) {
        return CanonicalDocumentWriteResult::failure(sizeResult.error(), std::nullopt,
                                                     sizeResult.compositionIndex(),
                                                     sizeResult.elementIndex());
    }
    const auto requiredSize = *sizeResult.value();
    if (output.size() < requiredSize) {
        return CanonicalDocumentWriteResult::failure(CanonicalDocumentError::OutputCapacityExceeded,
                                                     requiredSize);
    }

    const auto& snapshot = *document.snapshot;
    CanonicalJsonWriter writer(output.first(requiredSize),
                               {.maximumDepth = kCanonicalDocumentMaximumDepth,
                                .maximumValues = limits.maximumValues,
                                .maximumContainerEntries = limits.maximumContainerEntries});
    EmitState state{writer,
                    snapshot.project(),
                    snapshot.ids().highWater(),
                    *document.colorSettings,
                    splitSortScratch(document.sortScratch, measureSortPlan(snapshot.project())),
                    document.payloadScratch,
                    {},
                    document.roundTrip,
                    document.schemaMinor};
    if (!emitDocumentRoot(state) || writer.bytesWritten() != requiredSize) {
        std::terminate();
    }
    return CanonicalDocumentWriteResult::success(requiredSize);
}

} // namespace bloom::project
