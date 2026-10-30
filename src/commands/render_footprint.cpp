#include "render_footprint.hpp"

#include <bloom/document/animation.hpp>
#include <bloom/document/evaluation_inputs.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <variant>
#include <vector>

namespace bloom::commands::detail {
namespace {

using document::RenderTimeSpan;

// The span over which one component's sampled value can differ between two key lists. A changed
// key (added, removed, moved, or with a different value, interpolation or handle) changes at most
// the two segments around it in the list that holds it, and holds before the first and after the
// last key extend to the composition's ends. Curves are local: no key's shape reads a non-adjacent
// key.
template <typename Keyframe>
void componentChangeSpans(const std::vector<Keyframe>& before, const std::vector<Keyframe>& after,
                          const core::RationalTime duration, std::vector<RenderTimeSpan>& out,
                          const core::RationalTime low = {}) {
    if (before == after)
        return;
    if (before.empty() || after.empty()) {
        out.push_back({low, duration});
        return;
    }
    const auto collect = [&](const std::vector<Keyframe>& keys,
                             const std::vector<Keyframe>& other) {
        for (std::size_t index = 0; index < keys.size(); ++index) {
            if (std::ranges::find(other, keys[index]) != other.end())
                continue;
            const auto start = index == 0 ? low : keys[index - 1].time;
            const auto end = index + 1 == keys.size() ? duration : keys[index + 1].time;
            // A neighbour outside [0, duration) still bounds correctly once clipped; a key whose
            // neighbours are ordered after the end collapses to nothing and is dropped there.
            out.push_back({std::min(start, keys[index].time), std::max(end, keys[index].time)});
        }
    };
    collect(before, after);
    collect(after, before);
}

// std::nullopt when the two records are not the same curve shape (whole duration then applies).
[[nodiscard]] bool curveChangeSpans(const document::AnimationCurveRecord& before,
                                    const document::AnimationCurveRecord& after,
                                    const core::RationalTime duration,
                                    std::vector<RenderTimeSpan>& out,
                                    const core::RationalTime low = {}) {
    if (before.index() != after.index())
        return false;
    return std::visit(
        [&](const auto& left) {
            using Curve = std::decay_t<decltype(left)>;
            const auto& right = std::get<Curve>(after);
            if constexpr (document::kWholeValueAnimationCurve<Curve>) {
                componentChangeSpans(left.keyframes, right.keyframes, duration, out, low);
            } else {
                for (std::size_t index = 0; index < left.components.size(); ++index)
                    componentChangeSpans(left.components[index].keyframes,
                                         right.components[index].keyframes, duration, out, low);
            }
            return true;
        },
        before);
}

[[nodiscard]] std::vector<RenderTimeSpan> intersect(const std::vector<RenderTimeSpan>& left,
                                                    const std::vector<RenderTimeSpan>& right) {
    std::vector<RenderTimeSpan> result;
    for (const auto& a : left)
        for (const auto& b : right) {
            const auto start = std::max(a.start, b.start);
            const auto end = std::min(a.end, b.end);
            if (start < end)
                result.push_back({start, end});
        }
    return result;
}

// ADR 0024 layer-owned time. Key times of a retimed layer's own parameters are layer time, and of
// its upstream content time. A retimed layer is "projectable" when its content time equals its
// layer time wherever content shows: no Time Remap, and an unbounded extent or None out-of-extent
// modes.
struct RetimedLayer final {
    const document::LayerOutputBoundary* boundary = nullptr;
    bool topLevel = false;
    bool projectable = false;
    std::set<document::NodeId> upstream; // every node feeding its image or audio input
};

[[nodiscard]] bool hasTimeRemap(const document::Composition& composition,
                                const document::LayerOutputBoundary& boundary) {
    const auto* node = composition.graph().findNode(boundary.nodeId);
    return node != nullptr && std::ranges::any_of(node->parameters, [](const auto& binding) {
               return binding.role == document::kTimeRemapParameterRole;
           });
}

[[nodiscard]] std::vector<RetimedLayer> retimedLayers(const document::Composition& composition) {
    const auto& graph = composition.graph();
    std::vector<RetimedLayer> result;
    for (const auto& boundary : graph.layerOutputs()) {
        const bool remap = hasTimeRemap(composition, boundary);
        if (boundary.timing.isIdentityMapping() && !remap)
            continue;
        RetimedLayer layer{
            .boundary = &boundary, .topLevel = false, .projectable = false, .upstream = {}};
        const bool unbounded =
            std::holds_alternative<std::monostate>(boundary.timing.contentExtent);
        layer.projectable =
            !remap && (unbounded || (boundary.timing.beforeMode == document::LayerTimeMode::None &&
                                     boundary.timing.afterMode == document::LayerTimeMode::None));
        // Top level: a slot of the Merge feeding Output (its range is composition time).
        layer.topLevel = std::ranges::any_of(graph.layerStack().entries(), [&](const auto& entry) {
            return entry.layerId == boundary.layerId;
        });
        // Backward closure over input edges and driver bindings, starting at the layer's inputs.
        std::vector<document::NodeId> pending;
        const auto visitInputs = [&](const document::NodeId node) {
            for (const auto& edge : graph.edges()) {
                const auto* input = std::get_if<document::NodeInputRef>(&edge.destination);
                const auto* slot = std::get_if<document::LayerStackInputRef>(&edge.destination);
                if ((input && input->nodeId == node) || (slot && slot->stackNodeId == node))
                    pending.push_back(edge.source.nodeId);
            }
            if (const auto* record = graph.findNode(node))
                for (const auto& binding : record->parameters)
                    if (const auto* parameter = composition.parameters().find(binding.parameterId))
                        if (const auto* driver =
                                std::get_if<document::DriverBindingSource>(&parameter->source))
                            pending.push_back(driver->sourceNodeId);
        };
        visitInputs(boundary.nodeId);
        while (!pending.empty()) {
            const auto node = pending.back();
            pending.pop_back();
            if (layer.upstream.insert(node).second)
                visitInputs(node);
        }
        result.push_back(std::move(layer));
    }
    return result;
}

// A layer-clock time as placement time: start + tau * den / num, or nullopt on overflow.
[[nodiscard]] std::optional<core::RationalTime> placementOf(const document::LayerTiming& timing,
                                                            const core::RationalTime tau) {
    const auto multiply = [](const std::int64_t a,
                             const std::int64_t b) -> std::optional<std::int64_t> {
        constexpr auto limit = std::numeric_limits<std::int64_t>::max();
        if (a == std::numeric_limits<std::int64_t>::min() ||
            b == std::numeric_limits<std::int64_t>::min())
            return std::nullopt;
        const auto magnitude = a < 0 ? -a : a;
        if (magnitude != 0 && (b > limit / magnitude || b < -(limit / magnitude)))
            return std::nullopt;
        return a * b;
    };
    const auto n = multiply(tau.numerator(), timing.speed.denominator());
    const auto d = multiply(tau.denominator(), timing.speed.numerator());
    const auto& start = timing.startTime;
    const auto left = n && d ? multiply(start.numerator(), *d) : std::nullopt;
    const auto right = n ? multiply(*n, start.denominator()) : std::nullopt;
    const auto denominator = d ? multiply(start.denominator(), *d) : std::nullopt;
    if (!left || !right || !denominator ||
        (*right > 0 && *left > std::numeric_limits<std::int64_t>::max() - *right) ||
        (*right < 0 && *left < std::numeric_limits<std::int64_t>::min() - *right))
        return std::nullopt;
    return core::RationalTime::create(*left + *right, *denominator);
}

// Converts normalized spans to a footprint. Past the interval cap the union collapses to its hull,
// which is still a finite, composition-scoped superset.
[[nodiscard]] std::optional<AffectedTimeFootprint>
toFootprint(const document::CompositionId compositionId, std::vector<RenderTimeSpan> spans,
            const core::RationalTime duration) {
    spans = document::normalizeRenderTimeSpans(std::move(spans), duration);
    if (spans.size() > kMaxAffectedTimeIntervals)
        spans = {{spans.front().start, spans.back().end}};
    AffectedTimeFootprint footprint{.compositionId = compositionId, .intervals = {}};
    for (const auto& span : spans)
        footprint.intervals.push_back({span.start, span.end});
    return normalizeAffectedTimeFootprint(std::move(footprint));
}

} // namespace

std::optional<AffectedTimeFootprint> unreachableEditFootprint(
    const document::Composition& after, const document::RenderReachability& before,
    const std::set<document::NodeId>& touched, const document::NodeDefinitionRegistry& registry) {
    if (!before.known())
        return std::nullopt;
    const auto now = document::RenderReachability::compute(after, registry);
    if (!before.sameRenderTopology(now))
        return std::nullopt;
    for (const auto id : touched)
        if (before.reaches(id) || now.reaches(id))
            return std::nullopt;
    return AffectedTimeFootprint{.compositionId = after.id(), .intervals = {}};
}

std::optional<AffectedTimeFootprint> parameterEditFootprint(
    const document::Project& beforeProject, const document::Project& afterProject,
    const document::CompositionId compositionId, const document::NodeDefinitionRegistry& registry) {
    const auto* before = beforeProject.findComposition(compositionId);
    const auto* after = afterProject.findComposition(compositionId);
    if (before == nullptr || after == nullptr)
        return std::nullopt;
    if (before->contentIdentity() == after->contentIdentity())
        return AffectedTimeFootprint{.compositionId = compositionId, .intervals = {}};
    const auto duration = after->duration();
    if (before->duration() != duration || before->format() != after->format() ||
        duration <= core::RationalTime{} ||
        !document::sameEvaluationGraph(before->graph(), after->graph()) ||
        !document::sameEvaluationLayout(before->nodeLayout(), after->nodeLayout()))
        return std::nullopt;

    std::map<document::ParameterId,
             std::pair<const document::ParameterRecord*, const document::ParameterRecord*>>
        parameters;
    for (const auto& record : before->parameters().records())
        parameters[record.id].first = &record;
    for (const auto& record : after->parameters().records())
        parameters[record.id].second = &record;
    std::map<document::ParameterId, std::vector<document::NodeId>> owners;
    for (const auto& node : after->graph().nodes())
        for (const auto& binding : node.parameters)
            owners[binding.parameterId].push_back(node.id);

    const auto beforeReachShared = document::RenderReachability::of(*before, registry);
    const auto& beforeReach = *beforeReachShared;
    const auto afterReach = document::RenderReachability::compute(*after, registry);
    if (!beforeReach.known() || !afterReach.known())
        return std::nullopt;
    const bool sameTopology = beforeReach.sameRenderTopology(afterReach);
    const std::vector<RenderTimeSpan> whole{{core::RationalTime{}, duration}};
    // ADR 0024: key times of a retimed layer's parameters are layer times and of its upstream
    // content times. They are projected onto composition time through the layer's linear clock
    // where content time equals layer time; otherwise the layer's whole span is used.
    const auto retimed = retimedLayers(*after);
    // Nodes that reach Composition Output without passing a retimed layer (their keys are
    // composition times).
    std::set<document::NodeId> plain;
    if (!retimed.empty()) {
        std::vector<document::NodeId> pending{after->graph().compositionOutput()->nodeId};
        while (!pending.empty()) {
            const auto node = pending.back();
            pending.pop_back();
            if (std::ranges::any_of(
                    retimed, [&](const auto& layer) { return layer.boundary->nodeId == node; }))
                continue;
            if (!plain.insert(node).second)
                continue;
            for (const auto& edge : after->graph().edges()) {
                const auto* input = std::get_if<document::NodeInputRef>(&edge.destination);
                const auto* slot = std::get_if<document::LayerStackInputRef>(&edge.destination);
                if ((input && input->nodeId == node) || (slot && slot->stackNodeId == node))
                    pending.push_back(edge.source.nodeId);
            }
            if (const auto* record = after->graph().findNode(node))
                for (const auto& binding : record->parameters)
                    if (const auto* parameter = after->parameters().find(binding.parameterId))
                        if (const auto* driver =
                                std::get_if<document::DriverBindingSource>(&parameter->source))
                            pending.push_back(driver->sourceNodeId);
        }
    }
    // Layer-clock spans are computed unbounded on both sides.
    const auto far = core::RationalTime::fromInteger(std::int64_t{1} << 40);
    const auto nearFar = core::RationalTime::fromInteger(-(std::int64_t{1} << 40));

    std::vector<RenderTimeSpan> changed;
    for (const auto& [id, pair] : parameters) {
        const auto [old, current] = pair;
        const auto* oldCurve =
            old ? std::get_if<document::AnimationCurveSource>(&old->source) : nullptr;
        const auto* newCurve =
            current ? std::get_if<document::AnimationCurveSource>(&current->source) : nullptr;
        std::vector<RenderTimeSpan> times;
        // The same change in an unbounded layer/content clock; nullopt means "everywhere".
        std::optional<std::vector<RenderTimeSpan>> clockTimes;
        const bool sameRecord = old != nullptr && current != nullptr && *old == *current;
        if (oldCurve != nullptr && newCurve != nullptr && oldCurve->curveId == newCurve->curveId &&
            (sameRecord || old->schemaKey == current->schemaKey)) {
            const auto* a = before->animationCurves().find(oldCurve->curveId);
            const auto* b = after->animationCurves().find(newCurve->curveId);
            if (a == nullptr || b == nullptr || !curveChangeSpans(*a, *b, duration, times))
                times = whole;
            std::vector<RenderTimeSpan> unbounded;
            if (a != nullptr && b != nullptr && curveChangeSpans(*a, *b, far, unbounded, nearFar))
                clockTimes = std::move(unbounded);
        } else if (!sameRecord) {
            times = whole;
        } else {
            clockTimes = std::vector<RenderTimeSpan>{};
        }
        if (times.empty())
            continue;
        const auto found = owners.find(id);
        if (found == owners.end())
            continue; // An unbound parameter is never read.
        const bool driver =
            (old && std::holds_alternative<document::DriverBindingSource>(old->source)) ||
            (current && std::holds_alternative<document::DriverBindingSource>(current->source));
        for (const auto owner : found->second) {
            if (driver || !sameTopology) {
                // A driver binding changes what is compiled; only an owner that stays unreachable
                // under an identical compiled topology is provably neutral.
                if (!sameTopology || beforeReach.reaches(owner) || afterReach.reaches(owner))
                    return std::nullopt;
                continue;
            }
            const std::array node{owner};
            const auto confinement =
                document::confineNodeChange(*after, afterReach, node, registry);
            if (confinement.kind == document::RenderConfinementKind::Unproven)
                return std::nullopt;
            if (confinement.kind == document::RenderConfinementKind::Unaffected)
                continue;
            // Retimed layers whose clock reads this owner's keys.
            bool onlyRetimed = !retimed.empty() && !plain.contains(owner);
            for (const auto& layer : retimed) {
                const bool own = layer.boundary->nodeId == owner;
                if (!own && !layer.upstream.contains(owner))
                    continue;
                const RenderTimeSpan span{layer.boundary->inPoint,
                                          layer.boundary->endPoint(duration)};
                // A nested retimed layer's range is in its enclosing clock: stay conservative.
                if (!layer.topLevel) {
                    onlyRetimed = false;
                    changed.insert(changed.end(), whole.begin(), whole.end());
                    continue;
                }
                if (!clockTimes || (!own && !layer.projectable)) {
                    changed.push_back(span);
                    continue;
                }
                for (const auto& [a, b] : *clockTimes) {
                    const auto from = a <= nearFar ? std::optional{span.start}
                                                   : placementOf(layer.boundary->timing, a);
                    const auto to =
                        b >= far ? std::optional{span.end} : placementOf(layer.boundary->timing, b);
                    if (!from || !to) {
                        changed.push_back(span);
                        break;
                    }
                    const auto start = std::max(*from, span.start);
                    const auto end = std::min(*to, span.end);
                    if (start < end)
                        changed.push_back({start, end});
                }
            }
            if (onlyRetimed)
                continue;
            switch (confinement.kind) {
            case document::RenderConfinementKind::LayerSpans: {
                const auto bounded = intersect(times, confinement.spans);
                changed.insert(changed.end(), bounded.begin(), bounded.end());
                break;
            }
            case document::RenderConfinementKind::WholeComposition:
                changed.insert(changed.end(), times.begin(), times.end());
                break;
            default:
                break;
            }
        }
    }
    return toFootprint(compositionId, std::move(changed), duration);
}

} // namespace bloom::commands::detail
