#include "node_operation_support.hpp"
#include <algorithm>
#include <bloom/commands/layer_operations.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/core/frame_time_mapping.hpp>
#include <limits>

namespace bloom::commands {
namespace {
std::optional<core::RationalTime> snap(const core::RationalTime time,
                                       const document::Composition& composition) {
    const auto rate = composition.format().frameRate();
    const auto mapping = core::FrameTimeMapping::create(composition.duration(), rate.numerator(),
                                                        rate.denominator());
    if (!mapping)
        return std::nullopt;
    const auto index = mapping.value()->nearestFrameIndex(time);
    auto result = mapping.value()->timeForFrame(index);
    // The frame mapping clamps scrubs to the last frame. Range endpoints may also be duration.
    if (time == composition.duration())
        return time;
    if (!result)
        return std::nullopt;
    return *result.value();
}
OperationResult invalidRange() {
    return OperationResult::rejected(
        OperationIssueCode::InvalidValue,
        "Layer range must satisfy 0 <= in < out <= duration on composition frames");
}
} // namespace
std::string_view SetLayerParent::typeId() const noexcept { return "bloom.layer.set-parent"; }
OperationResult SetLayerParent::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(composition_);
    auto* layer = composition ? composition->graph().findLayer(layer_) : nullptr;
    if (!layer)
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Layer must exist in the target composition");
    if (layer->locked)
        return OperationResult::rejected(OperationIssueCode::InvalidValue, "Layer is locked");
    auto ancestor = parent_;
    std::size_t remaining = composition->graph().layerOutputs().size();
    while (ancestor) {
        if (*ancestor == layer_ || remaining-- == 0)
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Layer parenting would create a cycle");
        const auto* parent = composition->graph().findLayer(*ancestor);
        if (!parent)
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Parent must be a layer in the same composition");
        ancestor = parent->parent;
    }
    if (layer->parent == parent_)
        return OperationResult::noChange();
    const auto previous = layer->parent;
    layer->parent = parent_;
    if (const auto failure = detail::validateGraph(*composition)) {
        layer->parent = previous;
        return *failure;
    }
    return OperationResult::applied();
}
std::string_view SetLayerRange::typeId() const noexcept { return "bloom.layer.set-range"; }
OperationResult SetLayerRange::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(composition_);
    auto* layer = composition ? composition->graph().findLayer(layer_) : nullptr;
    if (!layer)
        return detail::invalidTarget();
    if (layer->locked)
        return OperationResult::rejected(OperationIssueCode::InvalidValue, "Layer is locked");
    if (in_ < core::RationalTime{} || in_ >= out_ || out_ > composition->duration())
        return invalidRange();
    const auto in = snap(in_, *composition), out = snap(out_, *composition);
    if (!in || !out || *in >= *out || *out > composition->duration())
        return invalidRange();
    const auto duration = composition->duration();
    if (layer->inPoint == *in && layer->endPoint(duration) == *out)
        return OperationResult::noChange();
    // A trim/extension moves only WHEN this layer is active; it never slips the source, because the
    // media/curve sampling is absolute composition time. So the rendered-output change is confined
    // to the symmetric difference of the old and new activity spans, half-open. A consumer of this
    // layer elsewhere in the graph sees exactly the same predicate change, so that difference is
    // the honest bound for the whole composition.
    const AffectedTimeRange oldSpan{layer->inPoint, layer->endPoint(duration)};
    const AffectedTimeRange newSpan{*in, *out};
    layer->inPoint = *in;
    layer->outPoint = *out;
    // A\B and B\A, each at most one interval for two single spans.
    std::vector<AffectedTimeRange> changed;
    const auto difference = [&changed](const AffectedTimeRange& a, const AffectedTimeRange& b) {
        if (a.end <= b.start || b.end <= a.start) {
            changed.push_back(a);
            return;
        }
        if (a.start < b.start)
            changed.push_back({a.start, std::min(a.end, b.start)});
        if (b.end < a.end)
            changed.push_back({std::max(a.start, b.end), a.end});
    };
    difference(oldSpan, newSpan);
    difference(newSpan, oldSpan);
    auto result = OperationResult::applied();
    result.affectedTimes = normalizeAffectedTimeFootprint({composition_, std::move(changed)});
    // normalize can only fail if the difference produced no interval, which cannot happen for two
    // valid non-identical spans; a nullopt here would conservatively mean whole-render.
    return result;
}
namespace {

// SPLIT-1. Proves that splitting this ordinary Layer Output produces an output-equivalent head+tail
// whose only image change is the head/tail activity boundary, so the split can publish a
// deliberately EMPTY finite footprint plus an original->tail identity remap over [split,
// originalOut).
//
// The proof is deliberately narrow. It requires the boundary node to exist and be the ordinary
// built-in Layer Output schema, the layer's outgoing edges to feed ONLY ordinary content Merge
// slots (no audio role, no non-merge node consumer), no surviving layer parented to it, and no
// surviving parameter driver-bound to the boundary node (a driver is a ParameterRecord source, NOT
// a graph edge, so it must be checked separately; CanonicalGraph::validate restricts a legal driver
// to a value-node output, so this check is defensive today but must not be dropped -- the
// classifier reasons from the record store, not from the validator). DuplicateNodes then clones the
// bound parameters and animation curves (same absolute keyframe sample times) and copies the
// external image input edge unchanged, so both halves sample the same source at the same absolute
// composition time. Anything else -- a missing/unsupported node or schema, a driver reference, an
// audio or node consumer, a child -- returns false and the split stays conservatively whole-render.
[[nodiscard]] bool splitIsOutputEquivalent(const document::Composition& composition,
                                           const document::LayerOutputBoundary& original) {
    const auto* node = composition.graph().findNode(original.nodeId);
    if (node == nullptr || node->typeId != document::kLayerOutputNodeType ||
        node->schemaVersion != document::kLayerOutputNodeSchemaVersion)
        return false;
    for (const auto& edge : composition.graph().edges()) {
        if (edge.source.nodeId != original.nodeId)
            continue;
        const auto* slot = std::get_if<document::LayerStackInputRef>(&edge.destination);
        if (slot == nullptr || slot->role != document::kLayerStackContentInputRole)
            return false;
        // A muted Layer Stack compiles only its first slot (snapshot_compiler_lowering.ipp), so
        // splitting the layer in that slot moves the tail out of the compiled plan. The head+tail
        // is then not output-equivalent, and the split must stay conservatively whole-render.
        const auto layout = composition.nodeLayout().find(slot->stackNodeId);
        if (layout != composition.nodeLayout().end() && layout->second.muted)
            return false;
    }
    for (const auto& layer : composition.graph().layerOutputs()) {
        if (layer.nodeId != original.nodeId && layer.parent.has_value() &&
            *layer.parent == original.layerId)
            return false;
    }
    for (const auto& record : composition.parameters().records()) {
        if (const auto* driver = std::get_if<document::DriverBindingSource>(&record.source)) {
            if (driver->sourceNodeId == original.nodeId)
                return false;
        }
    }
    return true;
}

} // namespace

