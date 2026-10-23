#include "render_footprint.hpp"

#include <bloom/document/animation.hpp>

#include <algorithm>
#include <array>
#include <map>
#include <span>
#include <variant>
#include <vector>

namespace bloom::commands::detail {
namespace {

using document::RenderTimeSpan;

[[nodiscard]] bool sameGraph(const document::CanonicalGraph& left,
                             const document::CanonicalGraph& right) {
    if (!std::ranges::equal(left.nodes(), right.nodes()) ||
        !std::ranges::equal(left.edges(), right.edges()) ||
        !std::ranges::equal(left.layerOutputs(), right.layerOutputs()) ||
        left.compositionOutput() != right.compositionOutput() ||
        left.merges().size() != right.merges().size())
        return false;
    for (std::size_t index = 0; index < left.merges().size(); ++index) {
        const auto& a = left.merges()[index];
        const auto& b = right.merges()[index];
        if (a.nodeId() != b.nodeId() || a.enabled() != b.enabled() ||
            !std::ranges::equal(a.entries(), b.entries()))
            return false;
    }
    return true;
}

[[nodiscard]] bool sameMuteState(const document::NodeLayout& left,
                                 const document::NodeLayout& right) {
    const auto muted = [](const document::NodeLayout& layout) {
        std::vector<document::NodeId> result;
        for (const auto& [id, record] : layout)
            if (record.muted)
                result.push_back(id);
        return result;
    };
    return muted(left) == muted(right);
}

// The span over which one component's sampled value can differ between two key lists. A changed
// key (added, removed, moved, or with a different value, interpolation or handle) changes at most
// the two segments around it in the list that holds it, and holds before the first and after the
// last key extend to the composition's ends. Curves are local: no key's shape reads a non-adjacent
// key.
void componentChangeSpans(const std::vector<document::ScalarKeyframe>& before,
                          const std::vector<document::ScalarKeyframe>& after,
                          const core::RationalTime duration, std::vector<RenderTimeSpan>& out) {
    if (before == after)
        return;
    if (before.empty() || after.empty()) {
        out.push_back({core::RationalTime{}, duration});
        return;
    }
    const auto collect = [&](const std::vector<document::ScalarKeyframe>& keys,
                             const std::vector<document::ScalarKeyframe>& other) {
        for (std::size_t index = 0; index < keys.size(); ++index) {
            if (std::ranges::find(other, keys[index]) != other.end())
                continue;
            const auto start = index == 0 ? core::RationalTime{} : keys[index - 1].time;
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
                                    std::vector<RenderTimeSpan>& out) {
    if (before.index() != after.index())
        return false;
    return std::visit(
        [&](const auto& left) {
            using Curve = std::decay_t<decltype(left)>;
            const auto& right = std::get<Curve>(after);
            if constexpr (std::is_same_v<Curve, document::ScalarAnimationCurve>) {
                componentChangeSpans(left.keyframes, right.keyframes, duration, out);
            } else {
                for (std::size_t index = 0; index < left.components.size(); ++index)
                    componentChangeSpans(left.components[index].keyframes,
                                         right.components[index].keyframes, duration, out);
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
        duration <= core::RationalTime{} || !sameGraph(before->graph(), after->graph()) ||
        !sameMuteState(before->nodeLayout(), after->nodeLayout()))
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

    const auto beforeReach = document::RenderReachability::compute(*before, registry);
    const auto afterReach = document::RenderReachability::compute(*after, registry);
    if (!beforeReach.known() || !afterReach.known())
        return std::nullopt;
    const bool sameTopology = beforeReach.sameRenderTopology(afterReach);
    const std::vector<RenderTimeSpan> whole{{core::RationalTime{}, duration}};
    // ADR 0024: key times of a retimed layer (and of its upstream) are layer or content times, not
    // composition times. Until they are projected through the layer mapping, a key edit in a
    // composition with any retimed layer is confined to the reaching layers' whole spans.
    const bool retimed = std::ranges::any_of(
        after->graph().layerOutputs(), [&](const document::LayerOutputBoundary& layer) {
            const auto* node = after->graph().findNode(layer.nodeId);
            return !layer.timing.isIdentityMapping() ||
                   (node != nullptr &&
                    std::ranges::any_of(node->parameters, [](const auto& binding) {
                        return binding.role == document::kTimeRemapParameterRole;
                    }));
        });

    std::vector<RenderTimeSpan> changed;
    for (const auto& [id, pair] : parameters) {
        const auto [old, current] = pair;
        const auto* oldCurve =
            old ? std::get_if<document::AnimationCurveSource>(&old->source) : nullptr;
        const auto* newCurve =
            current ? std::get_if<document::AnimationCurveSource>(&current->source) : nullptr;
        std::vector<RenderTimeSpan> times;
        const bool sameRecord = old != nullptr && current != nullptr && *old == *current;
        if (oldCurve != nullptr && newCurve != nullptr && oldCurve->curveId == newCurve->curveId &&
            (sameRecord || old->schemaKey == current->schemaKey)) {
            const auto* a = before->animationCurves().find(oldCurve->curveId);
            const auto* b = after->animationCurves().find(newCurve->curveId);
            if (a == nullptr || b == nullptr || !curveChangeSpans(*a, *b, duration, times))
                times = whole;
        } else if (!sameRecord) {
            times = whole;
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
            switch (confinement.kind) {
            case document::RenderConfinementKind::Unaffected:
                break;
            case document::RenderConfinementKind::LayerSpans: {
                const auto bounded = intersect(retimed ? whole : times, confinement.spans);
                changed.insert(changed.end(), bounded.begin(), bounded.end());
                break;
            }
            case document::RenderConfinementKind::WholeComposition:
                if (retimed)
                    changed.insert(changed.end(), whole.begin(), whole.end());
                else
                    changed.insert(changed.end(), times.begin(), times.end());
                break;
            case document::RenderConfinementKind::Unproven:
                return std::nullopt;
            }
        }
    }
    return toFootprint(compositionId, std::move(changed), duration);
}

} // namespace bloom::commands::detail
