#include <bloom/document/node_layout.hpp>

#include <bloom/document/persisted_text.hpp>

#include <array>
#include <cmath>
#include <map>
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

const NodeGroupRecord* findNodeGroupOf(const NodeGroups& groups, const NodeId node) {
    for (const auto& [id, group] : groups) {
        if (group.members.contains(node))
            return &group;
    }
    return nullptr;
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

ValidationResult validateNodeGroups(const NodeGroups& groups, const CanonicalGraph& graph) {
    ValidationResult result;
    // Which group already claimed a node, so "a node is in at most one group" is reported against
    // the second claimant rather than discovered twice.
    std::map<NodeId, NodeGroupId> owners;
    for (const auto& [id, group] : groups) {
        const auto path = "nodeGroups[" + std::to_string(id.value()) + "]";
        if (!id.isValid() || group.id != id) {
            result.add(ValidationCode::InvalidId, path + ".id",
                       "Node group ID must be nonzero and match its own key");
        }
        validateHumanFacingName(group.name, path + ".name", "Node group name", result);
        if (!std::isfinite(group.padding.x) || !std::isfinite(group.padding.y) ||
            group.padding.x < 0.0 || group.padding.y < 0.0) {
            result.add(ValidationCode::InvalidValue, path + ".padding",
                       "Node group padding requires finite nonnegative components");
        }
        for (const auto member : group.members) {
            // A missing member is a warning, exactly as an unknown-node layout entry is: a group is
            // presentation, and an unreadable module's node must not cost the artist the document.
            if (graph.findNode(member) == nullptr) {
                result.add(ValidationCode::MissingReference,
                           path + ".members[" + std::to_string(member.value()) + "]",
                           "Node group member references an unknown node",
                           ValidationSeverity::Warning);
            }
            const auto [owner, inserted] = owners.try_emplace(member, id);
            if (!inserted) {
                result.add(ValidationCode::DuplicateId,
                           path + ".members[" + std::to_string(member.value()) + "]",
                           "Node is already a member of node group " +
                               std::to_string(owner->second.value()));
            }
        }
    }
    return result;
}

} // namespace bloom::document
