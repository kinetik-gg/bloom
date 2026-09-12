#pragma once

#include <bloom/document/graph.hpp>

#include <map>
#include <span>
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

[[nodiscard]] std::size_t defaultNodeLayoutColumn(std::string_view typeId) noexcept;
[[nodiscard]] NodeLayoutRecord defaultNodeLayoutRecord(std::size_t column,
                                                       std::size_t row) noexcept;
[[nodiscard]] NodeLayout defaultNodeLayout(std::span<const NodeRecord> nodes);
[[nodiscard]] ValidationResult validateNodeLayout(const NodeLayout& layout,
                                                  const CanonicalGraph& graph);

} // namespace bloom::document
