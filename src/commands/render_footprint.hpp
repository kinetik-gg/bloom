#pragma once

#include <bloom/commands/result.hpp>
#include <bloom/document/project.hpp>
#include <bloom/document/render_reachability.hpp>

#include <optional>
#include <set>

namespace bloom::commands::detail {

// Structural proof for node add/remove/connect/disconnect/dissolve (issue #355). `before` is the
// composition's reachability captured BEFORE the operation mutated anything; `touched` names every
// node whose record, inputs, slots or parameters the operation changed or erased, plus layer
// parents whose transform-parent status may change. The edit is render-neutral -- a deliberate
// empty footprint -- only when both reachabilities are known, no touched node is reachable before
// or after, and the compiled topology (nodes, edges, nested references) is identical. Anything
// else is std::nullopt, the conservative whole render.
[[nodiscard]] std::optional<AffectedTimeFootprint> unreachableEditFootprint(
    const document::Composition& after, const document::RenderReachability& before,
    const std::set<document::NodeId>& touched, const document::NodeDefinitionRegistry& registry);

// Parameter/animation proof for a transaction whose render-affecting operations only changed
// parameter sources, curves and keyframes of `compositionId`. Diffs the composition's parameter
// records and curves between `before` and `after` once; each changed parameter contributes the
// time its value function changed (a keyframe edit: the interval between the edited key's
// neighbours; anything else: the whole duration) intersected with where its owning node can reach
// output (render_reachability.hpp). A changed driver binding is topology and contributes nothing
// only when its owner stays unreachable and the compiled topology is unchanged. std::nullopt when
// the graph, layout mute state, duration or format changed, or anything is unproven.
[[nodiscard]] std::optional<AffectedTimeFootprint> parameterEditFootprint(
    const document::Project& before, const document::Project& after,
    document::CompositionId compositionId,
    const document::NodeDefinitionRegistry& registry = document::builtInNodeDefinitions());

} // namespace bloom::commands::detail
