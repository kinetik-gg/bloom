#pragma once

#include <bloom/core/rational_time.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/node_definition_registry.hpp>

#include <cstddef>
#include <set>
#include <span>
#include <vector>

namespace bloom::document {

class Composition;
class Project;

// One half-open composition-time span [start, end).
struct RenderTimeSpan final {
    core::RationalTime start;
    core::RationalTime end;

    friend bool operator==(const RenderTimeSpan&, const RenderTimeSpan&) = default;
};

// Work counters for one reachability or confinement pass, so tests can prove each pass is linear in
// the composition it reads and never touches another composition.
struct RenderReachabilityWork final {
    std::size_t nodesVisited = 0;
    std::size_t edgesVisited = 0;
};

// The shared render-reachability answer for ONE composition: the exact node and edge set the
// snapshot compiler follows from Composition Output (and from Layer Bounds readout roots), through
// image and audio edges, layer stacks (honouring disabled merges, muted nodes, disabled, muted and
// solo-excluded layers), layer parents, and parameter drivers in both directions. It mirrors
// SnapshotCompiler::collectReachableGraph(); a node outside this set is not compiled, so editing it
// cannot change rendered pixels or audio. See docs/architecture/layer-graph-model.md,
// "Render Reachability".
//
// Fail closed: when the composition has no output, a reachable reference names a missing node, or a
// node's definition is not registered, known() is false and every caller must treat the edit as an
// unproven whole render.
class RenderReachability final {
  public:
    [[nodiscard]] static RenderReachability
    compute(const Composition& composition,
            const NodeDefinitionRegistry& registry = builtInNodeDefinitions());

    [[nodiscard]] bool known() const noexcept { return known_; }
    [[nodiscard]] bool reaches(NodeId node) const { return nodes_.contains(node); }
    [[nodiscard]] const std::set<NodeId>& nodes() const noexcept { return nodes_; }
    [[nodiscard]] const std::set<EdgeId>& edges() const noexcept { return edges_; }
    // Compositions named by reachable Composition Source nodes. A reachable source whose reference
    // is not a constant sets nestedReferenceUnknown(): it may name any composition.
    [[nodiscard]] const std::set<CompositionId>& nestedCompositions() const noexcept {
        return nested_;
    }
    [[nodiscard]] bool nestedReferenceUnknown() const noexcept { return nestedUnknown_; }
    [[nodiscard]] RenderReachabilityWork work() const noexcept { return work_; }

    // Same compiled node set, edge set and nested references. Two known reachabilities that agree
    // compile the same graph topology.
    [[nodiscard]] bool sameRenderTopology(const RenderReachability& other) const noexcept;

  private:
    bool known_ = false;
    bool nestedUnknown_ = false;
    std::set<NodeId> nodes_;
    std::set<EdgeId> edges_;
    std::set<CompositionId> nested_;
    RenderReachabilityWork work_;
};

enum class RenderConfinementKind {
    // No changed node is reachable: rendered output cannot change.
    Unaffected,
    // The change reaches output only through layer boundaries whose stack participation gates it to
    // their active spans; `spans` is their normalized union (possibly empty when every gate is
    // never active).
    LayerSpans,
    // The change reaches Composition Output without passing a gating layer boundary: it may change
    // output at any time its own inputs change.
    WholeComposition,
    // A dependency this helper cannot bound (a readout root, a dead end, an unknown graph).
    Unproven,
};

struct RenderConfinement final {
    RenderConfinementKind kind = RenderConfinementKind::Unproven;
    std::vector<RenderTimeSpan> spans;
};

// Where in composition time a change to the output or parameters of `changed` nodes can alter the
// composition's rendered output. Walks forward from the reachable changed nodes along every edge,
// parameter driver and layer-parent link. A layer boundary is a gate -- the walk records its active
// span and does not continue through its output -- only when it feeds nothing but enabled, unmuted
// layer-stack slots of its own layer and drives no parameter; its children are still followed,
// because they inherit its transform at every time they are active. Compositions are evaluated
// time-locally (a node's output at t reads its inputs at t), which is what makes the gate sound;
// nested composition time mapping is outside this proof and is handled by
// compositionsDependingOn(). Bounded by O(nodes + edges + parameter bindings).
[[nodiscard]] RenderConfinement
confineNodeChange(const Composition& composition, const RenderReachability& reachability,
                  std::span<const NodeId> changed,
                  const NodeDefinitionRegistry& registry = builtInNodeDefinitions(),
                  RenderReachabilityWork* work = nullptr);

// Every composition whose rendered output depends on `target`, through reachable Composition Source
// references, transitively; `target` itself is always included. A composition with a reachable
// non-constant reference, or whose reachability is unknown, depends on every composition.
[[nodiscard]] std::set<CompositionId>
compositionsDependingOn(const Project& project, CompositionId target,
                        const NodeDefinitionRegistry& registry = builtInNodeDefinitions());

// Sorts, clips to [0, duration), drops empty spans and merges overlapping or touching ones.
[[nodiscard]] std::vector<RenderTimeSpan>
normalizeRenderTimeSpans(std::vector<RenderTimeSpan> spans, core::RationalTime duration);

} // namespace bloom::document
