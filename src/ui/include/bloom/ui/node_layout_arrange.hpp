#pragma once

#include <bloom/document/graph.hpp>
#include <bloom/document/node_layout.hpp>

#include <map>
#include <set>

namespace bloom::document {
class Composition;
}

namespace bloom::ui {

using NodeCardSize = document::NodeCardSize;
using NodeCardSizes = document::NodeCardSizes;
using NodePositions = std::map<document::NodeId, document::Vec2d>;

// Arranges a complete graph from sources on the left to Composition Output on the right. Card
// extents are supplied by the scene after measurement; callers that do not have a measured extent
// get the conservative document fallback. The overload with a selection arranges the induced
// subgraph and returns positions relative to its own top-left, which lets the editor preserve the
// selection's existing bounding-box origin.
[[nodiscard]] NodePositions arrangeNodes(const document::CanonicalGraph& graph,
                                         const document::NodeLayout& layout,
                                         const NodeCardSizes& sizes);

// The graph-only form is useful for canonical image links. A Composition overload additionally
// follows durable driver bindings and applies NodeGroups, which are owned beside the graph rather
// than by CanonicalGraph itself.
[[nodiscard]] NodePositions arrangeNodes(const document::Composition& composition,
                                         const NodeCardSizes& sizes);

[[nodiscard]] NodePositions arrangeNodes(const document::CanonicalGraph& graph,
                                         const document::NodeLayout& layout,
                                         const NodeCardSizes& sizes,
                                         const std::set<document::NodeId>& selection);

[[nodiscard]] NodePositions arrangeNodes(const document::Composition& composition,
                                         const NodeCardSizes& sizes,
                                         const std::set<document::NodeId>& selection);

} // namespace bloom::ui