std::string_view SplitLayerAtTime::typeId() const noexcept { return "bloom.layer.split-at-time"; }
OperationResult SplitLayerAtTime::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(composition_);
    const auto* layer = composition ? composition->graph().findLayer(layer_) : nullptr;
    if (!layer)
        return detail::invalidTarget();
    if (layer->locked)
        return OperationResult::rejected(OperationIssueCode::InvalidValue, "Layer is locked");
    const auto original = *layer;
    if (time_ <= original.inPoint || time_ >= original.endPoint(composition->duration()))
        return invalidRange();
    const auto time = snap(time_, *composition);
    if (!time || *time <= original.inPoint || *time >= original.endPoint(composition->duration()))
        return invalidRange();
    // Classify from the pre-split graph.
    const bool equivalent = splitIsOutputEquivalent(*composition, original);
    const auto originalOut = original.endPoint(composition->duration());
    auto duplicated = DuplicateNodes(composition_, {original.nodeId}, {0, 0}).apply(draft);
    if (duplicated.status != OperationStatus::Applied)
        return duplicated;
    document::LayerId copyId;
    for (const auto& output : duplicated.outputs)
        if (output.name == "layer." + std::to_string(layer_.value()))
            copyId = std::get<document::LayerId>(output.id);
    auto* copy = composition->graph().findLayer(copyId);
    if (!copy)
        return detail::invalidTarget();
    composition->graph().findLayer(layer_)->outPoint = *time;
    copy->parent = original.parent;
    copy->inPoint = *time;
    copy->outPoint = original.outPoint;
    duplicated.outputs.push_back({"layer", copyId});
    if (!equivalent)
        return duplicated; // whole-render default: no footprint, no remaps
    // The split is output-equivalent, so its changed-time footprint is deliberately EMPTY and the
    // only additional evidence is the ordered original->tail identity remap over the tail's span.
    duplicated.affectedTimes = normalizeAffectedTimeFootprint({composition_, {}});
    duplicated.layerIdentityRemaps =
        std::vector<LayerIdentityRemap>{LayerIdentityRemap{.compositionId = composition_,
                                                           .start = *time,
                                                           .end = originalOut,
                                                           .beforeLayerId = layer_,
                                                           .afterLayerId = copyId,
                                                           .beforeNodeId = original.nodeId,
                                                           .afterNodeId = copy->nodeId}};
    return duplicated;
}
std::string_view SetLayerEnabled::typeId() const noexcept { return "bloom.layer.set-enabled"; }
OperationResult SetLayerEnabled::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(composition_);
    auto* layer = composition ? composition->graph().findLayer(layer_) : nullptr;
    if (!layer)
        return detail::invalidTarget();
    auto layout = detail::layoutFor(*composition, layer->nodeId);
    if (layer->enabled == value_ && layout.muted == !value_)
        return OperationResult::noChange();
    layer->enabled = value_;
    layout.muted = !value_;
    composition->nodeLayout()[layer->nodeId] = layout;
    return OperationResult::applied();
}
std::string_view SetLayerSolo::typeId() const noexcept { return "bloom.layer.set-solo"; }
OperationResult SetLayerSolo::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(composition_);
    auto* layer = composition ? composition->graph().findLayer(layer_) : nullptr;
    if (!layer)
        return detail::invalidTarget();
    if (layer->solo == value_)
        return OperationResult::noChange();
    layer->solo = value_;
    return OperationResult::applied();
}
std::string_view SetLayerLocked::typeId() const noexcept { return "bloom.layer.set-locked"; }
OperationResult SetLayerLocked::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(composition_);
    auto* layer = composition ? composition->graph().findLayer(layer_) : nullptr;
    if (!layer)
        return detail::invalidTarget();
    if (layer->locked == value_)
        return OperationResult::noChange();
    layer->locked = value_;
    return OperationResult::applied();
}
std::string_view SetLayerLabelColor::typeId() const noexcept {
    return "bloom.layer.set-label-color";
}
OperationResult SetLayerLabelColor::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(composition_);
    auto* layer = composition ? composition->graph().findLayer(layer_) : nullptr;
    if (!layer)
        return detail::invalidTarget();
    if (layer->labelColor == color_)
        return OperationResult::noChange();
    layer->labelColor = color_;
    return OperationResult::applied();
}
std::string_view SetWorkArea::typeId() const noexcept { return "bloom.composition.set-work-area"; }
OperationResult SetWorkArea::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(composition_);
    if (!composition)
        return detail::invalidTarget();
    if (start_ < core::RationalTime{} || start_ >= end_ || end_ > composition->duration())
        return invalidRange();
    const auto start = snap(start_, *composition), end = snap(end_, *composition);
    if (!start || !end || *start >= *end)
        return invalidRange();
    const document::WorkArea area{*start, *end};
    if (composition->workArea() == area)
        return OperationResult::noChange();
    composition->setWorkArea(area);
    return OperationResult::applied();
}
std::string_view ClearWorkArea::typeId() const noexcept {
    return "bloom.composition.clear-work-area";
}
OperationResult ClearWorkArea::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(composition_);
    if (!composition)
        return detail::invalidTarget();
    if (!composition->workArea())
        return OperationResult::noChange();
    composition->setWorkArea(std::nullopt);
    return OperationResult::applied();
}
} // namespace bloom::commands
