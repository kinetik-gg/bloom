#pragma once

#include <bloom/document/graph.hpp>

#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>

namespace bloom::document {

struct NodeLayoutRecord {
    Vec2d position;
    double width = 128.0;
    bool collapsed = false;
    bool muted = false;

    friend bool operator==(const NodeLayoutRecord&, const NodeLayoutRecord&) = default;
};

using NodeLayout = std::map<NodeId, NodeLayoutRecord>;

// Card geometry is deliberately a small Qt-free value. The node editor supplies measured values
// for Arrange; command-side creation uses the conservative defaults so a newly authored card never
// lands on top of an existing card before the first projection measures it.
struct NodeCardSize {
    double width = 240.0;
    double height = 180.0;

    friend bool operator==(const NodeCardSize&, const NodeCardSize&) = default;
};

using NodeCardSizes = std::map<NodeId, NodeCardSize>;

inline constexpr double kNodeLayoutSpacing = 16.0; // Spacing::L in the UI grammar.
inline constexpr NodeCardSize kConservativeNodeCardSize{240.0, 540.0};

// Returns the first deterministic free top-left position at or around `preferred`. Existing
// rectangles are indexed into x buckets, so a probe checks nearby cards rather than scanning the
// complete layout. `gap` is the minimum clear distance on every side, not merely the distance
// between card origins.
[[nodiscard]] Vec2d findNearestFreeNodePosition(const NodeLayout& layout,
                                                const NodeCardSizes& sizes, NodeCardSize newSize,
                                                Vec2d preferred, double gap = kNodeLayoutSpacing);

// The inset a group frame keeps between its members' bounding rectangle and its own border, frozen
// in document units exactly as the default card width is: the editor reads it rather than spelling
// a padding of its own, so a file authored on one build frames its members identically on another.
inline constexpr double kDefaultNodeGroupPadding = 24.0;

// One node group: a named frame drawn behind a set of member cards. This is LAYOUT, not graph
// semantics -- a group carries no ports, no encapsulation and no evaluation meaning, and the
// compiler never sees it. (`Node Group` in docs/architecture/layer-graph-model.md's terminology
// means the deferred encapsulation feature; this record is the Blender-Frame-shaped organizer the
// "Node groups" section of that document defines.)
struct NodeGroupRecord {
    NodeGroupId id;
    std::string name;
    std::set<NodeId> members;
    Vec2d padding{kDefaultNodeGroupPadding, kDefaultNodeGroupPadding};

    friend bool operator==(const NodeGroupRecord&, const NodeGroupRecord&) = default;
};

// Keyed by id so iteration -- and therefore the persisted order -- is ascending by NodeGroupId,
// exactly as NodeLayout is ordered by NodeId.
using NodeGroups = std::map<NodeGroupId, NodeGroupRecord>;

// The group a node belongs to, or nullptr. A node is a member of at most one group, so this is a
// single answer rather than a list.
[[nodiscard]] const NodeGroupRecord* findNodeGroupOf(const NodeGroups& groups, NodeId node);

[[nodiscard]] std::size_t defaultNodeLayoutColumn(std::string_view typeId) noexcept;
[[nodiscard]] NodeLayoutRecord defaultNodeLayoutRecord(std::size_t column,
                                                       std::size_t row) noexcept;
[[nodiscard]] NodeLayout defaultNodeLayout(std::span<const NodeRecord> nodes);
[[nodiscard]] ValidationResult validateNodeLayout(const NodeLayout& layout,
                                                  const CanonicalGraph& graph);
[[nodiscard]] ValidationResult validateNodeGroups(const NodeGroups& groups,
                                                  const CanonicalGraph& graph);

} // namespace bloom::document
