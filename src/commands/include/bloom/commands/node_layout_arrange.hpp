#pragma once

#include <bloom/document/graph.hpp>
#include <bloom/document/node_layout.hpp>

#include <bloom/commands/operation.hpp>

#include <map>
#include <optional>
#include <set>
#include <string_view>

namespace bloom::document {
class Composition;
}

namespace bloom::commands {

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

// Arrange All Nodes (no selection) or Arrange Selection in the Node editor, as one MoveNodes step.
// The editor passes the card sizes its scene measured; a script or an agent has no scene, so
// absent sizes use the conservative document card size. A selection keeps its bounding box's
// top-left, as the editor does.
class ArrangeNodes final : public Operation {
  public:
    ArrangeNodes(document::CompositionId composition, std::set<document::NodeId> selection = {},
                 NodeCardSizes sizes = {})
        : composition_(composition), selection_(std::move(selection)), sizes_(std::move(sizes)) {}
    [[nodiscard]] std::string_view typeId() const noexcept override { return "bloom.node.arrange"; }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId composition_;
    std::set<document::NodeId> selection_;
    NodeCardSizes sizes_;
};

} // namespace bloom::commands
