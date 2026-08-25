#include <bloom/project/canonical_document.hpp>

#include <algorithm>
#include <bloom/core/utf8.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/persisted_text.hpp>
#include <bloom/project/canonical_base64.hpp>
#include <bloom/project/canonical_decimal.hpp>

#include <cstddef>
#include <cstdint>
#include <numeric>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

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

[[nodiscard]] bool emitVersion(EmitState& state,
                               const bloom::document::SchemaVersion version) noexcept {
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
    return state.ok(state.writer.endObject());
}

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
               state.ok(writer.endObject());
    }
    if (const auto* integer = std::get_if<std::int64_t>(&value)) {
        return state.ok(writer.memberName("kind")) && state.ok(writer.stringValue("int64")) &&
               emitNamedSigned(state, "value", *integer) && state.ok(writer.endObject());
    }
    if (const auto* floating = std::get_if<double>(&value)) {
        return state.ok(writer.memberName("kind")) && state.ok(writer.stringValue("float64")) &&
               state.ok(writer.memberName("value")) && state.ok(writer.float64Value(*floating)) &&
               state.ok(writer.endObject());
    }
    if (const auto* vector = std::get_if<Vec2d>(&value)) {
        return state.ok(writer.memberName("kind")) && state.ok(writer.stringValue("vec2")) &&
               state.ok(writer.memberName("x")) && state.ok(writer.float64Value(vector->x)) &&
               state.ok(writer.memberName("y")) && state.ok(writer.float64Value(vector->y)) &&
               state.ok(writer.endObject());
    }
    if (const auto* color = std::get_if<bloom::core::Color4d>(&value)) {
        return state.ok(writer.memberName("kind")) && state.ok(writer.stringValue("color4")) &&
               state.ok(writer.memberName("red")) && state.ok(writer.float64Value(color->red)) &&
               state.ok(writer.memberName("green")) &&
               state.ok(writer.float64Value(color->green)) && state.ok(writer.memberName("blue")) &&
               state.ok(writer.float64Value(color->blue)) && state.ok(writer.memberName("alpha")) &&
               state.ok(writer.float64Value(color->alpha)) && state.ok(writer.endObject());
    }
    if (const auto* text = std::get_if<std::string>(&value)) {
        return state.ok(writer.memberName("kind")) && state.ok(writer.stringValue("string")) &&
               state.ok(writer.memberName("value")) && state.ok(writer.stringValue(*text)) &&
               state.ok(writer.endObject());
    }
    if (const auto* rational = std::get_if<bloom::core::RationalTime>(&value)) {
        return state.ok(writer.memberName("kind")) && state.ok(writer.stringValue("rational")) &&
               emitNamedSigned(state, "numerator", rational->numerator()) &&
               emitNamedSigned(state, "denominator", rational->denominator()) &&
               state.ok(writer.endObject());
    }
    return false;
}

[[nodiscard]] bool emitParameter(EmitState& state, const bloom::document::ParameterRecord& record,
                                 const std::size_t parameterRank) noexcept {
    using namespace bloom::document;
    auto& writer = state.writer;
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
    if (const auto* constant = std::get_if<ConstantValueSource>(&record.source)) {
        if (!state.ok(writer.memberName("kind")) || !state.ok(writer.stringValue("constant"))) {
            return false;
        }
        if (!state.ok(writer.memberName("value"))) {
            return false;
        }
        if (!emitConstantValue(state, constant->value)) {
            {
                state.walk.fail(CanonicalDocumentError::InvalidParameter,
                                state.walk.compositionIndex, parameterRank);
                return false;
            }
        }
    } else if (const auto* curve = std::get_if<AnimationCurveSource>(&record.source)) {
        if (!state.ok(writer.memberName("kind")) ||
            !state.ok(writer.stringValue("animation-curve"))) {
            return false;
        }
        if (!emitNamedId(state, "curveId", curve->curveId.value())) {
            return false;
        }
    } else {
        // Admission rejected live driver sources before staging; reaching this path means the
        // caller bypassed canonicalDocumentSize.
        {
            state.walk.fail(CanonicalDocumentError::UnsupportedDriverBindingSource,
                            state.walk.compositionIndex, parameterRank);
            return false;
        }
    }
    return state.ok(writer.endObject()) && state.ok(writer.endObject());
}

