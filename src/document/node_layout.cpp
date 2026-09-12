#include <bloom/document/node_layout.hpp>

#include <array>
#include <cmath>
#include <string>

namespace bloom::document {

std::size_t defaultNodeLayoutColumn(const std::string_view typeId) noexcept {
    if (typeId == kLayerOutputNodeType)
        return 1;
    if (typeId == kLayerStackNodeType)
        return 2;
    if (typeId == kCompositionOutputNodeType)
        return 3;
    return 0;
}

NodeLayoutRecord defaultNodeLayoutRecord(const std::size_t column, const std::size_t row) noexcept {
    // The original four-column editor placement, frozen in document units for migration.
    return {{32.0 + static_cast<double>(column) * 256.0, 32.0 + static_cast<double>(row) * 180.0},
            128.0,
            false,
            false};
}

NodeLayout defaultNodeLayout(const std::span<const NodeRecord> nodes) {
    std::array<std::size_t, 4> rows{};
    NodeLayout layout;
    for (const auto& node : nodes) {
        const auto column = defaultNodeLayoutColumn(node.typeId);
        layout.emplace(node.id, defaultNodeLayoutRecord(column, rows.at(column)++));
    }
    return layout;
}

ValidationResult validateNodeLayout(const NodeLayout& layout, const CanonicalGraph& graph) {
    ValidationResult result;
    for (const auto& [id, record] : layout) {
        const auto path = "nodeLayout[" + std::to_string(id.value()) + "]";
        if (graph.findNode(id) == nullptr) {
            result.add(ValidationCode::MissingReference, path,
                       "Layout entry references an unknown node", ValidationSeverity::Warning);
        }
        if (!std::isfinite(record.position.x) || !std::isfinite(record.position.y) ||
            !std::isfinite(record.width) || record.width <= 0.0) {
            result.add(ValidationCode::InvalidValue, path,
                       "Node layout requires a finite position and positive finite width");
        }
    }
    return result;
}

} // namespace bloom::document