[[nodiscard]] bool
emitInterpolation(EmitState& state,
                  const bloom::document::KeyframeInterpolation interpolation) noexcept {
    switch (interpolation) {
    case bloom::document::KeyframeInterpolation::Hold:
        return state.ok(state.writer.stringValue("hold"));
    case bloom::document::KeyframeInterpolation::Linear:
        return state.ok(state.writer.stringValue("linear"));
    }
    return false;
}

[[nodiscard]] bool emitScalarKeyframe(EmitState& state,
                                      const bloom::document::ScalarKeyframe& key) noexcept {
    auto& writer = state.writer;
    if (!state.ok(writer.beginObject())) {
        return false;
    }
    if (!emitNamedId(state, "id", key.id.value())) {
        return false;
    }
    if (!state.ok(writer.memberName("time"))) {
        return false;
    }
    if (!emitRational(state, key.time.numerator(), key.time.denominator())) {
        return false;
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
    return state.ok(writer.endObject());
}

[[nodiscard]] bool emitVec2Keyframe(EmitState& state,
                                    const bloom::document::Vec2Keyframe& key) noexcept {
    auto& writer = state.writer;
    if (!state.ok(writer.beginObject())) {
        return false;
    }
    if (!emitNamedId(state, "id", key.id.value())) {
        return false;
    }
    if (!state.ok(writer.memberName("time"))) {
        return false;
    }
    if (!emitRational(state, key.time.numerator(), key.time.denominator())) {
        return false;
    }
    if (!state.ok(writer.memberName("value")) || !state.ok(writer.beginObject())) {
        return false;
    }
    if (!state.ok(writer.memberName("x")) || !state.ok(writer.float64Value(key.value.x))) {
        return false;
    }
    if (!state.ok(writer.memberName("y")) || !state.ok(writer.float64Value(key.value.y))) {
        return false;
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
            std::span<const std::size_t> keyOrder;
            if (!makeOrder(
                    vector->keyframes, [](const Vec2Keyframe& key) noexcept { return key.time; },
                    state.sort.window2, keyOrder, state.walk.error)) {
                {
                    state.walk.fail(CanonicalDocumentError::InvalidCollectionIdentity,
                                    compositionIndex);
                    return false;
                }
            }
            for (const auto keyIndex : keyOrder) {
                if (!emitVec2Keyframe(state, vector->keyframes[keyIndex])) {
                    {
                        state.walk.fail(CanonicalDocumentError::InvalidAnimationCurve,
                                        compositionIndex);
                        return false;
                    }
                }
            }
        } else {
            {
                state.walk.fail(CanonicalDocumentError::InvalidAnimationCurve, compositionIndex);
                return false;
            }
        }
        if (!state.ok(writer.endArray()) || !state.ok(writer.endObject())) {
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
            if (!state.ok(writer.endObject())) {
                return false;
            }
        }
        if (!state.ok(writer.endArray()) || !state.ok(writer.endObject())) {
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
        if (!state.ok(writer.beginObject())) {
            return false;
        }
        if (!emitNamedId(state, "id", edge.id.value())) {
            return false;
        }
        if (!state.ok(writer.memberName("source")) || !emitOutputPortRef(state, edge.source)) {
            return false;
        }
        if (!state.ok(writer.memberName("destination")) || !state.ok(writer.beginObject())) {
            return false;
        }
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
        } else if (const auto* stackInput = std::get_if<LayerStackInputRef>(&edge.destination)) {
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
            {
                state.walk.fail(CanonicalDocumentError::InvalidGraph, compositionIndex);
                return false;
            }
        }
        if (!state.ok(writer.endObject()) || !state.ok(writer.endObject())) {
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
        if (!state.ok(writer.endObject())) {
            return false;
        }
    }
    if (!state.ok(writer.endArray())) {
        return false;
    }

    if (!state.ok(writer.memberName("layerStack")) || !state.ok(writer.beginObject())) {
        return false;
    }
    if (!emitNamedId(state, "nodeId", graph.layerStack().nodeId().value())) {
        return false;
    }
    if (!state.ok(writer.memberName("entries")) || !state.ok(writer.beginArray())) {
        return false;
    }
    for (const auto& entry : graph.layerStack().entries()) {
        if (!state.ok(writer.beginObject())) {
            return false;
        }
        if (!emitNamedId(state, "slotId", entry.slotId.value())) {
            return false;
        }
        if (!emitNamedId(state, "layerId", entry.layerId.value())) {
            return false;
        }
        if (!state.ok(writer.endObject())) {
            return false;
        }
    }
    if (!state.ok(writer.endArray()) || !state.ok(writer.endObject())) {
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
    if (!emitOutputPortRef(state, *graph.compositionOutput())) {
        return false;
    }
    return state.ok(writer.endObject());
}

[[nodiscard]] bool emitComposition(EmitState& state, const Composition& composition,
                                   const std::size_t compositionIndex) noexcept {
    auto& writer = state.writer;
    state.walk.compositionIndex = compositionIndex;
    state.walk.elementIndex = kCanonicalDocumentNoIndex;
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
    if (!emitRational(state, composition.duration().numerator(),
                      composition.duration().denominator())) {
        return false;
    }
    if (!state.ok(writer.memberName("format")) || !state.ok(writer.beginObject())) {
        return false;
    }
    const auto format = composition.format();
    if (!state.ok(writer.memberName("width")) || !state.ok(writer.integerValue(format.width()))) {
        return false;
    }
    if (!state.ok(writer.memberName("height")) || !state.ok(writer.integerValue(format.height()))) {
        return false;
    }
    if (!state.ok(writer.memberName("pixelAspect"))) {
        return false;
    }
    if (!emitRational(state, format.pixelAspect().numerator(),
                      format.pixelAspect().denominator())) {
        return false;
    }
    if (!state.ok(writer.memberName("frameRate"))) {
        return false;
    }
    if (!emitRational(state, format.frameRate().numerator(), format.frameRate().denominator())) {
        return false;
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
    if (!emitGraph(state, composition, compositionIndex)) {
        return false;
    }
    return state.ok(writer.endObject());
}

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
    if (!emitVersion(state, settings.schemaVersion)) {
        return false;
    }
    if (!state.ok(writer.memberName("processColorSpaceId")) ||
        !state.ok(writer.stringValue(settings.processColorSpaceId))) {
        return false;
    }
    if (!state.ok(writer.memberName("ocioConfig")) || !state.ok(writer.beginObject())) {
        return false;
    }
    if (!state.ok(writer.memberName("schemaVersion"))) {
        return false;
    }
    if (!emitVersion(state, settings.ocioConfig.schemaVersion)) {
        return false;
    }
    if (!state.ok(writer.memberName("locator"))) {
        return false;
    }
    if (!emitLocator(state, settings.ocioConfig.locator)) {
        return false;
    }
    if (!state.ok(writer.memberName("expectedRevision")) || !state.ok(writer.beginObject())) {
        return false;
    }
    if (!state.ok(writer.memberName("algorithm")) || !state.ok(writer.stringValue("sha256"))) {
        return false;
    }
    const auto hex = settings.ocioConfig.expectedRevision.digest.toLowercaseHex();
    if (!state.ok(writer.memberName("digest")) ||
        !state.ok(writer.stringValue(std::string_view(hex.data(), hex.size())))) {
        return false;
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
    if (!state.ok(writer.memberName("portability")) || !state.ok(writer.stringValue(portability))) {
        return false;
    }
    if (!state.ok(writer.memberName("contextVariables")) || !state.ok(writer.beginArray())) {
        return false;
    }
    for (const auto& variable : settings.ocioConfig.contextVariables) {
        if (!state.ok(writer.beginObject())) {
            return false;
        }
        if (!state.ok(writer.memberName("name")) || !state.ok(writer.stringValue(variable.name))) {
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
    if (!state.ok(writer.endArray()) || !state.ok(writer.endObject())) {
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
    if (!emitVersion(state, record.schemaVersion)) {
        return false;
    }
    if (!state.ok(writer.memberName("subject"))) {
        return false;
    }
    if (record.subject.has_value() && !emitTypedTarget(state, *record.subject)) {
        return false;
    }
    if (!record.subject.has_value() && !state.ok(writer.nullValue())) {
        return false;
    }
    if (!state.ok(writer.memberName("mediaType")) ||
        !state.ok(writer.stringValue(record.mediaType))) {
        return false;
    }
    if (!state.ok(writer.memberName("referencePolicy")) || !state.ok(writer.beginObject())) {
        return false;
    }
    if (std::holds_alternative<NoExtensionReferences>(record.referencePolicy)) {
        if (!state.ok(writer.memberName("kind")) || !state.ok(writer.stringValue("none"))) {
            return false;
        }
    } else if (const auto* table =
                   std::get_if<ExtensionHostReferenceTable>(&record.referencePolicy)) {
        if (!state.ok(writer.memberName("kind")) || !state.ok(writer.stringValue("host-table"))) {
            return false;
        }
        if (!state.ok(writer.memberName("references")) || !state.ok(writer.beginArray())) {
            return false;
        }
        for (const auto& reference : table->references) {
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
            if (!emitTypedTarget(state, reference.target)) {
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
        if (!emitVersion(state, remapper->version)) {
            return false;
        }
    } else {
        {
            state.walk.fail(CanonicalDocumentError::InvalidExtensionRecord);
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
    return state.ok(writer.endObject());
}

[[nodiscard]] bool emitHighWaterMember(EmitState& state, const std::string_view name,
                                       const std::uint64_t value) noexcept {
    return emitNamedId(state, name, value);
}

[[nodiscard]] bool emitDocumentRoot(EmitState& state) noexcept {
    using namespace bloom::document;
    auto& writer = state.writer;
    if (!state.ok(writer.beginObject())) {
        return false;
    }
    if (!state.ok(writer.memberName("schemaVersion"))) {
        return false;
    }
    if (!emitVersion(state, kV1SchemaVersion)) {
        return false;
    }

    if (!state.ok(writer.memberName("project")) || !state.ok(writer.beginObject())) {
        return false;
    }
    if (!emitNamedId(state, "id", state.project.id().value())) {
        return false;
    }
    if (!state.ok(writer.memberName("name")) ||
        !state.ok(writer.stringValue(state.project.name()))) {
        return false;
    }
    if (!state.ok(writer.memberName("colorSettings")) || !emitColorSettings(state)) {
        return false;
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
    if (!state.ok(writer.endArray()) || !state.ok(writer.endObject())) {
        return false;
    }

    if (!state.ok(writer.memberName("idAllocation")) || !state.ok(writer.beginObject())) {
        return false;
    }
    if (!state.ok(writer.memberName("highestIssued")) || !state.ok(writer.beginObject())) {
        return false;
    }
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
        !emitHighWaterMember(state, "extensionRecord", water.extensionRecord)) {
        return false;
    }
    if (!state.ok(writer.endObject()) || !state.ok(writer.endObject())) {
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
    if (!state.ok(writer.endObject())) {
        return false;
    }
    return state.ok(writer.finish());
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
    if (settings.schemaVersion != kV1SchemaVersion ||
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

    // Native v1 Save is a restricted supported-subset encoder: a live driver source is an
    // unsupported save feature reported with its exact location, never a degraded rewrite.
    for (std::size_t compositionIndex = 0; compositionIndex < project.compositions().size();
         ++compositionIndex) {
        const auto& composition = project.compositions()[compositionIndex];
        const auto& parameters = composition.parameters().records();
        for (std::size_t parameterIndex = 0; parameterIndex < parameters.size(); ++parameterIndex) {
            if (std::holds_alternative<DriverBindingSource>(parameters[parameterIndex].source)) {
                walk.fail(CanonicalDocumentError::UnsupportedDriverBindingSource, compositionIndex,
                          parameterIndex);
                return walk;
            }
        }

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
                    {}};
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
                    {}};
    if (!emitDocumentRoot(state) || writer.bytesWritten() != requiredSize) {
        std::terminate();
    }
    return CanonicalDocumentWriteResult::success(requiredSize);
}

} // namespace bloom::project
